#!/usr/bin/env python3
"""Read the bugcheck Windows stopped on, and the arguments it passed.

The stop screen names the bugcheck but not where it came from, and the four
arguments are the difference between "a driver faulted" and "the kernel's own
startup took a trap".

They are not read out of registers at a breakpoint. Windows leaves them in
KiBugCheckData, which is still there afterwards, and reading that needs no
hardware breakpoints and never stops the machine. The earlier version of this
waited at a breakpoint and had to solve two problems that come with it; the
only one that survives is finding the kernel.

The kernel's address cannot be had from a system register, because this
target's debug interface exposes none: `p $vbar_el1` answers `void` without an
error, and the monitor's register dump stops at the general purpose ones. A
value that cannot be read looks exactly like a value that is not ready yet,
which is how the first attempt spent ten minutes polling for something that
was never going to arrive.

Nor can it be had from the driver, which reports the identity mapped address;
the kernel runs on a KASLR'd high address that cannot be worked out from that.

So it is found in memory, by the shape of what is there: the vector table's
first slot is a branch into the payload and the next three are the same
handler prologue, repeated, two megabytes from wherever the search starts.

Nothing waits a fixed time: the guest's own output ends each wait.

Usage: bugcheck_probe.py [--work DIR] [--keep]
"""

import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

VECTOR_TABLE_RVA = 0x604800       # where the kernel puts its vector table

# KiBugCheckData, where Windows leaves the code and its four arguments. The
# symbol tables this project keeps give it as section 24 offset 1812832, and
# .data starts at 0xc00000 in the same numbering, so the two add up; the page
# is added because those tables sit a page below the image.
KEBUGCHECK_RVA = 0x25ba40         # KeBugCheck, for a breakpoint if one is wanted
KI_BUG_CHECK_DATA_RVA = 0xc00000 + 1812832 + 0x1000

QEMU = os.environ.get("QEMU", "../qemu/build/qemu-system-aarch64")
FIRMWARE = os.environ.get("QEMU_FW", "../winemu/linaro_ovmf.fd")
WIN_DISK = os.environ.get("WIN_DISK", "../winemu/files/winpe_26100.qcow2")


def log(msg):
    print(msg, flush=True)


