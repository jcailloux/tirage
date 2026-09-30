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

`-DTIRAGE_BUILD_TOOLS=OFF` builds only the header-only library and its unit tests, without libvips.

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

Protocol: one request per connection, in frames of a 4-byte little-endian length then BEVE. The daemon
answers `queued` (with the number of encodes ahead) and `started` for an encode, then one final message:
a response (success, or refusal with its code), `busy` (retry later: queue full, deadline passed,
daemon stopping) or a failure. Hanging up cancels the request.

## Direct mode

The CLI does not talk to the daemon yet (phase 3). Meanwhile it launches the worker itself:

```sh
export TIRAGE_DIRECT=1 TIRAGE_WORKER=$PWD/.build/gcc/tirage-worker TIRAGE_THREADS=3
.build/gcc/tirage probe --profile profile.json photo.jpg
.build/gcc/tirage encode --profile profile.json --crop 0,0,900,1000 --crop-unit permille out/ photo.jpg
```

`encode` writes `<variant>-<width>.<ext>` into the output directory and prints a JSON report. A
refusal is that report with a non-empty `error` and exit code 0. A failure exits non-zero.

## License

MIT, see [LICENSE](LICENSE).
