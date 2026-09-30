#!/bin/sh
# Package checks (plan § 8 and § 9), phase 4.
#
# Installs the .deb into a trixie container whose init is systemd, as on a
# server, and checks what phase 3 could not with a user manager: the real
# units, the socket under /run/tirage open to the tirage group only, callers
# under ProtectHome=yes, the slice, reload, a stop during an encode, an upgrade
# that keeps the configuration, then removal and purge.
#
# Usage: tests/package/check.sh .build/deb/tirage_<version>_amd64.deb
# Needs podman, and python3 to write a large test image.

set -eu

deb=$(realpath "${1:?usage: $0 <tirage.deb>}")
here=$(cd "$(dirname "$0")" && pwd)
name="tirage-package-$$"
work=$(mktemp -d)
cleanup() {
    podman rm -f "$name" >/dev/null 2>&1 || true
    rm -rf "$work"
}
trap cleanup EXIT

failed=0
check() {
    if [ "$1" = ok ]; then echo "ok    $2"; else echo "FAIL  $2"; failed=1; fi
}
# ok when the command succeeds.
test_ok() { if "$@" >/dev/null 2>&1; then echo ok; else echo no; fi; }
on() { podman exec "$name" "$@"; }
journal() { on journalctl --quiet -o cat -u tirage.service; }

# The inputs: a small PNG with EXIF (the hardening check's), and 2000x1500 of
# noise, long enough to encode at AVIF effort 9 (seconds) to stop the daemon
# meanwhile.
sed -n '/^iVBOR/,/^YII=/p' "$here/../hardening/check.sh" | base64 -d > "$work/in.png"
python3 - "$work/noise.png" <<'PY'
import os, struct, sys, zlib
w, h = 1200, 900
raw = b"".join(b"\0" + os.urandom(w * 3) for _ in range(h))
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                              + chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b""))
PY
cat > "$work/profile.json" <<'JSON'
{"version": 1,
 "input": {"formats": ["png"]},
 "variants": {"v": {"widths": [80], "formats": ["avif", "webp", "jpeg"],
                    "quality": [{"avif": 60, "webp": 80, "jpeg": 85}]}}}
JSON
cat > "$work/slow.json" <<'JSON'
{"version": 1,
 "input": {"formats": ["png"]},
 "encoding": {"avif": {"effort": 9, "chroma": "444"}},
 "variants": {"v": {"widths": [1200], "formats": ["avif"], "quality": [{"avif": 90}]}}}
