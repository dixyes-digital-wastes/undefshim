#!/usr/bin/env bash
#
# Does the driver come up on its own?
#
# One boot, no Windows disk: the driver is loaded by the shell, reads its
# configuration, takes memory for the pool, and installs its hooks. This is the
# check that the pieces still fit together after a change, and the place the
# arithmetic that the payload will depend on gets verified on real firmware
# rather than only on a host.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
SERIAL_LOG="${SERIAL_LOG:-$BUILD_DIR/driver-boot.log}"

SERIAL_LOG="$SERIAL_LOG" \
STOP_PATTERN='M4 setup|M4 failed|M2 failed' \
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}" \
    tests/deploy/run.sh || exit 1

fail() {
    echo "FAIL: $1"
    grep -E 'pool|config|loadimage|milestone' "$SERIAL_LOG" | tail -8
    exit 1
}

grep -q 'M4 setup' "$SERIAL_LOG" || fail "the driver did not finish bringing up"
grep -q 'config: loaded' "$SERIAL_LOG" || fail "the configuration was not read"
grep -q 'pool: ready' "$SERIAL_LOG" || fail "the pool was not allocated"

# The pool has to be identity mapped, or the payload cannot reach it both
# before and after the address space is rebuilt.
pool=$(grep -m1 '^pool: pa=' "$SERIAL_LOG") || fail "no pool report"
pa=$(printf '%s' "$pool" | sed -n 's/.*pa=\(0x[0-9a-f]*\).*/\1/p')
va=$(printf '%s' "$pool" | sed -n 's/.*va=\(0x[0-9a-f]*\).*/\1/p')

[ -n "$pa" ] && [ -n "$va" ] || fail "the pool report is not parseable"
[ "$pa" = "$va" ] || fail "the pool is not identity mapped: pa=$pa va=$va"

# One page of header plus a stack for every possible CPU.
want_bytes=$(( (1 + 8 * 4) * 4096 ))
got_bytes=$(printf '%s' "$pool" | sed -n 's/.*bytes=\(0x[0-9a-f]*\).*/\1/p')
[ "$((got_bytes))" -eq "$want_bytes" ] \
    || fail "pool size is $((got_bytes)), expected $want_bytes"

slots=$(printf '%s' "$pool" | sed -n 's/.*slots=\([0-9]*\).*/\1/p')
[ "$slots" = "8" ] || fail "expected 8 stack slots, got $slots"

# The stacks have to sit inside the pool and be aligned for the CPU.
stacks=$(grep -m1 '^pool: stack' "$SERIAL_LOG") || fail "no stack report"
first=$(printf '%s' "$stacks" | sed -n 's/.*stack\[0\]=\(0x[0-9a-f]*\).*/\1/p')
last=$(printf '%s' "$stacks" | sed -n 's/.*stack\[[0-9]*\]=\(0x[0-9a-f]*\).*/\1/p')

[ -n "$first" ] && [ -n "$last" ] || fail "the stack report is not parseable"
[ $(( first & 15 )) -eq 0 ] || fail "first stack top $first is not 16 byte aligned"
[ $(( last & 15 )) -eq 0 ] || fail "last stack top $last is not 16 byte aligned"
[ $(( last - first )) -eq $(( 7 * 16384 )) ] \
    || fail "stacks are not evenly spaced: first=$first last=$last"
[ $(( first )) -ge $(( pa + 4096 )) ] || fail "the first stack overlaps the pool header"
[ $(( last )) -le $(( pa + want_bytes )) ] || fail "the last stack runs past the pool"

echo "pool: $pa identity mapped, $((want_bytes)) bytes, $slots stacks, first=$first last=$last"
echo "PASS: the driver brings itself up"
