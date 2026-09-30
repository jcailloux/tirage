# tirage

Machine-wide image encoding for the websites sharing a server: one daemon, one queue, one CPU budget,
and a disposable worker process per encode.

Work in progress. The design is in [`plans/2026-09-30-conception.md`](plans/2026-09-30-conception.md).

## Build and test

Needs libvips >= 8.16 with its C++ binding (`libvips-dev` on Debian, `vips-devel` on Fedora) for
`tirage-worker`. Everything else is fetched by CMake.

```sh
cmake -S . -B .build/gcc -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build .build/gcc -j2
.build/gcc/tirage-tests         # pure logic
.build/gcc/tirage-integration   # the real worker and the real daemon, on images built with libvips
```

The leak non-regression test (500 AVIF encodes through the daemon, plan § 9) is skipped by default.
Run it in `debian:trixie-slim`, whose libheif leaks:
`tirage-integration -tc='*memory stays flat*' --no-skip` (`TIRAGE_LEAK_COUNT` changes the count).

`-DTIRAGE_BUILD_TOOLS=OFF` builds only the header-only libraries and their unit tests, without libvips.

The hardening checks (plan § 6) run the daemon, its workers and a caller under the sandboxing of
their systemd units, as transient units of your user manager: `tests/hardening/check.sh .build/gcc`.

## Daemon

```sh
.build/gcc/tiraged --config tirage.json --socket /tmp/tirage.sock
```

`tiraged` takes the socket from systemd when activated, else binds `--socket`, else the configuration's
`socket` (`/run/tirage/tirage.sock`). The configuration defaults to `/etc/tirage/tirage.json`, where
every key is optional and an unknown key is refused:

```json
{
  "threads": 3,
  "worker": "/usr/lib/tirage/tirage-worker",
  "queue": { "interactive": 32, "background": 256 },
  "probe_limit": 2,
  "timeout_s": 120,
  "max_pending_bytes": 536870912,
  "bounds": { "max_bytes": 67108864, "max_edge": 16384, "max_pixels": 100000000 }
}
```

`SIGHUP` reloads it for the requests that follow. `SIGTERM` answers the queued requests `busy` and
stops once the running worker is done. One line per request goes to the standard error (the journal).
`tirage status` shows the budget, what runs and what waits, by priority and by caller (`--json` for
the whole of it).

Protocol: one request per connection, in frames of a 4-byte little-endian length then BEVE. The daemon
answers `queued` (with the number of encodes ahead) and `started` for an encode, then one final message:
a response (success, or refusal with its code), `busy` (retry later: queue full, deadline passed,
daemon stopping) or a failure. Hanging up cancels the request.

## CLI

```sh
export TIRAGE_SOCKET=/tmp/tirage.sock   # else /run/tirage/tirage.sock
.build/gcc/tirage probe --profile profile.json photo.jpg
.build/gcc/tirage encode --profile profile.json --crop 0,0,900,1000 --crop-unit permille out/ photo.jpg
.build/gcc/tirage status
```

`encode` writes `<variant>-<width>.<ext>` into the output directory and prints a JSON report. A
refusal is that report with a non-empty `error` and exit code 0. Busy exits 75 (try again later), a
failure exits 1, bad arguments exit 2.

Direct mode, for development only, launches the worker without a daemon (and without its budget):

```sh
export TIRAGE_DIRECT=1 TIRAGE_WORKER=$PWD/.build/gcc/tirage-worker TIRAGE_THREADS=3
```

## C++ client

`libtirage-client` is header-only (`#include "tirage/client.h"`, CMake target `tirage::client`, glaze
only). A call is synchronous: make it from a thread of your own.

```cpp
std::stop_source stop;  // request_stop() hangs up, which cancels the job
auto reply = tirage::client::call(request, {
    .on_event = [](const tirage::client::Event& e) { /* Queued{position}, then Started{waited_ms} */ },
    .stop = stop.get_token(),
});
if (!reply) { /* transport: daemon unreachable, connection cut, or tirage::client::kCancelled */ }
else if (auto* r = std::get_if<tirage::Response>(&*reply)) { /* success, or refusal: r->error, r->code */ }
else if (auto* b = std::get_if<tirage::Busy>(&*reply)) { /* send it again later */ }
else { /* tirage::Failure: log it */ }
```

The same call honours `TIRAGE_SOCKET` and `TIRAGE_DIRECT`. `tirage::client::status()` returns what
`tirage status` shows.

## License

MIT, see [LICENSE](LICENSE).
