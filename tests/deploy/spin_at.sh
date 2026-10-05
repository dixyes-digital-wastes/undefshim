#!/usr/bin/env bash
#
# How far does the kernel get?
#
# The question this answers is a binary one: does the machine reach a given
# kernel RVA, or not. A spin (`b .`) is written there with the debug patch
# table, and if the kernel ever executes it the machine stops there and stays
# stopped, which the CPU's PC then reports.
#
# That matters because the interesting things all happen inside a boot nobody
# can watch from outside. The kernel rebuilds the address space, and what is
# mapped where afterwards decides whether anything we placed can still be
# reached. Reading the PC says whether a given point was reached; asking the
# page tables about a specific address says what it maps to now.
#
# QEMU can answer both, and neither needs a debugger: gva2gpa walks the page
# tables of whatever address space the current CPU is in.
#
# Usage: spin_at.sh <kernel-rva> [settle-seconds] [probe-address...]
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
WORK="$BUILD_DIR/spin"
RVA="${1:?usage: spin_at.sh <kernel-rva> [settle] [probe...]}"
SETTLE="${2:-90}"
shift 2 2>/dev/null || shift 1 2>/dev/null || true
PROBES=("$@")

QEMU="${QEMU:-../qemu/build/qemu-system-aarch64}"
QEMU_FW="${QEMU_FW:-../winemu/linaro_ovmf.fd}"
QEMU_CPU="${QEMU_CPU:-cortex-a76-nolrcpc}"
QEMU_MEM="${QEMU_MEM:-4096}"
QEMU_SMP="${QEMU_SMP:-8,sockets=1,clusters=2,cores=4,threads=1}"
WIN_DISK="${WIN_DISK:-}"

[ -f "$WIN_DISK" ] || { echo "WIN_DISK not set or missing, skipping"; exit 0; }

mkdir -p "$WORK"
name=$(printf '%s' "$RVA" | tr -d 'x')
esp="$WORK/$name.img"
log="$WORK/$name.log"

cat > "$WORK/$name.toml" <<EOF
version = 1

[log]
level = "verbose"

[debug]
enabled = true

[[debug.patch]]
target = "ntoskrnl"
rva    = $RVA
value  = 0x14000000
width  = 4
tag    = "spin at $RVA"
EOF

CONFIG="$WORK/$name.toml" ESP="$esp" tests/deploy/build_esp.sh >"$WORK/$name.esp" 2>&1 || {
    echo "FAIL: could not build the volume"
    cat "$WORK/$name.esp"
    exit 1
}

rm -f "$log"
"$QEMU" \
    -no-reboot -accel tcg,thread=multi,tb-size=2048 \
    -M virt,gic-version=3,virtualization=on,secure=on \
    -m "$QEMU_MEM" -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -kernel "$QEMU_FW" -device ramfb -vnc 0.0.0.0:0 \
    -qmp tcp:127.0.0.1:4444,server=on,wait=off \
    -device qemu-xhci,id=xhci \
    -device usb-storage,drive=esp,bootindex=1 \
    -drive file="$esp",if=none,format=raw,id=esp \
    -device virtio-blk-pci,drive=win,bootindex=2 \
    -drive "file=$WIN_DISK,if=none,format=qcow2,id=win,readonly=on" \
    -serial file:"$log" -display none >/dev/null 2>&1 &
qpid=$!
trap 'kill $qpid 2>/dev/null || true' EXIT INT TERM

# The patch itself reports when it lands, which is the last thing the serial
# will say: everything after it happens on the kernel's side, where there is
# no console.
for _ in $(seq 1 300); do
    tests/deploy/plain.sh "$log" | grep -q 'M4 patched' 2>/dev/null && break
    sleep 1
done
if ! tests/deploy/plain.sh "$log" | grep -q 'M4 patched' 2>/dev/null; then
    echo "FAIL: the spin was never written"
    tests/deploy/plain.sh "$log" | grep -aE 'patch|gmm|milestone' | tail -5
    exit 1
fi

sleep "$SETTLE"

python3 - "$RVA" "${PROBES[@]}" <<'PY'
import json, socket, sys

# The kernel is placed differently every boot, so an address worth looking at
# is only expressible relative to something this run established. Probes can
# say what that is:
#
#   k+0x...    offset from the kernel base, derived from the PC when the spin
#              is inside the kernel image
#   pa:0x...   the physical address of a virtual address, used in reverse to
#              turn a physical address back into one
#   x20+0x...  offset from a register's value, which is how a spin somewhere
#              other than the kernel -- a loader, say -- still reaches kernel
#              addresses: the register it was about to jump to holds them
def parse(spec, base, delta, regs):
    if spec.startswith("k+"):
        return (None if base is None else base + int(spec[2:], 16)), delta
    if spec.startswith("pa:"):
        return (None if delta is None else int(spec[3:], 16) + delta), delta
    for name, value in regs.items():
        if spec.startswith(name + "+"):
            return value + int(spec[len(name) + 1:], 16), delta
    return int(spec, 16), delta

rva = int(sys.argv[1], 16)
raw = sys.argv[2:]

s = socket.create_connection(("127.0.0.1", 4444), timeout=10)
f = s.makefile("rw")
f.readline()
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()

def mon(cmd):
    f.write(json.dumps({"execute": "human-monitor-command",
                        "arguments": {"command-line": cmd}}) + "\n")
    f.flush()
    return json.loads(f.readline()).get("return", "")

regs = mon("info registers")
pc = None
values = {}
for line in regs.splitlines():
    line = line.strip()
    if line.startswith("PC="):
        for field in line.split():
            if "=" not in field:
                continue
            name, _, value = field.partition("=")
            try:
                v = int(value, 16)
            except ValueError:
                continue
            values[name.lower()] = v
            if name == "PC":
                pc = v

if pc is None:
    print("FAIL: no PC in the register dump")
    sys.exit(1)

reached = (pc & 0xfff) == (rva & 0xfff) and (pc - rva) & 0xfffff == 0
base = (pc - rva) if reached else None

print("pc  = 0x%x" % pc)
if reached:
    print("verdict: REACHED (kernel base 0x%x)" % base)
else:
    print("verdict: not the kernel; base would be 0x%x (misaligned)" % (pc - rva))

delta = None
for spec in raw:
    a, delta = parse(spec, base, delta, values)
    if a is None:
        print("%-22s -> (needs a base, a register, or a va->pa probe first)" % spec)
        continue
    out = mon("gva2gpa 0x%x" % a).strip().replace("\r", "")
    print("%-22s -> 0x%-16x %s" % (spec, a, out))
    # A successful translation fixes the physical map's delta for the rest of
    # the run, which is what lets a physical address be turned back into one.
    if out.startswith("gpa: 0x"):
        delta = a - int(out.split("0x")[1], 16)
        print("%-22s    (delta 0x%x)" % ("", delta))
PY

kill $qpid 2>/dev/null || true
