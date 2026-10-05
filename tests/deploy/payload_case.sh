#!/usr/bin/env bash
#
# Does the payload get placed, and does it run there?
#
# The blob is built as position independent code and linked at zero, then
# copied to executable memory the firmware picks. Three things have to be true
# for anything later to work, and none of them is visible from the host:
#
#   - the copy lands in memory that is executable and that the OS will keep
#   - the entry and the configuration block are where the generated offsets
#     say they are, relative to the copy
#   - the payload can find its own data from the address it was entered at,
#     which is the only thing that makes a position independent blob useful
#
# The last one is what the payload proves by writing to the serial port: it
# reads the port address out of the configuration block, so output means the
# block was written where the payload looks for it.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
SERIAL_LOG="${SERIAL_LOG:-$BUILD_DIR/payload.log}"

SERIAL_LOG="$SERIAL_LOG" \
STOP_PATTERN='M6 payload placed|M6 failed' \
BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}" \
    tests/deploy/run.sh || exit 1

fail() {
    echo "FAIL: $1"
    grep -a 'payload' "$SERIAL_LOG" | tail -5
    exit 1
}

grep -q 'M6 payload placed' "$SERIAL_LOG" || fail "the payload was never reported"
! grep -q 'M6 failed' "$SERIAL_LOG" || fail "the payload could not be placed"

line=$(tr -d '\r' <"$SERIAL_LOG" | grep -a -m1 '^payload: at ')
[ -n "$line" ] || fail "no placement report"

base=$(printf '%s' "$line" | sed -n 's/.*at \(0x[0-9a-f]*\).*/\1/p')
bytes=$(printf '%s' "$line" | sed -n 's/.*bytes=\([0-9]*\).*/\1/p')
entry=$(printf '%s' "$line" | sed -n 's/.*entry=\(0x[0-9a-f]*\).*/\1/p')
config=$(printf '%s' "$line" | sed -n 's/.*config=\(0x[0-9a-f]*\).*/\1/p')

[ -n "$base" ] && [ -n "$bytes" ] && [ -n "$entry" ] && [ -n "$config" ] \
    || fail "the placement report is not parseable"

# Both have to lie inside the copy, or the offsets in the generated header and
# the linked blob disagree and the payload is being entered somewhere that is
# not its entry. bash reads bare digits as decimal, so the addresses are
# converted explicitly.
basev=$(( base ))
limit=$(( basev + bytes ))
entryv=$(( entry ))
configv=$(( config ))

[ "$entryv" -ge "$basev" ] && [ "$entryv" -lt "$limit" ] \
    || fail "the entry $entry is outside the copy at $base"
[ "$configv" -ge "$basev" ] && [ "$configv" -lt "$limit" ] \
    || fail "the configuration block $config is outside the copy at $base"

# The payload has to have written through the port address it was given, and
# to have answered the question about its own mapping. In the firmware's
# regime there are no tables, so the answer is the identity one; what this
# proves is that the call reached the payload, that it read its configuration,
# and that it came back.
alive=$(tr -d '\r' <"$SERIAL_LOG" | grep -a -m1 '^payload: alive ')
[ -n "$alive" ] || fail "the payload was placed but never ran"

frame=$(printf '%s' "$alive" | sed -n 's/.*frame=\([0-9]*\).*/\1/p')
[ "$frame" = "288" ] || fail "the payload reports frame=$frame, expected 288"

mapping=$(tr -d '\r' <"$SERIAL_LOG" | grep -a -m1 '^payload: selfmap ')
[ -n "$mapping" ] || fail "the payload did not report its own mapping"
case "$mapping" in
*"va=$base pa=$base"*)
    ;;
*)
    fail "the payload reports its mapping as: $mapping"
    ;;
esac

echo "PASS: payload at $base ($bytes bytes), entry $entry, config $config"
echo "      $alive"
echo "      $mapping"
