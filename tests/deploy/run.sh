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
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

QEMU="${QEMU:-../qemu/build/qemu-system-aarch64}"
QEMU_FW="${QEMU_FW:-../winemu/linaro_ovmf.fd}"
QEMU_CPU="${QEMU_CPU:-cortex-a76-nolrcpc}"
QEMU_MEM="${QEMU_MEM:-4096}"
QEMU_SMP="${QEMU_SMP:-8,sockets=1,clusters=2,cores=4,threads=1}"
BUILD_DIR="${BUILD_DIR:-build}"
ESP="${ESP:-$BUILD_DIR/esp.img}"
SERIAL_SOCK="${SERIAL_SOCK:-$BUILD_DIR/serial.sock}"
SERIAL_LOG="${SERIAL_LOG:-$BUILD_DIR/serial.log}"
STOP_PATTERN="${STOP_PATTERN:-US-M2-DONE}"
ARM_PATTERN="${ARM_PATTERN:-}"
GRACE="${GRACE:-0}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"

# A firmware exception ends the run: waiting out the timeout after the guest
# has already died just wastes time and hides the reason.
FAULT_PATTERN="Synchronous Exception"

# Optional Windows volume. When given, the boot volume's startup.nsh finds it
# and chainloads its boot manager, so the driver sees a real boot. Read only:
# the image is never written to.
WIN_DISK="${WIN_DISK:-}"

[ -f "$ESP" ] || { echo "missing esp: $ESP (run make esp first)" >&2; exit 1; }
[ -f "$QEMU_FW" ] || { echo "missing firmware: $QEMU_FW" >&2; exit 1; }
if [ -n "$WIN_DISK" ] && [ ! -f "$WIN_DISK" ]; then
    echo "missing windows disk: $WIN_DISK" >&2
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

"$QEMU" \
    -no-reboot \
    -accel tcg,thread=multi,tb-size=2048 \
    -M virt,gic-version=3,virtualization=on,secure=on \
    -m "$QEMU_MEM" \
    -smp "$QEMU_SMP" \
    -cpu "$QEMU_CPU" \
    -kernel "$QEMU_FW" \
    -device ramfb \
    -vnc 0.0.0.0:0 \
    -gdb tcp::1234 \
    -qmp tcp:127.0.0.1:4444,server=on,wait=off \
    "${win_args[@]}" \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-storage,drive=esp,bootindex=1 \
    -drive file="$ESP",if=none,format=raw,id=esp \
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

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

# An exception is a failure whatever else the log contains, so it is checked
# before the expected marker.
if grep -q "$FAULT_PATTERN" "$SERIAL_LOG"; then
    echo "FAIL: the guest took an exception"
    grep -n "$FAULT_PATTERN" "$SERIAL_LOG" | head -3
    exit 1
fi

if [ "$rc" -eq 0 ]; then
    echo "PASS: saw $STOP_PATTERN"
elif [ "$rc" -eq 2 ]; then
    echo "FAIL: QEMU exited before $STOP_PATTERN"
    exit 1
else
    echo "FAIL: never saw $STOP_PATTERN"
    exit 1
fi
