# Using tirage from your project

This page takes a project from nothing to its first encode: install the service, let your site
reach it, link the client, send a request and handle every answer. The [profile](profile.md) and
the [requests](requests.md) are described on their own pages.

## What you get, what you do not

tirage encodes. Your project sends an original and its profile, and gets back the encoded files in
memory. tirage stores nothing, keeps no profile, knows nothing of your users and names no file.

In exchange, the machine gets one queue and one CPU budget for every site on it, and libvips runs in
a throwaway process per encode, never in your service. A crash or a leak in a decoder costs one
worker, not your back end.

## 1. Install the service

On Debian 13 (trixie), from a release package:

```sh
sudo apt install ./tirage_0.3.1_amd64.deb
```

This installs the daemon, the `tirage` command, the worker, and starts `tirage.socket`
(`/run/tirage/tirage.sock`, open to the `tirage` group). The daemon starts on the first connection.
The first install writes `/etc/tirage/tirage.json` with half of the CPUs per encode. See the
[README](../README.md#daemon) for every setting, and how to build the package or install elsewhere.

Check it:

```sh
tirage --version
tirage status
```

## 2. Let your site reach the socket

The socket is open to the `tirage` group only. Add it to your service's unit:

```ini
[Service]
SupplementaryGroups=tirage
```

That is all, even under a strict sandbox: the callers of the test suite run with
`ProtectSystem=strict`, `PrivateTmp=yes` and `PrivateNetwork=yes`. If your unit restricts address
families, keep `AF_UNIX`.

The daemon identifies callers by their user (`SO_PEERCRED`), for its journal and `tirage status`
only. There is nothing to declare on tirage's side.

## 3. Link the client

The client is header-only and needs only [glaze](https://github.com/stephenberry/glaze) (fetched
for you), never libvips. With CMake:

```cmake
include(FetchContent)
FetchContent_Declare(
    tirage
    GIT_REPOSITORY https://github.com/jcailloux/tirage.git
    GIT_TAG        v0.3.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(tirage)

target_link_libraries(my-site PRIVATE tirage::client)
```

As a dependency, tirage builds neither its tools nor its tests (`TIRAGE_BUILD_TOOLS` and
`TIRAGE_BUILD_TESTS` are off unless it is the top-level project). It needs C++23: GCC 14 or a
recent clang.

`tirage::profile` is the same headers without the socket code (profile, protocol, validation), for
a part of your project that only builds or checks profiles.

### Versions

Pin a tag, and install a daemon **at least as recent** as that tag.

- A newer daemon understands an older client: it only adds fields, and the client skips the ones
  it does not know (from 0.3.1).
- An older daemon refuses a newer client's request when it carries a field it does not know (a
  `Failure`, "unreadable request"). That is on purpose: a daemon that dropped a mask in silence
  would publish what the mask should hide.

`tirage --version` gives the installed version. `protocol` and the profile's `version` change only
for a change that cannot be made by adding fields. A request of another protocol version is
refused with a message that says so.

## 4. Send a request

[`examples/encode.cpp`](../examples/encode.cpp) is a complete caller, built and run by the test
suite against the real daemon. Its core:

```cpp
#include "tirage/client.h"

auto profile = tirage::parse_profile(profile_json);  // or build a tirage::Profile in code
if (!profile) { /* profile.error().path, profile.error().message */ }

tirage::Request request{
    .protocol = tirage::kProtocolVersion,
    .operation = tirage::Operation::encode,
    .priority = tirage::Priority::interactive,
    .profile = std::move(*profile),
    .variants = {"web", "thumb"},
    .deadline_ms = 30'000,
    .input = std::move(image_bytes),
};

auto reply = tirage::client::call(request);
```

`call` is **synchronous**: it connects, sends, waits for the final message and returns. An encode
takes seconds, and may wait behind others: call it from a thread or a job queue of your own, never
from a thread that serves requests.

Build the profile once (at start-up, from a constant or a file of your repository), not on each
request: it is plain data, and the same profile serves every request.

## 5. Handle every answer

```cpp
std::stop_source stop;  // stop.request_stop() from another thread cancels the call
auto reply = tirage::client::call(request, {
    .on_event = [](const tirage::client::Event& e) { /* Queued{position}, then Started{waited_ms} */ },
    .stop = stop.get_token(),
});

if (!reply) {
    // Transport: daemon unreachable, connection cut, or tirage::client::kCancelled.
} else if (auto* r = std::get_if<tirage::Response>(&*reply)) {
    if (r->error.empty()) { /* r->outputs: store them */ }
    else { /* refusal: map r->code to a message for the user */ }
} else if (auto* b = std::get_if<tirage::Busy>(&*reply)) {
    // Not run: send the same request again later.
} else {
    // tirage::Failure: log std::get<tirage::Failure>(*reply).message.
}
```

What each branch means for your users:

- **Refusal** (`Response` with an `error`): the image or the request is at fault. Map `code` to a
  message of your own ("this file is not an image", "the image is too small"…). `error` is for your
  logs, in English, not for users. Sending the same request again gives the same refusal.
- **Busy**: the machine is saturated, the job did not run. Retry after a pause, or tell the user to
  try again in a moment. Do not retry in a tight loop.
- **Failure** and **transport errors**: something broke, not the image's fault. Log, and show a
  generic error.
- **Cancelled**: you asked for it, with `Options::stop`. A cancelled job leaves the queue, or its
  worker is killed: hang up when the user who waited for it is gone.

`on_event` runs on the calling thread before `call` returns. `Queued{position}` lets you show
"waiting, 2 ahead", and `Started` that encoding has begun.

## 6. Choose the priority and the deadline

- `interactive` for anything a person waits for, `background` for batch work. Batch work that is
  sent `interactive` delays every person waiting behind it.
- `deadline_ms` bounds the **wait in the queue**, not the encode: past it, a job still queued is
  dropped with `Busy` (reason `deadline`). Set it when a late result is useless, for example a bit
  under your HTTP timeout. Once started, an encode runs to its end, or to the daemon's
  `timeout_s`.

A batch of hundreds of images can be sent all at once: each request waits its turn. Mind the
`background` queue length (256 by default), past which requests are `Busy`.

## 7. Develop and test without the daemon

On a development machine, direct mode runs the worker in your process's place, without a daemon,
queue or budget:

```sh
export TIRAGE_DIRECT=1
export TIRAGE_WORKER=/path/to/tirage-worker   # else tirage-worker in the PATH
export TIRAGE_THREADS=3                       # else 1
```

The same `call` then gives the same events and answers, with the daemon's default bounds. It needs
libvips on that machine (to build the worker), never in production, where it would go around the
machine's budget.

To reach a daemon on another socket, for example one started by your own tests, set
`TIRAGE_SOCKET` or `Options::socket`.

## 8. Look at what runs

```sh
tirage status          # budget, the running encode, the queues, by caller
tirage status --json   # the same, for a script
journalctl -u tirage   # one line per request: caller, operation, sizes, wait, result
```

From code, `tirage::client::status()` returns the same `tirage::Status`.

## Checklist

- [ ] The package is installed, `tirage status` answers.
- [ ] Your service unit has `SupplementaryGroups=tirage`.
- [ ] The client is pinned to a tag, and the daemon is at least that version.
- [ ] The profile lives in your repository, and validates (`tirage::parse_profile`, or a test).
- [ ] `call` runs off your request-serving threads.
- [ ] Refusals map to user messages by `code`, `Busy` is retried or shown as "try again", failures
      are logged.
- [ ] Batch work is sent `background`.