JSON
chmod 755 "$work"
chmod 644 "$work"/*

podman build -q -t tirage-package-test "$here" >/dev/null
podman run -d --name "$name" --systemd=always \
    -v "$deb":/deb/tirage.deb:ro,z -v "$work":/srv/site:ro,z tirage-package-test >/dev/null
on systemctl is-system-running --wait >/dev/null 2>&1 || true

# --- Installation
out=$(on sh -c 'DEBIAN_FRONTEND=noninteractive apt-get install -y -q /deb/tirage.deb 2>&1') \
    && a=ok || { a=no; printf '%s\n' "$out"; }
check "$a" "apt installs the package and its dependencies"
check "$(test_ok on getent group tirage)" "the tirage user and group exist"
threads=$(( $(on nproc) / 2 ))
[ "$threads" -ge 1 ] || threads=1
on grep -q "\"threads\": $threads\$" /etc/tirage/tirage.json && a=ok || a=no
check "$a" "a first configuration with half of the CPUs ($threads threads)"
check "$(test_ok on systemctl is-enabled tirage.socket)" "tirage.socket enabled"
check "$(test_ok on systemctl is-active tirage.socket)" "tirage.socket listening"
on systemctl is-active tirage.service >/dev/null 2>&1 && a=no || a=ok
check "$a" "tiraged not started before the first connection"
[ "$(on stat -c '%U:%G %a' /run/tirage/tirage.sock)" = "root:tirage 660" ] && a=ok || a=no
check "$a" "/run/tirage/tirage.sock is root:tirage, mode 660"

# --- A site: tirage in its SupplementaryGroups, under its own sandbox.
on useradd --system --no-create-home site
on useradd --system --no-create-home stranger
site() {
    on systemd-run --quiet --wait --pipe -p User=site -p SupplementaryGroups=tirage \
        -p ProtectSystem=strict -p ProtectHome=yes -p PrivateTmp=yes -p PrivateNetwork=yes \
        -p NoNewPrivileges=yes "$@"
}
out=$(site sh -c '
    cd /tmp
    tirage encode --profile /srv/site/profile.json out /srv/site/in.png >/dev/null && echo encode=0 || echo encode=$?
    for f in out/v-80.avif out/v-80.webp out/v-80.jpg; do [ -s "$f" ] && echo "file=$f"; done
    tirage probe --profile /srv/site/profile.json /srv/site/in.png >/dev/null && echo probe=0 || echo probe=$?
    tirage status >/dev/null && echo status=0 || echo status=$?
' 2>&1) || true
has() { printf '%s\n' "$out" | grep -qx "$1" && echo ok || echo no; }
check "$(has encode=0)" "a site in the group encodes, under ProtectHome=yes"
check "$(has file=out/v-80.avif)" "AVIF encoded by the packaged worker"
check "$(has file=out/v-80.webp)" "WebP encoded"
check "$(has file=out/v-80.jpg)" "JPEG encoded"
check "$(has probe=0)" "probe"
check "$(has status=0)" "status"
[ "$failed" = 0 ] || printf '%s\n' "$out"

out=$(on runuser -u stranger -- tirage status 2>&1) && a=no || a=ok
printf '%s\n' "$out" | grep -q "Permission denied" || a=no
check "$a" "a user outside the group is refused by the socket"

# --- The daemon, its slice and its sandbox.
check "$(test_ok on systemctl is-active tirage.service)" "tiraged started by its socket"
pid=$(on systemctl show -p MainPID --value tirage.service)
[ "$(on ps -o user= -p "$pid")" = tirage ] && a=ok || a=no
check "$a" "tiraged runs as tirage"
[ "$(on systemctl show -p ControlGroup --value tirage.service)" = /tirage.slice/tirage.service ] && a=ok || a=no
check "$a" "tiraged, hence its workers, in tirage.slice"
slice=$(on systemctl show -p CPUWeight -p MemoryHigh -p MemoryMax tirage.slice)
printf '%s\n' "$slice" | grep -qx CPUWeight=50 && a=ok || a=no
check "$a" "tirage.slice: CPUWeight=50"
high=$(printf '%s\n' "$slice" | sed -n 's/^MemoryHigh=//p')
max=$(printf '%s\n' "$slice" | sed -n 's/^MemoryMax=//p')
total=$(on awk '/^MemTotal:/ {print $2 * 1024}' /proc/meminfo)
awk -v h="$high" -v m="$max" -v t="$total" \
    'BEGIN { exit !(h > t * 0.12 && h < t * 0.13 && m > t * 0.165 && m < t * 0.175) }' && a=ok || a=no
check "$a" "tirage.slice: MemoryHigh 12.5% and MemoryMax 17% of $((total >> 20)) MiB ($((high >> 20)), $((max >> 20)) MiB)"
j=$(journal)
printf '%s\n' "$j" | grep -q "socket handed over by systemd" && a=ok || a=no
check "$a" "the socket handed over by systemd"
printf '%s\n' "$j" | grep -q "caller=site op=encode .*result=ok" && a=ok || a=no
check "$a" "the journal names the caller"
printf '%s\n' "$j" | grep -q "result=failure" && a=no || a=ok
check "$a" "no failure in the journal"
echo "info  $(on systemd-analyze security --no-pager tirage.service | tail -n 1)"
# What a rootless container cannot give, tried by tests/hardening/check.sh instead.
printf '%s\n' "$j" | grep -E "proceeding without|ignoring namespace" | sort -u | sed 's/^/info  /'


on systemctl reload tirage.service
sleep 1
journal | grep -q "reloaded: $threads threads" && a=ok || a=no
check "$a" "systemctl reload rereads the configuration"

# --- Stop during an encode: the encode finishes (KillMode=mixed).
site sh -c 'cd /tmp && tirage encode --profile /srv/site/slow.json out /srv/site/noise.png >/dev/null \
             && [ -s out/v-1200.avif ] && echo slow=0 || echo slow=$?' > "$work/slow.out" 2>&1 &
caller=$!
running=no
for _ in $(seq 100); do
    if on tirage status 2>/dev/null | grep -q "^running: 1"; then running=yes; break; fi
    sleep 0.1
done
[ "$running" = yes ] && a=ok || a=no
check "$a" "a long encode is running"
# (systemd warns that the socket may start it again: that is the point.)
on systemctl stop tirage.service 2>/dev/null
wait "$caller" || true
grep -qx slow=0 "$work/slow.out" && a=ok || { a=no; cat "$work/slow.out"; }
check "$a" "systemctl stop lets the running encode finish"
journal | grep -q "^tiraged: stopped" && a=ok || a=no
check "$a" "tiraged stopped cleanly"
# The long encode's worker takes far more than the daemon ever does.
if peak=$(on cat /sys/fs/cgroup/tirage.slice/memory.peak 2>/dev/null); then
    [ "$peak" -gt $((100 << 20)) ] && a=ok || a=no
    check "$a" "the workers' memory is counted in the slice (peak $((peak >> 20)) MiB)"
else
    echo "skip  the slice's memory.peak (no memory controller in this container)"
fi
check "$(test_ok on systemctl is-active tirage.socket)" "the socket stays open meanwhile"

# --- Upgrade: the configuration stays the administrator's.
on sh -c 'printf "{\"threads\": 1}\n" > /etc/tirage/tirage.json'
on sh -c 'DEBIAN_FRONTEND=noninteractive apt-get install -y -q --reinstall /deb/tirage.deb >/dev/null 2>&1'
on grep -q '"threads": 1' /etc/tirage/tirage.json && a=ok || a=no
check "$a" "an upgrade keeps the configuration"
on tirage status 2>/dev/null | grep -q "1 threads per encode" && a=ok || a=no
check "$a" "the daemon answers after the upgrade, with it"

# --- Removal and purge.
on sh -c 'DEBIAN_FRONTEND=noninteractive apt-get remove -y -q tirage >/dev/null 2>&1'
on systemctl is-active tirage.socket >/dev/null 2>&1 && a=no || a=ok
on test -e /run/tirage/tirage.sock && a=no
check "$a" "remove stops the socket and removes its file"
check "$(test_ok on test -e /etc/tirage/tirage.json)" "remove keeps the configuration"
on sh -c 'DEBIAN_FRONTEND=noninteractive apt-get purge -y -q tirage >/dev/null 2>&1'
on test -e /etc/tirage && a=no || a=ok
check "$a" "purge removes the configuration"

if [ "$failed" != 0 ]; then
    echo "--- daemon"; journal || true
fi
exit "$failed"
