#!/usr/bin/env bash
#
# What is still reachable once the kernel is running.
#
# Everything placed at boot is placed at an address that only exists during
# boot: the pool is identity mapped, and the payload is copied to whatever
# physical memory the firmware handed out. The kernel then builds its own
# address space, and whether anything we placed can still be reached from
# there is the question the whole arrangement depends on.
#
# The machine is left running rather than stopped at a marker, and QEMU is
# asked directly: gva2gpa walks the page tables of whatever address space the
# current CPU is in, so it answers for the address space that is actually in
# force, not for one reconstructed from a dump.
#
# What this does not establish is permissions. gva2gpa says whether an address
# translates, not whether it is writable or executable, so a mapping that
# answers here may still fault on the first store or the first instruction
# fetched from it.
#
# Usage: identity_probe.sh [extra-address...]
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
WORK="$BUILD_DIR/probe"
ESP="${ESP:-$BUILD_DIR/esp.img}"
LOG="${LOG:-$WORK/identity.log}"
SETTLE="${SETTLE:-60}"
PROBES=("$@")

QEMU="${QEMU:-../qemu/build/qemu-system-aarch64}"
QEMU_FW="${QEMU_FW:-../winemu/linaro_ovmf.fd}"
QEMU_CPU="${QEMU_CPU:-cortex-a76-nolrcpc}"
QEMU_MEM="${QEMU_MEM:-4096}"
QEMU_SMP="${QEMU_SMP:-8,sockets=1,clusters=2,cores=4,threads=1}"
QMP_PORT="${QMP_PORT:-4444}"
WIN_DISK="${WIN_DISK:-}"

[ -f "$ESP" ] || { echo "missing esp: $ESP (run make esp first)" >&2; exit 1; }
[ -f "$WIN_DISK" ] || { echo "WIN_DISK not set or missing, skipping"; exit 0; }

mkdir -p "$WORK"
rm -f "$LOG"

"$QEMU" \
    -no-reboot -accel tcg,thread=multi,tb-size=2048 \
    -M virt,gic-version=3,virtualization=on,secure=on \
    -m "$QEMU_MEM" -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -kernel "$QEMU_FW" -device ramfb -vnc 0.0.0.0:0 \
    -qmp "tcp:127.0.0.1:$QMP_PORT,server=on,wait=off" \
    -device qemu-xhci,id=xhci \
    -device usb-storage,drive=esp,bootindex=1 \
    -drive file="$ESP",if=none,format=raw,id=esp \
    -device virtio-blk-pci,drive=win,bootindex=2 \
    -drive "file=$WIN_DISK,if=none,format=qcow2,id=win,readonly=on" \
    -serial "file:$LOG" -display none >/dev/null 2>&1 &
qpid=$!
trap 'kill $qpid 2>/dev/null || true' EXIT INT TERM

# The driver reports where it put things, and that report is the last thing
# the serial carries: everything after it happens on the kernel's side.
for _ in $(seq 1 300); do
    tests/deploy/plain.sh "$LOG" | grep -q 'pool: pa=' 2>/dev/null && break
    sleep 1
done
if ! tests/deploy/plain.sh "$LOG" | grep -q 'pool: pa=' 2>/dev/null; then
    echo "FAIL: the driver never reported a pool"
    tests/deploy/plain.sh "$LOG" | tail -5
    exit 1
fi

# Give the handover time to happen. The kernel starts a few seconds after the
# chainload on TCG, and there is no marker from inside it.
sleep "$SETTLE"

python3 - "$LOG" "$QMP_PORT" "${PROBES[@]}" <<'PY'
import json, re, socket, sys

log_path, port, probes = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
log = open(log_path, "rb").read().decode("latin1")


def find(pattern, what):
    m = re.search(pattern, log)
    if not m:
        raise SystemExit("FAIL: no %s in the log" % what)
    return int(m.group(1), 16)


pool_pa = find(r"pool: pa=0x([0-9a-f]+)", "pool address")
pool_va = find(r"pool: pa=0x[0-9a-f]+ va=0x([0-9a-f]+)", "pool address")
payload_va = find(r"payload: at 0x([0-9a-f]+)", "payload address")
payload_bytes = int(re.search(r"payload: at 0x[0-9a-f]+ bytes=([0-9]+)", log).group(1))

conn = socket.create_connection(("127.0.0.1", port), timeout=10)
f = conn.makefile("rwb")
f.readline()


def cmd(obj):
    f.write((json.dumps(obj) + "\n").encode())
    f.flush()
    while True:
        line = f.readline()
        if not line:
            raise SystemExit("FAIL: qmp closed")
        r = json.loads(line)
        if "return" in r or "error" in r:
            return r


def hmp(command):
    return cmd({"execute": "human-monitor-command",
                "arguments": {"command-line": command}}).get("return", "")


cmd({"execute": "qmp_capabilities"})

regs = hmp("info registers")
pc = re.search(r"PC=([0-9a-f]+)", regs)
print("pc  = 0x%s" % pc.group(1) if pc else "pc  = ?")

print("pool     pa=0x%x va=0x%x" % (pool_pa, pool_va))
print("payload  va=0x%x bytes=%u" % (payload_va, payload_bytes))

targets = [("pool identity", pool_pa), ("payload", payload_va)]
targets += [(("probe %s" % p), int(p, 16)) for p in probes]

for name, va in targets:
    answer = hmp("gva2gpa 0x%x" % va).strip()
    print("%-16s va=0x%-12x %s" % (name, va, answer or "(no answer)"))
PY
