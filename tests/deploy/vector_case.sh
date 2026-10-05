#!/usr/bin/env bash
#
# Does a synchronous exception reach the payload, and does the kernel carry on?
#
# The milestone is not "the payload runs": it is that the kernel runs, with the
# instructions this hardware does not have carried out for it. Both halves are
# checked, because either alone is an easy thing to mistake for success. A
# payload that is entered and never returns stops the machine somewhere that
# looks like progress; a machine that carries on without ever entering the
# payload has not been helped at all.
#
# None of this can be read from the serial log. Once the kernel is running its
# page tables do not map the serial port, so the payload cannot print, and the
# first write to the port stops the machine. What the payload does instead is
# leave a record in the pool, which the host reads back through the monitor.
#
# The strongest of the checks is that the kernel is still the code that is
# running afterwards. A register restored wrongly does not show up here as a
# wrong value -- the machine simply faults a few instructions later, in code
# that has nothing to do with this.
#
# The Windows disk is external, so this is skipped unless WIN_DISK is set.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
WORK="$BUILD_DIR/vector-case"
WIN_DISK="${WIN_DISK:-}"
# Where the loader's synchronous slot is, as an offset into the loader. The
# machine stops here when nothing is armed.
LOADER_SYNC_SLOT="${LOADER_SYNC_SLOT:-0x1a00}"
SETTLE="${SETTLE:-90}"
QMP_PORT="${QMP_PORT:-4444}"

if [ -z "$WIN_DISK" ]; then
    echo "WIN_DISK not set, skipping"
    exit 0
fi
if [ ! -f "$WIN_DISK" ]; then
    echo "missing windows disk: $WIN_DISK" >&2
    exit 1
fi

mkdir -p "$WORK"
cat > "$WORK/armed.toml" <<EOF

[log]
level = "verbose"

[debug]
enabled = true
arm = true

[scan]
# The exception path is what this checks, so the replacement that would keep
# the instructions from faulting in the first place is turned off. With it on
# there is nothing to take over and nothing to enter.
ldaprRewrite = false
EOF

log="$WORK/armed.log"
if ! CONFIG="$WORK/armed.toml" ESP="$WORK/armed.img" tests/deploy/build_esp.sh \
        >"$WORK/armed.esp" 2>&1; then
    echo "FAIL: could not build the volume"
    cat "$WORK/armed.esp"
    exit 1
fi

# The run is not stopped by a marker: the payload is entered long after the
# last thing the driver can say, so the machine is left running and then asked
# directly.
# Its own socket as well as its own log. run.sh shares one socket path by
# default, and a socket left behind by an interrupted run is then the socket
# the next run talks to, or cannot talk to at all -- which looks like a guest
# that produced no output.
#
# Its own process group, so that everything it starts goes away with it. An
# earlier version killed the pid the shell reported, which is the subshell the
# environment prefix creates rather than run.sh itself: the run survived, kept
# the serial socket, and the next check talked to it instead of to its own
# machine.
setsid env ESP="$WORK/armed.img" SERIAL_LOG="$log" SERIAL_SOCK="$WORK/armed.sock" \
    WIN_DISK="$WIN_DISK" \
    STOP_PATTERN='vector-case-never-matches' BOOT_TIMEOUT=$((SETTLE + 300)) \
    tests/deploy/run.sh >"$WORK/armed.run" 2>&1 &
runpid=$!

stop() {
    kill -TERM -"$runpid" 2>/dev/null || true
    sleep 1
    kill -KILL -"$runpid" 2>/dev/null || true
    pkill -f "file=$WORK/armed.img" 2>/dev/null || true
}
trap stop EXIT INT TERM

# Arming waits for the kernel to be found, which is a wait in the boot rather
# than a step the driver takes.
armed=no
for _ in $(seq 1 $((SETTLE + 240))); do
    if tests/deploy/plain.sh "$log" | grep -q 'M6.5 armed' 2>/dev/null; then
        armed=yes
        break
    fi
    if tests/deploy/plain.sh "$log" | grep -qaE 'M5 incomplete|nothing taken over' 2>/dev/null; then
        break
    fi
    sleep 1
done
if [ "$armed" != "yes" ]; then
    echo "FAIL: the vector table was never taken over"
    tests/deploy/plain.sh "$log" | grep -aE 'arm:|plan: vbar|M5' | tail -10
    exit 1
