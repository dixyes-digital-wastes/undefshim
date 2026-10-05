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

CHECK="vector_case"
. tests/deploy/check.sh

BUILD_DIR="${BUILD_DIR:-build}"
WORK="$BUILD_DIR/vector-case"
WIN_DISK="${WIN_DISK:-}"
# Where the loader's synchronous slot is, as an offset into the loader. The
# machine stops here when nothing is armed.
LOADER_SYNC_SLOT="${LOADER_SYNC_SLOT:-0x1a00}"
SETTLE="${SETTLE:-90}"
QMP_PORT="${QMP_PORT:-4444}"

if [ -z "$WIN_DISK" ]; then
    checkSkip "WIN_DISK is not set"
fi
if [ ! -f "$WIN_DISK" ]; then
    checkFail "no windows disk at $WIN_DISK"
fi

mkdir -p "$WORK"
cat > "$WORK/armed.toml" <<EOF

[uart]
# Where the machine's serial port is. Without this the driver has no port to
# report on, and a check that reads the log would be reading nothing
baseAddr = 0x09000000
type = "pl011"
width = 32
# A log with no colour escapes in it: a check reads these lines by
# matching text, and a colour sequence between the tag and the words
# is one more thing that can come between them and the pattern
color = false

[log]
level = "verbose"

[ldapr]
# The exception path is what this checks, so the replacement that would keep
# the instructions from faulting in the first place is turned off. With it on
# there is nothing to take over and nothing to enter.
imageInplaceRewrite = false
EOF

log="$WORK/armed.log"
if ! CONFIG="$WORK/armed.toml" ESP="$WORK/armed.img" tests/deploy/build_esp.sh \
        >"$WORK/armed.esp" 2>&1; then
    checkNote "the volume could not be built"
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
    checkNote "the vector table was never taken over"
    tests/deploy/plain.sh "$log" | grep -aE 'arm:|plan: vbar|M5' | tail -10
    exit 1
fi
tests/deploy/plain.sh "$log" | grep -a 'arm: vbar' | tail -2

# The replacement has to be off, or nothing faults and the exception path has
# nothing to do. A key read from the wrong configuration section looks exactly
# like a check that found nothing, so it is asserted rather than assumed: that
# mistake has already been made once.
if ! tests/deploy/plain.sh "$log" | grep -q '^config: \[ldapr\] imageInplaceRewrite=false'; then
    checkNote "the replacement is on, so this checks nothing"
    tests/deploy/plain.sh "$log" | grep -a '^config: ' | head -3
    exit 1
fi

sleep "$SETTLE"

python3 - "$log" "$QMP_PORT" "$LOADER_SYNC_SLOT" <<'PY'
import json, os, re, socket, sys

sys.path.insert(0, "tests/deploy")
sys.path.insert(0, "tests/unit")
import check as checklib
import logtext
import pool_dump

report = checklib.Check(os.environ.get("CHECK", "vector_case"))

log_path, port, loader_sync = sys.argv[1], int(sys.argv[2]), int(sys.argv[3], 16)
log = logtext.read(log_path)

m = re.search(r"pool: pa=0x([0-9a-f]+)", log)
if not m:
    report.fail("no pool address in the log")
    raise SystemExit(1)
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
            report.fail("the monitor closed the connection")
            raise SystemExit(1)
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

# The record is read through the dump's own idea of where its fields are. They
# are the payload's layout, which this side does not own: writing the offsets
# out here is how this check came to be reading the wrong words, silently,
# after the record grew
head = pool_dump.readPhysical(f, pool_pa + 8, pool_dump.PUBLIC_WORDS)
if len(head) < pool_dump.PUBLIC_WORDS:
    report.fail("the pool record could not be read")
    raise SystemExit(1)

def field(name):
    return head[pool_dump.PUBLIC.index(name)]

magic = field("magic")
entries = field("entries")
handled = field("handled")
handedBack = field("handedBack")
stuck = field("stuck")
lastEsr = field("lastEsr")
lastElr = field("lastElr")
lastFar = field("lastFar")
lastInsn = field("lastInsn")
emuInsn, emuAddr, emuValue = field("emuInsn"), field("emuAddr"), field("emuValue")
emuX9 = field("emuX9")

report.note("pc=0x%x pool=0x%x" % (pc, pool_pa))
report.note("magic=0x%x entries=%u handled=%u" % (magic, entries, handled))
report.note("last insn=0x%08x ec=0x%x elr=0x%x far=0x%x"
            % (lastInsn, (lastEsr >> 26) & 0x3f, lastElr, lastFar))
report.note("first emulated insn=0x%08x addr=0x%x value=0x%x x9=0x%x"
            % (emuInsn, emuAddr, emuValue, emuX9))
if loader_base is not None:
    report.note("loader base=0x%x, synchronous slot=0x%x"
                % (loader_base, loader_base + loader_sync))

fail = []

if magic != pool_dump.MAGIC:
    fail.append("the payload left no record in the pool (magic 0x%x)" % magic)
if entries == 0:
    fail.append("the payload was never entered")
# Every entry ends one of three ways: carried out, given to the kernel's own
# handler, or left with no destination at all. They are only checked against
# each other loosely, because the machine is running while this reads it: the
# fields are separate loads, so a snapshot can be a few entries out. What has
# to hold is that almost everything was carried out, and that nothing was left
# with nowhere to go
if stuck != 0:
    fail.append("%u entries were left with no destination at all" % stuck)
if handled * 100 < entries * 95:
    fail.append("of %u entries only %u were carried out, and %u were handed "
                "to the kernel" % (entries, handled, handedBack))
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
if handled and (lastEsr >> 26) & 0x3f:
    fail.append("the last exception has EC=0x%x, which is not an undefined "
                "instruction" % ((lastEsr >> 26) & 0x3f))

if fail:
    for line in fail:
        report.note(line)
    raise SystemExit(1)

report.note("magic is right, %u entries, %u carried out, %u handed back, "
            "%u stuck" % (entries, handled, handedBack, stuck))
PY
rc=$?

kill "$runpid" 2>/dev/null || true
pkill -f "file=$WORK/armed.img" 2>/dev/null || true

[ "$rc" -eq 0 ] || checkFail "the exception path did not carry out what it took"
checkPass "every undefined instruction taken was carried out, and the machine went on"
