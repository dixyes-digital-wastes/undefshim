#!/usr/bin/env python3
"""Freeze a stopped machine's evidence into files before it is thrown away.

A blue screen leaves what is worth knowing where only a debugger or the
monitor can reach it: the vector table the CPUs actually have loaded, the
stubs that were written into it, the bugcheck record, and the stack of the CPU
that died. Reboot the machine and all of it is gone, and a second run of the
same test only reproduces it if the fault is deterministic.

The addresses come from the machine where they can and from the serial log
where only the payload knew them. Nothing here assumes a layout: the kernel
base is found from the halt address, the stacks are found from the CPUs'
own registers, and the pool comes out of the log.

Usage: snapshot.py [--qmp-port N] [--gdb-port N] [--serial FILE] --out DIR
"""

import argparse
import json
import os
import re
import sys
import time

import pool_dump
import logtext

GDB = "gdb"

# Bytes asked for in one monitor read. The monitor caps a line, not a command,
# so this is about the size of the answer rather than the size of the ask.
BLOCK = 1024


def gdbRegisters(port):
    """Every CPU's registers, from the debug stub.

    The monitor's own register dump leaves out the banked system registers,
    and those are the ones that say which exception level was running and
    which vector table was loaded - so gdb is asked instead.
    """
    import subprocess

    out = subprocess.run(
        [GDB, "-q", "-batch",
         "-ex", "set confirm off", "-ex", "set pagination off",
         "-ex", "target remote :%d" % port,
         "-ex", "thread apply all info all-registers",
         "-ex", "detach"],
        capture_output=True, text=True, timeout=300).stdout
    cpus = []
    at = None
    for line in out.splitlines():
        m = re.match(r"Thread \d+ \(.*CPU#(\d+)", line)
        if m:
            at = {"cpu": int(m.group(1)), "regs": {}}
            cpus.append(at)
            continue
        m = re.match(r"(\S+)\s+(0x[0-9a-fA-F]+)\s", line.strip() + " ")
        if m and at is not None:
            at["regs"][m.group(1)] = int(m.group(2), 16)
    return cpus


def parseHexDump(out):
    """The values of a monitor dump, and nothing from its complaints.

    A read that lands outside RAM is answered with a sentence rather than an
    error, and that sentence carries an address in it - which is a number in
    the shape of a value. Only lines that are a dump line are taken.
    """
    values = []
    for line in out.splitlines():
        m = re.match(r"^([0-9a-fA-F]+):\s+(.*)$", line.strip())
        if not m:
            continue
        for v in m.group(2).split():
            if v.startswith("0x"):
                values.append(int(v, 16))
    return values


def findKernelBase(f, hint):
    """The base of the image the machine is halted in.

    Every module is mapped on the image's own alignment, and the vector table
    is inside one of them, so walking down in those steps and asking for a DOS
    header finds it without knowing which offset the table sits at.
    """
    top = hint & ~0x1FFFFF
    for i in range(0, 96):
        cand = top - i * 0x200000
        if cand < 0xFFFF800000000000:
            break
        head = readAt(f, cand, 1, False)
        if head and (head[0] & 0xFFFF) == 0x5A4D:
            return cand
    return None


def readAt(f, addr, count, physical):
    """One block of 64 bit values, whichever way it has to be read.

    The monitor caps how much one line may carry, so a caller asking for more
    than that gets what fits.
    """
    command = "xp" if physical else "x"
    out = pool_dump.cmd(f, {
        "execute": "human-monitor-command",
        "arguments": {"command-line": "%s /%dgx 0x%x" % (command, min(count, 128), addr)}}).get("return", "")
    return parseHexDump(out)


def read(f, addr, size, physical):
    out = bytearray()
    while len(out) < size:
        want = min(size - len(out), BLOCK) // 8
        if want == 0:
            break
        block = readAt(f, addr + len(out), want, physical)
        if not block:
            break
        for v in block:
            out += v.to_bytes(8, "little")
    return bytes(out[:size])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp-port", type=int, default=4447)
    ap.add_argument("--gdb-port", type=int, default=1234)
    ap.add_argument("--serial", default=None)
    ap.add_argument("--out", required=True)
    ap.add_argument("--kernel-base", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--pool", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--bugcheck-rva", type=lambda s: int(s, 0), default=0xDBA5E0)
    ap.add_argument("--stack-bytes", type=lambda s: int(s, 0), default=0x4000)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    cpus = gdbRegisters(args.gdb_port)
    sys.stderr.write("registers: %d cpus\n" % len(cpus))

    import socket
    con = None
    for attempt in range(10):
        try:
            con = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=30)
            f = con.makefile("rwb")
            f.readline()
            break
        except (OSError, TimeoutError):
            """
            The monitor serves one client at a time, so a connection that
            arrives while the reader before it is still letting go is refused
            for a moment and accepted after. Snapshotting a machine that just
            stopped is exactly that moment: the tool that watched it is letting
            go while this one is arriving.
            """
            con = None
            time.sleep(1)
    if con is None:
        sys.stderr.write("the monitor did not answer\n")
        return 2
    pool_dump.cmd(f, {"execute": "qmp_capabilities"})

    regions = []

    def take(name, addr, size, physical=False):
        data = read(f, addr, size, physical)
        path = os.path.join(args.out, name + ".bin")
        with open(path, "wb") as out:
            out.write(data)
        regions.append({"name": name, "addr": addr, "size": len(data),
                        "physical": physical, "file": os.path.basename(path)})
        sys.stderr.write("  %-16s %#014x %6d bytes\n" % (name, addr, len(data)))

    hint = None
    for at in cpus:
        vbar = at["regs"].get("VBAR_EL1")
        if vbar:
            hint = vbar
            break
    base = args.kernel_base or (findKernelBase(f, hint) if hint else None)
    sys.stderr.write("kernel base: %s\n" % ("%#x" % base if base else "unknown"))
    if base:
        take("nt-head", base, 0x200)
        take("nt-vector", base + (hint - base), 0x800)
        take("nt-stubs", base + (hint - base) - 0x2000, 0x2800)
        take("nt-bugcheck", base + args.bugcheck_rva, 0x40)

    pool = args.pool or (pool_dump.poolFromSerial(args.serial) if args.serial else None)
    if pool:
        take("payload-pool", pool, 0x22000, physical=True)

    for at in cpus:
        regs = at["regs"]
        for bank in ("SP_EL0", "SP_EL1"):
            sp = regs.get(bank)
            if sp:
                take("cpu%d-%s" % (at["cpu"], bank.lower()), sp, args.stack_bytes)

    for at in cpus:
        at["regs"] = {k: v for k, v in at["regs"].items()
                      if k in ("pc", "SP_EL0", "SP_EL1", "VBAR_EL1", "SCTLR_EL1",
                               "ESR_EL1", "FAR_EL1", "ELR_EL1", "SPSR_EL1",
                               "TCR_EL1", "TTBR0_EL1", "TTBR1_EL1", "TPIDR_EL1")}

    with open(os.path.join(args.out, "meta.json"), "w") as out:
        json.dump({"kernelBase": base, "pool": pool, "cpus": cpus,
                   "regions": regions}, out, indent=1)
    sys.stderr.write("wrote %s\n" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