class Serial:
    """Stream the guest's serial port, so a wait ends when the guest speaks."""

    def __init__(self, path, logfile):
        self.path = path
        self.logfile = logfile
        self.buf = b""
        self.sock = None

    def connect(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(0.5)
                s.connect(self.path)
                self.sock = s
                return
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() > deadline:
                    raise SystemExit("the serial socket never appeared")
                time.sleep(0.05)

    def waitFor(self, pattern, timeout):
        """Read until the pattern appears. Returns the text seen so far."""
        rx = re.compile(pattern.encode())
        deadline = time.monotonic() + timeout
        while True:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                if time.monotonic() > deadline:
                    return None
                continue
            if not chunk:
                return None
            self.buf += chunk
            with open(self.logfile, "ab") as f:
                f.write(chunk)
            if rx.search(self.buf):
                return self.buf.decode("latin1")


def cmd(sock_file, obj):
    sock_file.write((json.dumps(obj) + "\n").encode())
    sock_file.flush()
    while True:
        line = sock_file.readline()
        if not line:
            raise SystemExit("the monitor closed the connection")
        r = json.loads(line)
        if "return" in r or "error" in r:
            return r


def findKernelBase(qmp_file, pc):
    """The kernel's base, from the shape of its vector table.

    A system register would say this directly and cannot be read, so the
    answer is in memory. What makes the search cheap is that the first four
    slots have a fixed relationship: the first is a branch -- into the payload,
    because this project put one there -- and the next three hold the same
    instruction, the prologue of the handler the table dispatches to. Four
    words in that arrangement are not something else in memory looks like.

    The reads are virtual, not physical. The kernel is running at a KASLR'd
    high address and only the virtual view of it exists; a physical read of
    that address answers "cannot access memory" rather than anything useful.
    """

    def word(addr):
        out = cmd(qmp_file, {"execute": "human-monitor-command",
                             "arguments": {"command-line": "x /1gx 0x%x" % addr}})
        m = re.search(r":\s*0x([0-9a-f]+)", out.get("return", ""))
        return int(m.group(1), 16) if m else None

    top = pc - (pc % 0x200000)
    for i in range(16):
        base = top - i * 0x200000
        table = base + VECTOR_TABLE_RVA
        heads = [word(table + slot * 0x80) for slot in range(4)]
        if any(h is None for h in heads):
            continue
        if (heads[0] & 0xFC000000) != 0x14000000:
            continue
        if heads[1] != heads[2] or heads[2] != heads[3]:
            continue
        return base
    return None


def readWords(qmp_file, addr, count):
    """Read `count` doublewords of virtual memory.

    `x`, not `xp`: the kernel exists only at its KASLR'd virtual address, so a
    physical read of the same number answers "cannot access memory", which
    reads the same as "nothing is there".

    Each line the monitor prints holds more than one value, so everything
    after the colon is taken rather than the first field only.
    """
    out = cmd(qmp_file, {"execute": "human-monitor-command",
                         "arguments": {"command-line": "x /%dgx 0x%x" % (count, addr)}})
    values = []
    for line in out.get("return", "").splitlines():
        if ":" not in line:
            continue
        values += [int(v, 16) for v in re.findall(r"0x([0-9a-f]+)", line.split(":", 1)[1])]
    return values


def programCounter(qmp_file):
    out = cmd(qmp_file, {"execute": "human-monitor-command",
                         "arguments": {"command-line": "info registers"}})
    m = re.search(r"PC=([0-9a-f]+)", out.get("return", ""))
    return int(m.group(1), 16) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default="build/bugcheck")
    ap.add_argument("--qmp-port", type=int, default=4447)
    ap.add_argument("--gdb-port", type=int, default=1234)
    ap.add_argument("--find-timeout", type=float, default=1200)
    ap.add_argument("--catch-timeout", type=float, default=1200)
    ap.add_argument("--arm-slot0", default="true",
                    help="whether to take over the SP0 synchronous slot too")
    args = ap.parse_args()

    os.chdir(ROOT)
    work = os.path.join(ROOT, args.work)
    os.makedirs(work, exist_ok=True)

    config = os.path.join(work, "run.toml")
    with open(config, "w") as f:
        f.write("""version = 1

[log]
level = "info"

[scan]
ldapr_rewrite = true

[debug]
enabled = true
arm = true
arm_slot0 = %s
""" % args.arm_slot0)

    esp = os.path.join(work, "run.img")
    serialLog = os.path.join(work, "serial.log")
    serialSock = os.path.join(work, "serial.sock")
    for p in (serialLog, serialSock):
        if os.path.exists(p):
            os.unlink(p)

    log("building the volume")
    env = dict(os.environ, CONFIG=config, ESP=esp)
    subprocess.run(["tests/deploy/build_esp.sh"], env=env, check=True,
                   stdout=subprocess.DEVNULL)

    subprocess.run(["pkill", "-9", "-f", "qemu-system-aarch64"],
                   stderr=subprocess.DEVNULL)
    time.sleep(1)

    log("starting the machine")
    qemu = subprocess.Popen([
        QEMU,
        "-no-reboot", "-accel", "tcg,thread=multi,tb-size=2048",
        "-M", "virt,gic-version=3,virtualization=on,secure=on",
        "-m", "4096", "-smp", "8,sockets=1,clusters=2,cores=4,threads=1",
        "-cpu", os.environ.get("QEMU_CPU", "cortex-a76-nolrcpc"),
        "-kernel", FIRMWARE,
        "-device", "ramfb", "-vnc", "0.0.0.0:0", "-display", "none",
        "-gdb", "tcp::%d" % args.gdb_port,
        "-qmp", "tcp:127.0.0.1:%d,server=on,wait=off" % args.qmp_port,
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-storage,drive=esp,bootindex=1",
        "-drive", "file=%s,if=none,format=raw,id=esp" % esp,
        "-device", "virtio-blk-pci,drive=win,bootindex=2",
        "-drive", "file=%s,if=none,format=qcow2,id=win,readonly=on" % WIN_DISK,
        "-serial", "unix:%s,server=on,wait=off,logfile=%s" % (serialSock, serialLog),
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True)

    try:
        ser = Serial(serialSock, serialLog)
        ser.connect(args.find_timeout)

        log("waiting for the driver to arm")
        if ser.waitFor(r"US-M6\.5-ARMED", args.find_timeout) is None:
            log("the driver never armed; last serial output:")
            tail = open(serialLog, "rb").read().decode("latin1")[-800:]
            log(tail)
            return 1
        log("armed")

        # A system register would say the base outright and none is readable,
        # so it comes from memory: the vector table's first four slots have a
        # shape nothing else has. Until the kernel is running there is no such
        # table to find, so this is also what says the kernel has started.
        qmp = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20)
        qmpFile = qmp.makefile("rwb")
        qmpFile.readline()
        cmd(qmpFile, {"execute": "qmp_capabilities"})

        base = None
        deadline = time.monotonic() + args.find_timeout
        while time.monotonic() < deadline:
            pc = programCounter(qmpFile)
            if pc is not None and (pc >> 40) in (0xFFFFF8, 0xFFFFF9, 0xFFFFFA):
                base = findKernelBase(qmpFile, pc)
                if base is not None:
                    break
            time.sleep(4)
        if base is None:
            log("the kernel never appeared in memory")
            return 1
        log("kernel base = 0x%x" % base)

        # Polled rather than caught at a breakpoint. The value stays in memory
        # after the crash, so being stopped at the exact instruction buys
        # nothing that reading it later does not, and polling needs no
        # hardware breakpoints and no stopping the machine at all.
        at = base + KI_BUG_CHECK_DATA_RVA
        seen = None
        deadline = time.monotonic() + args.catch_timeout
        while time.monotonic() < deadline:
            words = readWords(qmpFile, at, 5)
            # The code is a small number and the table is zero until a
            # bugcheck fills it, so a plausible one is the signal.
            if words and 0 < words[0] < 0x1000:
                seen = words
                break
            time.sleep(5)

        if seen is None:
            log("no bugcheck appeared in %g seconds" % args.catch_timeout)
            log("KiBugCheckData at 0x%x reads: %s" % (at, readWords(qmpFile, at, 5)))
            return 1

        code = seen[0]
        log("")
        log("bugcheck 0x%x" % code)
        for i, v in enumerate(seen[1:], start=1):
            # The registers are gone by now, so what a parameter means has to
            # come from what it points at. An address inside the kernel is
            # named by its offset from the base, which is the form the symbol
            # tables are read in.
            extra = ""
            if base <= v < base + 0x2000000:
                extra = "  (kbase+0x%x)" % (v - base)
            log("  parameter %d = 0x%-16x%s" % (i, v, extra))

        # A few words below the stack pointer the crash kept, if it kept one.
        return 0
    finally:
        if not args.keep:
            try:
                os.killpg(os.getpgid(qemu.pid), signal.SIGTERM)
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
