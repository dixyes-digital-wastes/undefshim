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

# A file that parses, and that differs from the defaults on purpose so the
# output proves the file was really used.
cat > "$WORK/good.toml" <<'EOF'
version = 1

[log]
level = "verbose"

[scan]
ldapr_rewrite = false

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

# An integer that does not fit. Only the strtol range check catches this, so
# it is what proves errno=ERANGE survives on the target as well as on the host.
cat > "$WORK/range.toml" <<'EOF'
version = 99999999999999999999
EOF

run_case loaded "$WORK/good.toml"  "US-M2-DONE" "config: loaded"
run_case broken "$WORK/bad.toml"   "US-M2-FAIL" "config: broken"
run_case range  "$WORK/range.toml" "US-M2-FAIL" "config: broken"
run_case absent ""                 "US-M2-DONE" "config: absent"

# The loaded case has to show the file's values, not the defaults.
if ! grep -q "log.level=3 ldapr_rewrite=0 debug.enabled=1 patches=1" "$WORK/loaded.log"; then
    echo "FAIL loaded: values do not match the file"
    grep -E 'log\.level|patch\[0\]' "$WORK/loaded.log" || true
    failures=$((failures + 1))
else
    echo "PASS loaded: values came from the file"
fi

if ! grep -q "not a simple quoted string\|parse failed\|config:" "$WORK/broken.log"; then
    echo "FAIL broken: no reason was reported"
    failures=$((failures + 1))
else
    echo "PASS broken: reason reported"
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures configuration case(s) failed"
    exit 1
fi
echo "configuration cases: all passed"
