#!/bin/sh
# Hardening checks (plan § 6, "Durcissement"), phase 3.
#
# Runs tiraged, its workers and a caller under the sandboxing their systemd
# units will have, as transient units of the user's systemd manager, and
# encodes AVIF, WebP and JPEG through them:
#   - the daemon is socket-activated (LISTEN_FDS), with PrivateNetwork=yes,
#     MemoryDenyWriteExecute=yes and the rest of the service's settings: the
#     workers are its children and inherit all of it;
#   - the caller runs under ProtectSystem=strict and connects to a socket that
#     lies on a mount read-only for it.
#
# Differences with the units of phase 4, forced by a user manager: the socket
# is under $XDG_RUNTIME_DIR, not /run/tirage, and ProtectHome is read-only
# rather than yes, because the binaries come from the build directory.
#
# Usage: tests/hardening/check.sh <build-dir>

set -eu

build=$(realpath "${1:?usage: $0 <build-dir>}")
for bin in tiraged tirage-worker tirage; do
    [ -x "$build/$bin" ] || { echo "missing $build/$bin" >&2; exit 2; }
done

dir="${XDG_RUNTIME_DIR:?needs a user session}/tirage-hardening-$$"
unit="tirage-hardening-$$"
mkdir -p "$dir"
cleanup() {
    systemctl --user stop "$unit.socket" "$unit.service" 2>/dev/null || true
    systemctl --user reset-failed "$unit.service" 2>/dev/null || true
    rm -rf "$dir"
}
trap cleanup EXIT

failed=0
check() {
    if [ "$1" = ok ]; then echo "ok    $2"; else echo "FAIL  $2"; failed=1; fi
}

cat > "$dir/tirage.json" <<EOF
{"threads": 2, "worker": "$build/tirage-worker"}
EOF
cat > "$dir/profile.json" <<'EOF'
{"version": 1,
 "input": {"formats": ["png"]},
 "variants": {"v": {"widths": [80], "formats": ["avif", "webp", "jpeg"],
                    "quality": [{"avif": 60, "webp": 80, "jpeg": 85}]}}}
EOF
# 160x120, one colour, with an EXIF block (made by libvips).
base64 -d > "$dir/in.png" <<'EOF'
iVBORw0KGgoAAAANSUhEUgAAAKAAAAB4CAIAAAD6wG44AAAAtGVYSWZJSSoACAAAAAYAEgEDAAEA
AAABAAAAGgEFAAEAAABWAAAAGwEFAAEAAABeAAAAKAEDAAEAAAACAAAAEwIDAAEAAAABAAAAaYcE
AAEAAABmAAAAAAAAADhjAADoAwAAOGMAAOgDAAAGAACQBwAEAAAAMDIxMAGRBwAEAAAAAQIDAACg
BwAEAAAAMDEwMAGgAwABAAAA//8AAAKgBAABAAAAoAAAAAOgBAABAAAAeAAAAAAAAACONOv2AAAA
CXBIWXMAAAPoAAAD6AG1e1JrAAABLklEQVR4nO3RAQkAIBDAwI9jRCMayxQijIMLMNisfQib7wU8
ZXCcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3Cc
wXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEG
xxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkc
Z3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3Cc
wXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEGxxkcZ3CcwXEX9Np+KC/qLe0AAAAASUVORK5C
YII=
EOF

# The service's sandbox, as § 6 plans it, plus the usual systemd-analyze
# security settings, to learn now whether libvips and the codecs live with them.
systemd-run --user --quiet --unit="$unit" \
    --socket-property=ListenStream="$dir/tirage.sock" \
    --socket-property=SocketMode=0660 \
    -p ProtectSystem=strict -p ProtectHome=read-only -p PrivateTmp=yes \
    -p PrivateNetwork=yes -p NoNewPrivileges=yes -p MemoryDenyWriteExecute=yes \
    -p RestrictAddressFamilies=AF_UNIX -p RestrictNamespaces=yes -p RestrictRealtime=yes \
    -p LockPersonality=yes -p SystemCallArchitectures=native \
    -p SystemCallFilter=@system-service -p SystemCallFilter=~@privileged \
    -p ProtectKernelTunables=yes -p ProtectKernelModules=yes -p ProtectControlGroups=yes \
    "$build/tiraged" --config "$dir/tirage.json"

# The caller: a site under ProtectSystem=strict, without network, writing
# only into its private /tmp.
out=$(systemd-run --user --quiet --wait --pipe \
    -p ProtectSystem=strict -p ProtectHome=read-only -p PrivateTmp=yes \
    -p PrivateNetwork=yes -p NoNewPrivileges=yes \
    -E TIRAGE_SOCKET="$dir/tirage.sock" -E TIRAGE_DIRECT= \
    sh -c "
        if touch '$dir/writable' 2>/dev/null; then echo 'socket-mount=writable'; else echo 'socket-mount=read-only'; fi
        cd /tmp
        '$build/tirage' encode --profile '$dir/profile.json' out '$dir/in.png' >/dev/null && echo encode=0 || echo encode=\$?
        for f in out/v-80.avif out/v-80.webp out/v-80.jpg; do [ -s \"\$f\" ] && echo \"file=\$f\"; done
        '$build/tirage' probe --profile '$dir/profile.json' '$dir/in.png' >/dev/null && echo probe=0 || echo probe=\$?
        '$build/tirage' status >/dev/null && echo status=0 || echo status=\$?
    " 2>&1) || true

has() { printf '%s\n' "$out" | grep -qx "$1" && echo ok || echo no; }
check "$(has socket-mount=read-only)" "the socket lies on a read-only mount for the caller"
check "$(has encode=0)" "encode from a ProtectSystem=strict caller"
check "$(has file=out/v-80.avif)" "AVIF encoded by a worker under MemoryDenyWriteExecute"
check "$(has file=out/v-80.webp)" "WebP encoded"
check "$(has file=out/v-80.jpg)" "JPEG encoded"
check "$(has probe=0)" "probe"
check "$(has status=0)" "status"

journal=$(journalctl --user --quiet -o cat -u "$unit.service" 2>/dev/null || true)
printf '%s\n' "$journal" | grep -q "socket handed over by systemd" && a=ok || a=no
check "$a" "the daemon took its socket from systemd (PrivateNetwork=yes)"
printf '%s\n' "$journal" | grep -q "result=failure" && a=no || a=ok
check "$a" "no failure in the daemon's journal"

if [ "$failed" != 0 ]; then
    echo "--- caller"; printf '%s\n' "$out"
    echo "--- daemon"; printf '%s\n' "$journal"
fi
exit "$failed"
