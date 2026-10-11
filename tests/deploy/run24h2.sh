#!/bin/sh
# Start the full 24H2 install under QEMU, with the shim's volume in front of it.
#
# Runs on the machine that holds the images. Everything a run writes goes in a
# directory of its own: the overlay on the install and the serial log. The
# overlay is left writable, because this run does a first boot and an OOBE and
# both write to the disk; the install itself is the overlay's backing file, so
# a run still cannot change what is under it, and a run that goes wrong costs a
# restart and not a reinstall.
#
# The device list is the one tests/deploy/run.sh builds, because the
# arrangement is the same: the shim's volume boots first (its own ESP is the
# UEFI shell plus a startup.nsh that loads the driver and chainloads
# bootmgfw.efi), and the Windows volume boots second.
#
# Usage: run24h2.sh [-n NAME] [-a tcg|kvm] [-g PORT|none] [-S] [-v]
#
#   -n NAME   the run directory, under RUNS; default run
#   -a        default kvm. OOBE does not finish under tcg on any machine
#   -g PORT   open a gdbstub on PORT; default 1234, `none` for no stub
#   -S        start paused, so gdb can set breakpoints before the first
#             instruction
#   -i        also give the guest a keyboard and a tablet, on the same USB
#             controller. The install has no virtio input and this is the only
#             way to answer a prompt or move the pointer, which is what tells
#             "stopped" apart from "slow". Off by default so a run without it
#             is the same machine an earlier run without it used
#   -v        print the command line and nothing else, to see it without
#             starting anything
set -eu

BASE_DIR="${BASE_DIR:-path/to}"
RUNS="${RUNS:-$BASE_DIR/winemu/runs}"
QEMU="${QEMU:-/usr/bin/qemu-system-aarch64}"
INSTALL="${INSTALL:-$BASE_DIR/arm64_24h2.qcow2}"
ESP_SOURCE="${ESP_SOURCE:-$BASE_DIR/undefshim-ex2/build/esp.img}"
# The firmware packaged in the Linux image format, which -kernel is what loads.
# It is the one the KVM runs were made with: with no secure world asked for,
# QEMU gives the guest no level above EL1, and this firmware comes up there.
FIRMWARE="${FIRMWARE:-$BASE_DIR/winemu/linaro_ovmf.fd}"
MEM="${MEM:-4096}"
SMP="${SMP:-8,sockets=1,clusters=2,cores=4,threads=1}"

NAME=run
ACCEL=kvm
GDB_PORT=1234
PAUSE=""
SHOW=""
INPUT=""
while getopts "n:a:g:iSv" opt; do
    case "$opt" in
        n) NAME="$OPTARG" ;;
        a) ACCEL="$OPTARG" ;;
        g) GDB_PORT="$OPTARG" ;;
        i) INPUT=1 ;;
        S) PAUSE="-S" ;;
        v) SHOW=1 ;;
        *) echo "usage: $0 [-n name] [-a tcg|kvm] [-g port|none] [-i] [-S] [-v]" >&2
           exit 2 ;;
    esac
done

RUN="$RUNS/$NAME"
mkdir -p "$RUN"

# The install is an overlay, and it is also given read-only
if [ ! -f "$RUN/disk.qcow2" ]; then
    /usr/bin/qemu-img create -f qcow2 -b "$INSTALL" -F qcow2 "$RUN/disk.qcow2" \
        > /dev/null
fi
cp "$ESP_SOURCE" "$RUN/esp.img"

case "$ACCEL" in
    kvm)
        ACCEL_ARGS="-accel kvm"
        CPU="host"
        ;;
    tcg)
        # Multi-threaded because the guest is a whole system, and the model
        # with no RCpc in it so the shim has something to emulate
        ACCEL_ARGS="-accel tcg,thread=multi,tb-size=2048"
        CPU="cortex-a76-nolrcpc"
        ;;
    *)
        echo "-a takes kvm or tcg" >&2
        exit 2
        ;;
esac

SERIAL="$RUN/serial.sock"
LOG="$RUN/serial.log"
rm -f "$SERIAL" "$LOG"

if [ "$GDB_PORT" = none ]; then
    GDB_ARGS=""
else
    GDB_ARGS="-gdb tcp::$GDB_PORT"
fi

# A run without -i carries no input device, so the guest has no way to answer a
# prompt. That is deliberate: it keeps every run comparable to the ones made
# before the switch existed
#
# The input devices go after the two storage devices, and that order is load
# bearing. QEMU gives an xhci device the first free port in the order the
# devices are defined, and the firmware names the boot option after the port
# it found the drive on. Put the keyboard first and the two drives move down a
# port each; the option the firmware then boots is the install, not this
# volume, and no part of the shim ever runs
if [ -n "$INPUT" ]; then
    INPUT_ARGS="-device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0"
else
    INPUT_ARGS=""
fi

# One controller, and both volumes on it. The install image has no virtio
# driver in it, so a virtio-blk disk would be one the guest cannot see; USB
# storage is what the firmware brings up itself, and what the shim's volume
# already goes over. The shim's volume boots first: it loads the driver, and
# the driver hands over to the Windows volume that boots second
CMD="$QEMU \
    -no-reboot \
    -machine virt,gic-version=3 \
    $ACCEL_ARGS \
    -cpu $CPU \
    -m $MEM \
    -smp $SMP \
    -kernel $FIRMWARE \
    -device ramfb \
    -vnc 0.0.0.0:0 \
    $GDB_ARGS $PAUSE \
    -qmp tcp:127.0.0.1:4447,server=on,wait=off \
    -device qemu-xhci,id=xhci \
    -device usb-storage,bus=xhci.0,drive=win,bootindex=2 \
    -drive file=$RUN/disk.qcow2,if=none,format=qcow2,id=win \
    -device usb-storage,bus=xhci.0,drive=esp,bootindex=1 \
    -drive file=$RUN/esp.img,if=none,format=raw,id=esp \
    $INPUT_ARGS \
    -serial unix:$SERIAL,server=on,wait=off \
    -display none"

if [ -n "$SHOW" ]; then
    echo "$CMD"
    exit 0
fi

echo "run:      $RUN"
echo "disk:     $RUN/disk.qcow2 (overlay on $(basename "$INSTALL"), read-only)"
echo "esp:      $RUN/esp.img"
echo "firmware: $(basename "$FIRMWARE") via -kernel"
echo "accel:    $ACCEL, cpu $CPU, $MEM MiB, $SMP"
if [ -n "$GDB_ARGS" ]; then
    echo "gdb:      target remote :$GDB_PORT"
fi
echo "vnc:      :0  (5900)"
echo "serial:   $LOG  (socket $SERIAL; read it with socat - UNIX-CONNECT:$SERIAL)"
echo

exec $CMD