fi
tests/deploy/plain.sh "$log" | grep -a 'arm: vbar' | tail -2

# The replacement has to be off, or nothing faults and the exception path has
# nothing to do. A key read from the wrong configuration section looks exactly
# like a check that found nothing, so it is asserted rather than assumed: that
# mistake has already been made once.
if ! tests/deploy/plain.sh "$log" | grep -q '^config: level=[0-9]* rewrite=0 '; then
    echo "FAIL: the replacement is on, so this checks nothing"
    tests/deploy/plain.sh "$log" | grep -a '^config: ' | head -2
    exit 1
fi

sleep "$SETTLE"

python3 - "$log" "$QMP_PORT" "$LOADER_SYNC_SLOT" <<'PY'
import json, re, socket, sys

log_path, port, loader_sync = sys.argv[1], int(sys.argv[2]), int(sys.argv[3], 16)
log = open(log_path, "rb").read().decode("latin1")

m = re.search(r"pool: pa=0x([0-9a-f]+)", log)
if not m:
    raise SystemExit("FAIL: no pool address in the log")
pool_pa = int(m.group(1), 16)

loader_base = None
for pat in (r"winload base 0x([0-9a-f]+)", r"gmm: winload found at 0x([0-9a-f]+)"):
    mm = re.search(pat, log)
    if mm:
        loader_base = int(mm.group(1), 16)
        break

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
mm = re.search(r"PC=([0-9a-f]+)", regs)
pc = int(mm.group(1), 16) if mm else 0

# The record starts after the pool header's magic and slot count. It is read as
# words rather than through a structure: this is the host looking at memory,
# and the layout is the payload's business.
WORDS = 15 + 4 * 38
pool = []
for i in range(WORDS):
    out = hmp("xp /1gx 0x%x" % (pool_pa + 8 + i * 8))
    wm = re.search(r":\s*(0x[0-9a-f]+)", out)
    pool.append(int(wm.group(1), 16) if wm else 0)

magic, entries, handled = pool[0], pool[1], pool[2]
lastEsr, lastElr, lastFar = pool[3], pool[4], pool[5]
lastInsn = pool[8]
emuInsn, emuAddr, emuValue = pool[9], pool[10], pool[11]
emuX9 = pool[14]

print("pc=0x%x pool=0x%x" % (pc, pool_pa))
print("record magic=0x%x entries=%u handled=%u" % (magic, entries, handled))
print("       last insn=0x%08x ec=0x%x elr=0x%x far=0x%x"
      % (lastInsn, (lastEsr >> 26) & 0x3f, lastElr, lastFar))
print("       first emulated insn=0x%08x addr=0x%x value=0x%x x9=0x%x"
      % (emuInsn, emuAddr, emuValue, emuX9))
if loader_base is not None:
    print("loader base=0x%x, synchronous slot=0x%x"
          % (loader_base, loader_base + loader_sync))

fail = []

if magic != 0x5952544E55504355:
    fail.append("the payload left no record in the pool")
if entries == 0:
    fail.append("the payload was never entered")
if handled != entries:
    fail.append("the payload was entered %u times and claimed %u: it is "
                "refusing exceptions it should be carrying out" % (entries, handled))
if loader_base is not None and pc == loader_base + loader_sync:
    fail.append("still stopped at the loader's synchronous slot, so nothing "
                "that runs went through the slot")

# The payload runs out of the pool. If the machine is sitting there, an
# exception was taken and never came back from.
POOL_LO, POOL_HI = pool_pa, pool_pa + 0x21000
if POOL_LO <= pc < POOL_HI:
    fail.append("the machine is stopped inside the payload at 0x%x: an "
                "exception was taken and never returned from" % pc)

# Every exception the payload claimed was an undefined instruction, which on
# this hardware means one of the instructions the shim is for.
if entries and (lastEsr >> 26) & 0x3f:
    fail.append("the last exception has EC=0x%x, which is not an undefined "
                "instruction" % ((lastEsr >> 26) & 0x3f))

if fail:
    for line in fail:
        print("FAIL: " + line)
    raise SystemExit(1)

print("PASS: %u undefined instructions carried out, the machine still in the "
      "kernel" % entries)
PY
rc=$?

kill "$runpid" 2>/dev/null || true
pkill -f "file=$WORK/armed.img" 2>/dev/null || true
exit "$rc"
