#!/usr/bin/env bash
#
# The fast check: a stand-in kernel that uses the instructions at issue and
# says on the serial port whether each one produced the right value.
#
# Windows takes ten minutes and a pair of eyes; this takes seconds and an exit
# code. It exercises the same paths: the image is loaded by name as ntoskrnl,
# the driver arms it, the patch list for it is applied before it runs, and one
# site is deliberately left out of that list so the exception path runs too.
#
# The list is generated from the image itself, so it stays right when the fake
# changes; only the site that is left out is chosen by hand, and it is chosen
# as the 32 bit form because getting its width wrong is the mistake this shim
# has already made once.
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
LIST="$FAKE_DIR/fake.txt"

QEMU_CPU="${QEMU_CPU:-tsv110}"
export QEMU_CPU

[ -f "$DRIVER" ] || checkFail "no driver at $DRIVER (run make first)"

make -s fake BUILD_DIR="$BUILD_DIR" >/dev/null
[ -f "$FAKE_DIR/ntoskrnl.efi" ] || checkFail "no fake kernel at $FAKE_DIR/ntoskrnl.efi"

python3 tests/deploy/enumerate_sites.py --kernel "$FAKE_DIR/ntoskrnl.efi" \
    --pefile ntoskrnl --out "$LIST" | tail -1

# Leave the 32 bit form out, so that site keeps taking the exception. With
# DROP_SITE=0 every site is replaced and no exception is taken, which is how
# the two halves are told apart when something goes wrong.
if [ "${DROP_SITE:-1}" = "1" ]; then
python3 - "$LIST" <<'PY'
import sys
path = sys.argv[1]
lines = open(path).read().split("\n")
kept = []
dropped = 0
for line in lines:
    if line.startswith("0x") and " b8bfc100 " in line:
        kept.append("# " + line + "  (left out on purpose: this one must trap)")
        dropped += 1
        continue
    kept.append(line)
open(path, "w").write("\n".join(kept))
print("sites left out of the list: %d" % dropped)
PY
else
    checkNote "sites left out of the list: none"
fi

DRIVER="$DRIVER" CONFIG=config/us.toml STARTUP=tests/deploy/fake_startup.nsh \
    ESP="$ESP" BUILD_DIR="$BUILD_DIR" PATCHLIST_DIR="$(dirname "$LIST")" \
    bash tests/deploy/build_esp.sh >/dev/null
mcopy -i "$ESP" -o "$FAKE_DIR/ntoskrnl.efi" ::/ntoskrnl.efi

# A socket left behind by an earlier run makes QEMU refuse to start, which
# looks exactly like a boot that said nothing.
rm -f "${SERIAL_SOCK:-$BUILD_DIR/serial.sock}"

ESP="$ESP" SERIAL_LOG="$SERIAL_LOG" STOP_PATTERN="FAKEK: PASS" WIN_DISK= \
    ARM_PATTERN="M6.5 armed" BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}" \
    bash tests/deploy/run.sh >/dev/null 2>&1 || true

checkNote "the fake kernel's report:"
tests/deploy/plain.sh "$SERIAL_LOG" | grep -a "FAKEK:" || true
checkNote "what the driver did:"
tests/deploy/plain.sh "$SERIAL_LOG" | grep -a "patch:" || true

if tests/deploy/plain.sh "$SERIAL_LOG" | grep -qa "FAKEK: PASS"; then
    checkPass "the fake kernel carried out every instruction it was built with"
fi
checkFail "the fake kernel did not carry out every instruction"
