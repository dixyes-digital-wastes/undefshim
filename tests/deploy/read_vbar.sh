#!/usr/bin/env bash
# Read VBAR_EL1 from the running guest, after the kernel has taken over.
set -uo pipefail
cd "$(dirname "$0")/../.."

WORK=build/vbar
mkdir -p "$WORK"
LOG="$WORK/serial.log"
rm -f "$LOG"

ESP="${ESP:-build/vbar/run.img}"
if [ ! -f "$ESP" ]; then
    mkdir -p "$WORK"
    cat > "$WORK/run.toml" <<EOF
version = 1

[log]
level = "info"

[debug]
enabled = true
arm = true
EOF
    CONFIG="$WORK/run.toml" ESP="$ESP" tests/deploy/build_esp.sh >/dev/null 2>&1 || {
        echo "could not build the volume"; exit 1; }
fi

nohup ../qemu/build/qemu-system-aarch64 \
    -no-reboot -accel tcg,thread=multi,tb-size=2048 \
    -M virt,gic-version=3,virtualization=on,secure=on \
    -m 4096 -smp "${QEMU_SMP:-8,sockets=1,clusters=2,cores=4,threads=1}" \
    -cpu cortex-a76-nolrcpc \
    -kernel ../winemu/linaro_ovmf.fd \
    -device ramfb -vnc 0.0.0.0:0 \
    -qmp tcp:127.0.0.1:4444,server=on,wait=off \
    -gdb tcp::1234 \
    -device qemu-xhci,id=xhci \
    -device usb-storage,drive=esp,bootindex=1 \
    -drive "file=$ESP,if=none,format=raw,id=esp" \
    -device virtio-blk-pci,drive=win,bootindex=2 \
    -drive file=../winemu/files/winpe_26100.qcow2,if=none,format=qcow2,id=win,readonly=on \
    -serial "file:$LOG" -display none >/dev/null 2>&1 &

QEMU_PID=$!
trap 'kill $QEMU_PID 2>/dev/null || true' EXIT INT TERM

for _ in $(seq 1 120); do
    grep -q 'US-M6.5-ARMED' "$LOG" 2>/dev/null && break
    sleep 2
done
echo "armed; waiting ${SETTLE:-90}s for the kernel to run"
sleep "${SETTLE:-90}"

# Read the pool's entry record instead of the registers: gdb cannot reach the
# system registers on this target, but the pool is ordinary memory.
python3 - "$LOG" 4444 <<'PY'
import json, re, socket, sys

log = open(sys.argv[1], "rb").read().decode("latin1")
m = re.search(r"pool: pa=0x([0-9a-f]+)", log)
if not m:
    raise SystemExit("no pool address in the log")
pool = int(m.group(1), 16)

s = socket.create_connection(("127.0.0.1", int(sys.argv[2])), timeout=10)
f = s.makefile("rwb")
f.readline()


def cmd(o):
    f.write((json.dumps(o) + "\n").encode())
    f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            return r


def hmp(c):
    return cmd({"execute": "human-monitor-command",
                "arguments": {"command-line": c}}).get("return", "")


cmd({"execute": "qmp_capabilities"})
out = hmp("xp /4gx 0x%x" % (pool + 8))
print("pool entry:", re.findall(r"0x[0-9a-f]+", out.split(":", 1)[1]))
regs = hmp("info registers")
mm = re.search(r"PC=([0-9a-f]+)", regs)
pc = int(mm.group(1), 16) if mm else 0
print("pc=0x%x  (kbase is not known here, but the pool is)" % pc)
PY
