#!/usr/bin/env bash
#
# Run the image under QEMU and report whether undefshim came up.
#
# The firmware here has an internal shell, but it only falls back to it after
# every boot option fails, which costs minutes. The boot volume carries the
# standalone UEFI shell as BOOTAA64.EFI instead, and it runs startup.nsh.
#
# Serial goes through a unix socket so the monitor can watch it live and stop
# the moment the expected marker shows up.
#
# Where the guest runs, and at which exception level
# --------------------------------------------------
# QEMU_ACCEL is how the guest's CPU is provided. `tcg` (the default) emulates
# it, which is the only way to be handed a CPU model this work is about -- one
# without the RCpc loads -- and such a machine has EL3 and EL2 to give. `kvm`
# runs on the host's CPU: a host without nested virtualization has no EL2 to
# give its own guests, so the guest is entered at EL1, and the only CPU model
# that can be asked for there is the host's own. The accel decides the -accel
# arguments and the default CPU; QEMU_CPU still overrides the model.
#
# QEMU_EL is which exception level the firmware is entered at, and it decides
# both what the machine is given and which firmware file is used. EL2 (the
# default) asks for virtualization=on,secure=on, so that the firmware comes up
# at EL2 with an EL3 above it, and boots QEMU_FW_EL2. EL1 asks for neither, so
# that the machine has nothing above EL1, and boots QEMU_FW_EL1. QEMU_FW, when
# set, is the file used whatever the level says.
#
# QEMU_FW_LOAD is how that file reaches the guest. `kernel` (the default)
# passes it with -kernel, which is what a file in the Linux image format wants
# -- the form the firmware in use here is packaged in. `pflash` puts the file
# in the machine's first flash bank and QEMU_FW_VARS in the second, which is
# the layout an upstream ArmVirtQemu build comes in and the only way to run
# one, since such a build keeps its variables in that second bank.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# Reports under the name of the check that started this run, or under its own
# when it was started by hand
CHECK="${CHECK:-run}"
. tests/deploy/check.sh

QEMU="${QEMU:-../qemu/build/qemu-system-aarch64}"
QEMU_ACCEL="${QEMU_ACCEL:-tcg}"
QEMU_EL="${QEMU_EL:-2}"
QEMU_FW_EL2="${QEMU_FW_EL2:-../winemu/linaro_ovmf.fd}"
QEMU_FW_EL1="${QEMU_FW_EL1:-../winemu/edk2-nightly-armvirtqemu-aarch64-release-code.fd}"
QEMU_FW_LOAD="${QEMU_FW_LOAD:-kernel}"
QEMU_FW_VARS="${QEMU_FW_VARS:-}"
# The model, when one is asked for by name. It has no default here: which one
# a machine can be is the accel's answer and not this file's, and a default
# filled in at this point would be the tcg one -- so a run with QEMU_ACCEL=kvm
# and nothing else would ask a KVM machine for the tcg model and be refused
QEMU_CPU="${QEMU_CPU:-}"
QEMU_MEM="${QEMU_MEM:-4096}"
QEMU_SMP="${QEMU_SMP:-8,sockets=1,clusters=2,cores=4,threads=1}"
BUILD_DIR="${BUILD_DIR:-build}"
ESP="${ESP:-$BUILD_DIR/esp.img}"
SERIAL_SOCK="${SERIAL_SOCK:-$BUILD_DIR/serial.sock}"
SERIAL_LOG="${SERIAL_LOG:-$BUILD_DIR/serial.log}"
STOP_PATTERN="${STOP_PATTERN:-M2 done}"
ARM_PATTERN="${ARM_PATTERN:-}"
GRACE="${GRACE:-0}"
# A run that is going to be looked at rather than read: the machine is left
# running, with its screen on the usual display and its monitor on QMP_PORT,
# so that a picture can be taken of it.
KEEP="${KEEP:-}"
QMP_PORT="${QMP_PORT:-4444}"
# Where the monitor listens. The default keeps it to this machine, which is
# what a run beside the one looking at it wants; a run on another machine is
# read over the network instead, and names the address to bind there. The
# display is on every interface either way, for the same reason.
QMP_HOST="${QMP_HOST:-127.0.0.1}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"

# A firmware exception ends the run: waiting out the timeout after the guest
# has already died just wastes time and hides the reason.
# Overridable: a fault ends the run, which is right for a check but cuts
# the firmware's message short when the reason is what is wanted.
FAULT_PATTERN="${FAULT_PATTERN-Synchronous Exception}"

# Optional Windows volume. When given, the boot volume's startup.nsh finds it
# and chainloads its boot manager, so the driver sees a real boot. Read only:
# the image is never written to.
WIN_DISK="${WIN_DISK:-}"

# A volume of programs for the guest to run, attached as a virtio disk. The
# boot volume goes over USB, which the firmware brings up but a WinPE need not:
# a USB stack the image does not load leaves the volume invisible to the
# running guest, and the keyboard with it. A virtio disk needs no such thing.
#
# Read only, and with no boot index: it is a disk to read.
DEMO_DISK="${DEMO_DISK:-}"

# What the two choices above come to, worked out once so that the command line
# below reads as one list of devices rather than as a set of branches.
case "$QEMU_ACCEL" in
tcg)
    accel_args=(-accel tcg,thread=multi,tb-size=2048)
    qemu_cpu="${QEMU_CPU:-cortex-a76-nolrcpc}"
    ;;
kvm)
    accel_args=(-accel kvm)
    qemu_cpu="${QEMU_CPU:-host}"
    ;;
*)
    checkFail "QEMU_ACCEL must be tcg or kvm, not $QEMU_ACCEL"
    ;;
