# tirage

Machine-wide image encoding for the websites sharing a server: one daemon, one queue, one CPU budget,
and a disposable worker process per encode.

Work in progress. The design is in [`plans/2026-09-30-conception.md`](plans/2026-09-30-conception.md).

## Build and test

```sh
cmake -S . -B .build/gcc -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build .build/gcc -j2
.build/gcc/tirage-tests
```

## License

MIT, see [LICENSE](LICENSE).
