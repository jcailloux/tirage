#!/bin/sh
# Builds the Debian package (plan § 8) in a debian:trixie-slim container, after
# the unit and integration tests pass there: a red suite never makes a package.
#
# Usage: packaging/build.sh
# Output: .build/deb/tirage_<version>_amd64.deb
#
# TIRAGE_JOBS sets the build's parallelism (2 by default: GCC and RAM).

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)

# podman first (rootless on Fedora: the files written belong to you), docker
# with --user otherwise.
if command -v podman >/dev/null 2>&1; then
    engine=podman
    user=""
elif command -v docker >/dev/null 2>&1; then
    engine=docker
    user="--user $(id -u):$(id -g)"
else
    echo "neither podman nor docker found" >&2
    exit 1
fi

"$engine" build -t tirage-builder -f "$root/packaging/Containerfile" "$root/packaging"

# The build directory stays on the host, FetchContent's cache with it. HOME is
# for git, under --user.
# shellcheck disable=SC2086
"$engine" run --rm $user -e HOME=/tmp -e JOBS="${TIRAGE_JOBS:-2}" \
    -v "$root":/src:z -w /src tirage-builder \
    sh -c 'set -e
           cmake -S . -B .build/deb -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
           cmake --build .build/deb -j"$JOBS"
           ctest --test-dir .build/deb --output-on-failure
           cd .build/deb && rm -f tirage_*.deb && cpack -G DEB
           dpkg-deb --field tirage_*.deb
           dpkg-deb --contents tirage_*.deb'

ls "$root"/.build/deb/tirage_*.deb
