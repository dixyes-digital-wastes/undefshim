#!/usr/bin/env bash
#
# Where do undefined instructions go once the kernel is running?
#
# With the replacement off, every RCpc load faults, and the question this
# answers is where each fault is delivered: to the table this project patched,
# or to the kernel's own handler. The exception log QEMU keeps with -d int
# records the PC each exception is taken to, so the answer is in the log
# without instrumenting anything.
#
# Usage: trap_probe.sh [seconds]
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

WORK=build/trap
SECONDS_TO_RUN="${1:-300}"
WIN_DISK="${WIN_DISK:-../winemu/files/winpe_26100.qcow2}"

mkdir -p "$WORK"
ESP="$WORK/probe.img"

cat > "$WORK/probe.toml" <<EOF
version = 1

[log]
level = "verbose"

[debug]
enabled = true
arm = true
EOF

CONFIG="$WORK/probe.toml" ESP="$ESP" tests/deploy/build_esp.sh >/dev/null || exit 1

rm -f "$WORK/serial.log" "$WORK/int.log"
setsid ../qemu/build/qemu-system-aarch64 \
    -no-reboot -accel tcg,thread=multi,tb-size=2048 \
    -M virt,gic-version=3,virtualization=on,secure=on \
    -m 4096 -smp 8,sockets=1,clusters=2,cores=4,threads=1 \
    -cpu cortex-a76-nolrcpc \
    -kernel ../winemu/linaro_ovmf.fd \
    -device ramfb -vnc 0.0.0.0:0 -display none \
    -qmp tcp:127.0.0.1:4445,server=on,wait=off \
    -device qemu-xhci,id=xhci \
    -device usb-storage,drive=esp,bootindex=1 \
    -drive "file=$ESP,if=none,format=raw,id=esp" \
    -device virtio-blk-pci,drive=win,bootindex=2 \
    -drive "file=$WIN_DISK,if=none,format=qcow2,id=win,readonly=on" \
    -serial "file:$WORK/serial.log" \
    -d int -D "$WORK/int.log" >/dev/null 2>&1 &
QPID=$!
trap 'kill -TERM -"$QPID" 2>/dev/null || true' EXIT INT TERM

sleep "$SECONDS_TO_RUN"
kill -TERM -"$QPID" 2>/dev/null || true
sleep 2
kill -KILL -"$QPID" 2>/dev/null || true

echo "serial: $(tests/deploy/plain.sh "$WORK/serial.log" | grep -c . 2>/dev/null || echo 0) lines"
echo "int log: $(wc -l < "$WORK/int.log" 2>/dev/null || echo 0) lines"

python3 - "$WORK/int.log" "$WORK/serial.log" <<'PY'
import re, sys

raw = open(sys.argv[1], "rb").read().decode("latin1", "replace")
log = open(sys.argv[2], "rb").read().decode("latin1", "replace")

kbase = None
m = re.search(r"gmm: ntoskrnl found at 0x([0-9a-f]+)", log)
if m:
    kbase = int(m.group(1), 16)

# Every exception QEMU took, with the PC it was delivered to.
records = re.findall(
    r"Taking exception \d+ \[([^\]]+)\] on CPU (\d+)\n"
    r"(?:\.\.\..*\n)*?"
    r"\.\.\.to EL1 PC ([0-9a-f]+)", raw)

counts = {}
for kind, cpu, pc in records:
    counts[kind] = counts.get(kind, 0) + 1
print("exception kinds:", counts)

undefined = [(cpu, int(pc, 16)) for kind, cpu, pc in records if kind == "Undefined Instruction"]
print("undefined instructions taken:", len(undefined))

if kbase is not None:
    print("kernel base = 0x%x" % kbase)
    # The synchronous slot this project patches, and the handler the kernel
    # installs there, as offsets into the kernel.
    ours = kbase + 0x604a00
    theirs = kbase + 0x605c78
    toOurs = sum(1 for _, pc in undefined if pc == ours)
    toTheirs = sum(1 for _, pc in undefined if pc == theirs)
    other = [pc for _, pc in undefined if pc not in (ours, theirs)]
    print("to our stub     (kbase+0x604a00): %d" % toOurs)
    print("to their handler(kbase+0x605c78): %d" % toTheirs)
    if other:
        seen = {}
        for pc in other:
            seen[pc] = seen.get(pc, 0) + 1
        print("elsewhere:")
        for pc, n in sorted(seen.items(), key=lambda kv: -kv[1])[:8]:
            print("  0x%x (%+d from base, %d times)" % (pc, pc - kbase, n))
PY