esac

case "$QEMU_EL" in
2)
    # EL3 is asked for so that the firmware has an EL2 below it to come up at,
    # which is the machine the driver was written for. What the guest is given
    # is then the firmware's to decide, and what it gives is EL2.
    machine_args=(-M virt,gic-version=3,virtualization=on,secure=on)
    QEMU_FW="${QEMU_FW:-$QEMU_FW_EL2}"
    ;;
1)
    # Neither level above the one the firmware is entered at, so nothing can
    # drop the guest to EL1: it is already there.
    machine_args=(-M virt,gic-version=3)
    QEMU_FW="${QEMU_FW:-$QEMU_FW_EL1}"
    ;;
*)
    checkFail "QEMU_EL must be 2 or 1, not $QEMU_EL"
    ;;
esac

case "$QEMU_FW_LOAD" in
kernel)
    fw_args=(-kernel "$QEMU_FW")
    ;;
pflash)
    if [ -z "$QEMU_FW_VARS" ]; then
        checkFail "QEMU_FW_LOAD=pflash needs QEMU_FW_VARS"
    fi
    fw_args+=(-drive "if=pflash,format=raw,unit=0,file=$QEMU_FW,readonly=on")
    fw_args+=(-drive "if=pflash,format=raw,unit=1,file=$QEMU_FW_VARS")
    ;;
*)
    checkFail "QEMU_FW_LOAD must be kernel or pflash, not $QEMU_FW_LOAD"
    ;;
esac

[ -f "$ESP" ] || checkFail "no image at $ESP (run make esp first)"
[ -f "$QEMU_FW" ] || checkFail "no firmware at $QEMU_FW"
if [ -n "$QEMU_FW_VARS" ] && [ ! -f "$QEMU_FW_VARS" ]; then
    checkFail "no variable store at $QEMU_FW_VARS"
fi
if [ -n "$WIN_DISK" ] && [ ! -f "$WIN_DISK" ]; then
    checkFail "no windows disk at $WIN_DISK"
    exit 1
fi

rm -f "$SERIAL_SOCK" "$SERIAL_LOG"

# The Windows volume is attached only when asked for, so the everyday case
# stays a single device boot. It is given the later boot index: the driver's
# own volume has to start first, because that is what loads the driver and
# then hands over.
win_args=()
if [ -n "$WIN_DISK" ]; then
    win_args+=(-device virtio-blk-pci,drive=win,bootindex=2)
    win_args+=(-drive "file=$WIN_DISK,if=none,format=qcow2,id=win,readonly=on")
fi

demo_args=()
if [ -n "$DEMO_DISK" ]; then
    demo_args+=(-device virtio-blk-pci,drive=demo)
    demo_args+=(-drive "file=$DEMO_DISK,if=none,format=raw,id=demo,readonly=on")
fi

# One controller for everything on it. virt has none of its own -- -usb adds
# nothing -- so every device needs a bus naming this one, and a second
# controller would only be another thing for the guest to bring up.
usb_args=(
    -device qemu-xhci,id=xhci
    -device usb-kbd,bus=xhci.0
    -device usb-tablet,bus=xhci.0
    -device usb-storage,bus=xhci.0,drive=esp,bootindex=1
    -drive "file=$ESP,if=none,format=raw,id=esp"
)

"$QEMU" \
    -no-reboot \
    "${accel_args[@]}" \
    "${machine_args[@]}" \
    -m "$QEMU_MEM" \
    -smp "$QEMU_SMP" \
    -cpu "$qemu_cpu" \
    "${fw_args[@]}" \
    -device ramfb \
    -vnc 0.0.0.0:0 \
    -gdb tcp::1234 \
    -qmp tcp:$QMP_HOST:$QMP_PORT,server=on,wait=off \
    "${win_args[@]}" \
    "${demo_args[@]}" \
    "${usb_args[@]}" \
    -serial "unix:$SERIAL_SOCK,server=on,wait=off" \
    -display none &
QEMU_PID=$!

# Stop QEMU whenever this script is interrupted.
trap 'kill "$QEMU_PID" 2>/dev/null || true' EXIT INT TERM

rc=0
arm_args=()
if [ -n "$ARM_PATTERN" ]; then
    arm_args+=(--arm "$ARM_PATTERN" --grace "$GRACE")
fi
python3 tests/deploy/serial_monitor.py "$SERIAL_SOCK" \
    --stop "$STOP_PATTERN|$FAULT_PATTERN" \
    "${arm_args[@]}" \
    --timeout "$BOOT_TIMEOUT" \
    --log "$SERIAL_LOG" || rc=$?

if [ -n "$KEEP" ]; then
    # Leaving it running means leaving it running: the exit trap would
    # otherwise kill the machine this mode exists to keep.
    trap - EXIT INT TERM
    checkNote "left running: display 5900, qmp $QMP_PORT, serial $SERIAL_LOG"
    exit 0
fi

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

# An exception is a failure whatever else the log contains, so it is checked
# before the expected marker.
if tests/deploy/plain.sh "$SERIAL_LOG" | grep -q "$FAULT_PATTERN"; then
    checkNote "where it was taken:"
    tests/deploy/plain.sh "$SERIAL_LOG" | grep -n "$FAULT_PATTERN" | head -3
    checkFail "the guest took an exception"
fi

if [ "$rc" -eq 0 ]; then
    checkPass "saw $STOP_PATTERN"
elif [ "$rc" -eq 2 ]; then
    checkFail "qemu exited before $STOP_PATTERN"
else
    checkFail "never saw $STOP_PATTERN"
fi
