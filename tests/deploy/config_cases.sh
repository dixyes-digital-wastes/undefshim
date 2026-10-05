#!/usr/bin/env bash
#
# The configuration outcomes, end to end under QEMU.
#
#   1. a valid file is found, read and applied
#   2. a file is found but will not parse; the driver must refuse to arm
#   3. an integer is out of range; the strtol range check must catch it
#   4. no file anywhere; the driver runs on its built in defaults
#
# Each case builds its own volume and boots it, so the only thing that differs
# between them is what is on the disk.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
WORK="$BUILD_DIR/config-cases"
# The screen cases leave their machine running while the picture is read, so
# they need a monitor each. The serial case does not care which one it uses.
QMP_PORT="${QMP_PORT:-4455}"
DRIVER="${DRIVER:-$BUILD_DIR/undefshim_driver.efi}"

[ -f "$DRIVER" ] || { echo "missing driver: $DRIVER (run make first)" >&2; exit 1; }

mkdir -p "$WORK"
failures=0

# run_case <name> <config-path-or-empty> <expected-marker> <expected-text>
run_case() {
    local name="$1" config="$2" marker="$3" expect="$4"
    local out="$WORK/$name.log"

    if ! CONFIG="$config" ESP="$WORK/$name.img" tests/deploy/build_esp.sh >"$out.esp" 2>&1; then
        echo "FAIL $name: could not build the volume"
        cat "$out.esp"
        failures=$((failures + 1))
        return
    fi

    if ! ESP="$WORK/$name.img" SERIAL_LOG="$out" STOP_PATTERN="$marker" BOOT_TIMEOUT=60 \
         tests/deploy/run.sh >"$out.run" 2>&1; then
        echo "FAIL $name: never reached $marker"
        tail -5 "$out"
        failures=$((failures + 1))
        return
    fi

    if ! grep -q "$expect" "$out"; then
        echo "FAIL $name: reached $marker but did not see \"$expect\""
        failures=$((failures + 1))
        return
    fi

    echo "PASS $name"
}

# run_screen_case <name> <config-path-or-empty> <expected-text>
#
# For the cases whose configuration cannot name a UART -- because it is
# missing, or because it does not parse -- the driver has no serial port and
# says so on the screen instead. Skipped where the screen cannot be read.
run_screen_case() {
    local name="$1" config="$2" expect="$3"
    local img="$WORK/$name.img"

    if ! CONFIG="$config" ESP="$img" tests/deploy/build_esp.sh >"$WORK/$name.esp" 2>&1; then
        echo "FAIL $name: could not build the volume"
        cat "$WORK/$name.esp"
        failures=$((failures + 1))
        return
    fi

    WORK="$WORK/$name-screen" tests/deploy/screen_case.sh "$img" "$QMP_PORT" "$expect" 90
    case $? in
    0)
        echo "PASS $name"
        ;;
    2)
        echo "SKIP $name: the screen cannot be read"
        ;;
    *)
        failures=$((failures + 1))
        ;;
    esac
}

# A file that parses, and that differs from the defaults on purpose so the
# output proves the file was really used.
#
# It names the UART on purpose as well. The driver only knows where the port
# is once it has read a file that says so, so a configuration that parses and
# names one is the only case here that can report over it; the others are read
# off the screen.
cat > "$WORK/good.toml" <<'EOF'

[uart]
baseAddr = 0x09000000
type = "pl011"
width = 32

[log]
level = "verbose"

[scan]
ldaprRewrite = false

[debug]
enabled = true

[[debug.patch]]
target = "ntoskrnl"
rva    = 0x203bd4
value  = 0x14000000
width  = 4
tag    = "vbar write site"
EOF

# Unbalanced quote: the parser rejects the document.
cat > "$WORK/bad.toml" <<'EOF'
[log]
level = "info
EOF

# A number too large for the parser's integer conversion to hold. Only the
# check on its error indicator catches this -- the digits parse, they just do
# not fit -- so it is what proves that the check survives on the target and
# not only on the host.
#
# It is put under a key that is actually read: a key nothing looks at would
# be ignored, the document would parse, and this would quietly stop testing
# anything at all
cat > "$WORK/range.toml" <<'EOF'
[uart]
baseAddr = 99999999999999999999
EOF

run_case loaded "$WORK/good.toml" "M2 done" "config: loaded"

# The other three have no port to report over: the two that will not parse
# leave the driver with no address to write to, and the one that is absent
# leaves it with nothing at all
# What is matched on is the words, not the punctuation or the heads of the
# milestone names: a recogniser reads the lower case text of this face
# reliably and the capitals and digits less so, so the checks are written
# against what it reads rather than against what was drawn
run_screen_case broken "$WORK/bad.toml"   "broken"
run_screen_case range  "$WORK/range.toml" "broken"
run_screen_case absent ""                 "using defaults"

# The loaded case has to show the file's values, not the defaults.
if ! tests/deploy/plain.sh "$WORK/loaded.log" | grep -q "config: level=4 rewrite=0 debug=1 patches=1"; then
    echo "FAIL loaded: values do not match the file"
    tests/deploy/plain.sh "$WORK/loaded.log" | grep -E 'level=' || true
    failures=$((failures + 1))
else
    echo "PASS loaded: values came from the file"
fi

# The reason a file was refused is the point of the screen path: it is what a
# machine with no port to say it on has to show
if [ -f "$WORK/broken-screen/screen.txt" ]; then
    if grep -q "broken" "$WORK/broken-screen/screen.txt"; then
        echo "PASS broken: reason reported on the screen"
    else
        echo "FAIL broken: no reason was reported"
        cat "$WORK/broken-screen/screen.txt"
        failures=$((failures + 1))
    fi
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures configuration case(s) failed"
    exit 1
fi
echo "configuration cases: all passed"
