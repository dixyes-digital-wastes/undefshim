#!/usr/bin/env bash
#
# The fast check: a stand-in kernel that runs at EL1 and uses the
# instructions at issue, saying on the serial port whether each one produced
# the right value.
#
# Windows takes ten minutes and a pair of eyes; this takes seconds and an exit
# code. It reaches the same path: the image is loaded and recognised as a
# kernel by the sections it carries, the vectors of the table it installs are
# taken over, and every RCpc load in it takes an exception the handler has to
# carry out.
#
# Two things about how this reaches that path are worth knowing, because
# neither is obvious and both were wrong before:
#
#   * The firmware runs at EL2 with VHE, so an application it starts runs
#     there too, and a trap at EL2 goes to the firmware's own vector base.
#     Every vector this project takes over is an EL1 one, because that is the
#     level a kernel runs at. So the stand-in leaves EL2 for EL1 itself.
#
#   * The plan that decides where a kernel's vectors are comes from a memory
#     map scan that only a boot manager produces. Nothing here produces one,
#     so the check runs with the switch that takes the vectors over at load
#     time instead.
#
# Both are in tests/fake/us.toml and tests/fake/ntoskrnl.c.
#
# Because nothing replaces the instructions in this run, the check only passes
# if the exception path carried every load out. That is the property being
# tested, and it is asserted rather than assumed.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

CHECK="fake_case"
. tests/deploy/check.sh

BUILD_DIR="${BUILD_DIR:-build}"
DRIVER="${DRIVER:-$BUILD_DIR/undefshim_driver.efi}"
FAKE_DIR="$BUILD_DIR/fake"
ESP="${ESP:-$BUILD_DIR/esp-fake.img}"
SERIAL_LOG="${SERIAL_LOG:-$BUILD_DIR/serial-fake.log}"

QEMU_CPU="${QEMU_CPU:-tsv110}"
export QEMU_CPU

[ -f "$DRIVER" ] || checkFail "no driver at $DRIVER (run make first)"

make -s fake BUILD_DIR="$BUILD_DIR" >/dev/null
[ -f "$FAKE_DIR/ntoskrnl.efi" ] || checkFail "no fake kernel at $FAKE_DIR/ntoskrnl.efi"

DRIVER="$DRIVER" CONFIG=tests/fake/us.toml STARTUP=tests/deploy/fake_startup.nsh \
    ESP="$ESP" BUILD_DIR="$BUILD_DIR" PATCHLIST_DIR= \
    bash tests/deploy/build_esp.sh >/dev/null
mcopy -i "$ESP" -o "$FAKE_DIR/ntoskrnl.efi" ::/ntoskrnl.efi

# A socket left behind by an earlier run makes QEMU refuse to start, which
# looks exactly like a boot that said nothing.
rm -f "${SERIAL_SOCK:-$BUILD_DIR/serial.sock}"

ESP="$ESP" SERIAL_LOG="$SERIAL_LOG" STOP_PATTERN="FAKEK: PASS" WIN_DISK= \
    BOOT_TIMEOUT="${BOOT_TIMEOUT:-120}" \
    bash tests/deploy/run.sh >/dev/null 2>&1 || true

checkNote "the fake kernel's report:"
tests/deploy/plain.sh "$SERIAL_LOG" | grep -a "FAKEK:" || true

# The property that makes the rest mean anything: the vectors were taken
# over. Without it the run could be succeeding for a reason that has nothing
# to do with the handler
if ! tests/deploy/plain.sh "$SERIAL_LOG" | grep -qa "M6.5 armed"; then
    checkFail "the vectors were never taken over, so nothing here was tested"
fi

if tests/deploy/plain.sh "$SERIAL_LOG" | grep -qa "WRONG\|FAKEK: FAIL"; then
    checkFail "the fake kernel got a value wrong"
fi

if tests/deploy/plain.sh "$SERIAL_LOG" | grep -qa "FAKEK: PASS"; then
    checkPass "every RCpc load in the fake kernel was carried out by the handler"
fi
checkFail "the fake kernel did not carry out every instruction"
