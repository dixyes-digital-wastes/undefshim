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

CHECK="driver_boot"
. tests/deploy/check.sh

BUILD_DIR="${BUILD_DIR:-build}"
SERIAL_LOG="${SERIAL_LOG:-$BUILD_DIR/driver-boot.log}"

SERIAL_LOG="$SERIAL_LOG" \
STOP_PATTERN='M4 setup|M4 failed|M2 failed' \
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}" \
WIN_DISK= \
    tests/deploy/run.sh || exit 1

fail() {
    checkNote "what the driver said:"
    tests/deploy/plain.sh "$SERIAL_LOG" | grep -E 'pool|config|loadimage|milestone' | tail -8
    checkFail "$1"
}

tests/deploy/plain.sh "$SERIAL_LOG" | grep -q 'M4 setup' || fail "the driver did not finish bringing up"
tests/deploy/plain.sh "$SERIAL_LOG" | grep -q 'config: loaded' || fail "the configuration was not read"
tests/deploy/plain.sh "$SERIAL_LOG" | grep -q 'pool: ready' || fail "the pool was not allocated"

# The pool has to be identity mapped, or the payload cannot reach it both
# before and after the address space is rebuilt.
pool=$(tests/deploy/plain.sh "$SERIAL_LOG" | grep -m1 '^pool: pa=') || fail "no pool report"
pa=$(printf '%s' "$pool" | sed -n 's/.*pa=\(0x[0-9a-f]*\).*/\1/p')
va=$(printf '%s' "$pool" | sed -n 's/.*va=\(0x[0-9a-f]*\).*/\1/p')

[ -n "$pa" ] && [ -n "$va" ] || fail "the pool report is not parseable"
[ "$pa" = "$va" ] || fail "the pool is not identity mapped: pa=$pa va=$va"

# One page of header plus a stack for every possible CPU.
slots=$(printf '%s' "$pool" | sed -n 's/.*slots=\([0-9]*\).*/\1/p')
[ "$slots" = "8" ] || fail "expected 8 stack slots, got $slots"

# A header of whole pages followed by a stack for every CPU. The header's size
# is a property of the structure, so it is taken from what was allocated
# rather than written here: a number copied into the check drifts the moment
# a field is added, and this is the arithmetic the payload depends on
got_bytes=$(printf '%s' "$pool" | sed -n 's/.*bytes=\(0x[0-9a-f]*\).*/\1/p')
header_bytes=$(( got_bytes - slots * 16384 ))
[ "$header_bytes" -ge 4096 ] && [ $(( header_bytes % 4096 )) -eq 0 ] \
    || fail "the pool is $((got_bytes)) bytes: not a whole page of header plus $slots stacks"

# The stacks have to sit inside the pool and be aligned for the CPU.
stacks=$(tests/deploy/plain.sh "$SERIAL_LOG" | grep -m1 '^pool: stack') || fail "no stack report"
first=$(printf '%s' "$stacks" | sed -n 's/.*stack\[0\]=\(0x[0-9a-f]*\).*/\1/p')
last=$(printf '%s' "$stacks" | sed -n 's/.*stack\[[0-9]*\]=\(0x[0-9a-f]*\).*/\1/p')

[ -n "$first" ] && [ -n "$last" ] || fail "the stack report is not parseable"
[ $(( first & 15 )) -eq 0 ] || fail "first stack top $first is not 16 byte aligned"
[ $(( last & 15 )) -eq 0 ] || fail "last stack top $last is not 16 byte aligned"
[ $(( last - first )) -eq $(( 7 * 16384 )) ] \
    || fail "stacks are not evenly spaced: first=$first last=$last"
# The top of a stack is its base plus the stack: what sits at the header is
# the bottom of the first one
[ $(( first )) -eq $(( pa + header_bytes + 16384 )) ] \
    || fail "the first stack top $first is not a stack above the header at $pa"
[ $(( last )) -le $(( pa + got_bytes )) ] || fail "the last stack runs past the pool"

checkNote "pool $pa is identity mapped, $((got_bytes)) bytes, $slots stacks"
checkNote "first stack top $first, last $last"
checkPass "the driver brings itself up"
