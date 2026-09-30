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
.build/gcc/tirage-integration   # the real worker, on images built with libvips
```

`-DTIRAGE_BUILD_TOOLS=OFF` builds only the header-only library and its unit tests, without libvips.

## Direct mode

Until the daemon exists, the CLI launches the worker itself:

```sh
export TIRAGE_DIRECT=1 TIRAGE_WORKER=$PWD/.build/gcc/tirage-worker TIRAGE_THREADS=3
.build/gcc/tirage probe --profile profile.json photo.jpg
.build/gcc/tirage encode --profile profile.json --crop 0,0,900,1000 --crop-unit permille out/ photo.jpg
```

`encode` writes `<variant>-<width>.<ext>` into the output directory and prints a JSON report. A
refusal is that report with a non-empty `error` and exit code 0. A failure exits non-zero.

## License

MIT, see [LICENSE](LICENSE).
