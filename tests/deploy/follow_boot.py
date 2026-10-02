#!/usr/bin/env python3
"""Follow a boot: where the CPU is, and what the payload has seen.

The serial port goes quiet once the kernel has its own page tables, and the
screen only changes when Windows draws something, so neither says what is
happening in between. QEMU will answer both questions directly: the current
program counter says whether the machine is moving at all, and the record in
the pool says how many exceptions the payload has carried out.

Sampling both over time is what separates a machine that is working from one
that has stopped somewhere plausible. A counter that stops rising with the PC
inside the payload is a handler that never returned; a counter that stops
rising with the PC in the kernel is the kernel making no further progress.

Usage: follow_boot.py <pool-physical-address> [seconds] [interval]
"""

import json
import re
import socket
import sys
import time

pooll = int(sys.argv[1], 16)
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 600
interval = int(sys.argv[3]) if len(sys.argv) > 3 else 30

s = socket.create_connection(("127.0.0.1", 4444), timeout=10)
f = s.makefile("rwb")
f.readline()


def cmd(obj):
    f.write((json.dumps(obj) + "\n").encode())
    f.flush()
    while True:
        line = f.readline()
        if not line:
            raise SystemExit("qmp closed")
        r = json.loads(line)
        if "return" in r or "error" in r:
            return r


def hmp(command):
    return cmd({"execute": "human-monitor-command",
                "arguments": {"command-line": command}}).get("return", "")


cmd({"execute": "qmp_capabilities"})


def words(address, count):
    """Read guest physical memory.

    Each line the monitor prints holds several values, so everything after
    the address is taken, not just the first field.
    """
    out = hmp("xp /%dgx 0x%x" % (count, address))
    values = []
    for line in out.splitlines():
        if ":" not in line:
            continue
        values += [int(v, 16) for v in re.findall(r"0x[0-9a-f]+", line.split(":", 1)[1])]
    return values


last = None
t = 0
print("%6s  %-18s %8s %8s  %-10s %s" % ("t", "pc", "entries", "handled", "ec", "elr"))
while t <= seconds:
    regs = hmp("info registers")
    m = re.search(r"PC=([0-9a-f]+)", regs)
    pc = int(m.group(1), 16) if m else 0

    r = words(pooll + 8, 15)
    entries, handled = r[1], r[2]
    esr, elr, far = r[3], r[4], r[5]
    insn = r[8]

    print("%5ds  0x%-16x %8d %8d  0x%-8x 0x%x  insn=0x%08x"
          % (t, pc, entries, handled, (esr >> 26) & 0x3f, elr, insn))
    sys.stdout.flush()

    # Sampled the same way twice in a row: nothing is moving, and waiting
    # longer will not change that.
    if last == (pc, entries):
        print("  (no change since the previous sample)")
    last = (pc, entries)

    time.sleep(interval)
    t += interval
