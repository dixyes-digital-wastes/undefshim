#!/usr/bin/env bash
#
# The delivery check: does WinPE reach its desktop, with the shim loaded?
#
# The Windows image runs lrcpc_demo.exe and udf_demo.exe from startnet.cmd and
# echoes each ERRORLEVEL, so the result is on the screen and nowhere else.
# There is no way to read it from the serial port: the kernel does not map the
# UART, and the payload deliberately says nothing for that reason.
#
# So this leaves the machine alone long enough to get there and takes pictures.
# The pictures are for a person to look at; nothing here tries to read them,
# because a check that pattern matches on a screen will pass on the wrong
# screen eventually.
#
# Earlier pictures are kept: WinPE spends a long time in boot.wim before it
# runs anything, and knowing where it stopped is the difference between "not
# there yet" and "died earlier".
#
# The Windows disk is external, so this is skipped unless WIN_DISK is set.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
WORK="$BUILD_DIR/windows"
WIN_DISK="${WIN_DISK:-}"
# How long to keep taking pictures for. WinPE on TCG is slow and the first
# screen worth reading is minutes in.
WATCH="${WATCH:-480}"
INTERVAL="${INTERVAL:-60}"
QMP_PORT="${QMP_PORT:-4444}"

if [ -z "$WIN_DISK" ]; then
    echo "WIN_DISK not set, skipping"
    exit 0
fi
if [ ! -f "$WIN_DISK" ]; then
    echo "missing windows disk: $WIN_DISK" >&2
    exit 1
fi

mkdir -p "$WORK"
# Whether to carry out the instructions by rewriting them or by taking the
# exception they cause. Both are delivery candidates and they fail in
# different places, so the run that is looked at has to say which one it was:
# a picture of the wrong mechanism is a picture of nothing.
REWRITE="${REWRITE:-true}"
cat > "$WORK/run.toml" <<EOF

[scan]
ldaprRewrite = $REWRITE

[log]
level = "info"

[debug]
enabled = true
arm = true
EOF

log="$WORK/serial.log"
shots="$WORK/shots"
rm -rf "$shots"
mkdir -p "$shots"

if ! CONFIG="$WORK/run.toml" ESP="$WORK/run.img" tests/deploy/build_esp.sh \
        >"$WORK/esp.out" 2>&1; then
    echo "FAIL: could not build the volume"
    cat "$WORK/esp.out"
    exit 1
fi

ESP="$WORK/run.img" SERIAL_LOG="$log" WIN_DISK="$WIN_DISK" \
STOP_PATTERN='windows-never-matches' BOOT_TIMEOUT=$((WATCH + 300)) \
    tests/deploy/run.sh >"$WORK/run.out" 2>&1 &
runpid=$!

stop() {
    kill "$runpid" 2>/dev/null || true
    pkill -f "file=$WORK/run.img" 2>/dev/null || true
}
trap stop EXIT INT TERM

# Wait for the shim to be armed, which is the last thing our side can say.
armed=no
for _ in $(seq 1 $((WATCH + 240))); do
    if grep -q 'M6.5 armed' "$log" 2>/dev/null; then
        armed=yes
        break
    fi
    sleep 1
done
if [ "$armed" != "yes" ]; then
    echo "FAIL: the shim was never armed"
    grep -aE 'arm:|M5' "$log" | tail -6
    exit 1
fi
echo "armed: $(grep -a 'arm: vbar' "$log" | tail -1)"

# Then leave it alone and watch. The pictures are the result.
elapsed=0
n=0
while [ "$elapsed" -lt "$WATCH" ]; do
    sleep "$INTERVAL"
    elapsed=$((elapsed + INTERVAL))
    n=$((n + 1))
    shot="$shots/$(printf '%03d' "$n")-${elapsed}s.ppm"
    if python3 - "$shot" "$QMP_PORT" <<'PY' >/dev/null 2>&1
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[2])), timeout=5)
f = s.makefile("rwb")
f.readline()


def cmd(o):
    f.write((json.dumps(o) + "\n").encode())
    f.flush()
    while True:
        line = f.readline()
        if not line:
            raise SystemExit(1)
        r = json.loads(line)
        if "return" in r or "error" in r:
            return r


cmd({"execute": "qmp_capabilities"})
cmd({"execute": "screendump", "arguments": {"filename": sys.argv[1]}})
PY
    then
        echo "  ${elapsed}s: $shot"
    else
        echo "  ${elapsed}s: the machine did not answer, it may be gone"
        break
    fi
done

# Convert whatever was taken, so the pictures can be looked at.
if command -v pnmtopng >/dev/null; then
    for ppm in "$shots"/*.ppm; do
        [ -f "$ppm" ] && pnmtopng "$ppm" >"${ppm%.ppm}.png" 2>/dev/null
    done
fi

echo "pictures in $shots"
ls -1 "$shots" | tail -5
