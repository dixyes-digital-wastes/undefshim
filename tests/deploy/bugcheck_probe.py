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

KEBUGCHECK_RVA = 0x25ba40         # KeBugCheck, if a breakpoint is ever wanted

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

    Asked for in blocks, because the monitor's own limit on one command is
    well below what scanning a data section needs, and one command per word
    turns a scan into a long wait.
    """
    values = []
    done = 0
    while done < count:
        want = min(count - done, 256)
        out = cmd(qmp_file, {"execute": "human-monitor-command",
                             "arguments": {"command-line":
                                           "x /%dgx 0x%x" % (want, addr + done * 8)}})
        got = 0
        for line in out.get("return", "").splitlines():
            if ":" not in line:
                continue
            row = [int(v, 16) for v in re.findall(r"0x([0-9a-f]+)", line.split(":", 1)[1])]
            values += row
            got += len(row)
        if got == 0:
            break
        done += got
    return values


def findBugCheckRecord(qmp_file, base):
    """The bugcheck code and its four arguments, found rather than assumed.

    They live in the data section as a code followed by four parameters, and
    the symbol that names them resolves to zeros -- so the record is found by
    shape instead: a small number, then four values that are either zero or a
    kernel address. A run of small numbers with small numbers after them, of
    which a data section has many, does not match.

    Returns (offset, words) or (None, None).
    """
    WINDOW_START, WINDOW_LEN = 0xdb0000, 0x20000
    words = readWords(qmp_file, base + WINDOW_START, WINDOW_LEN // 8)
    if not words:
        return None, None

    def plausible(v):
        # An address, a small number, or zero. The small ones are real and not
        # an accident of the search: PAGE_FAULT_IN_NONPAGED_AREA passes a flag
        # of 1, and a filter that only accepted addresses skipped the whole
        # record and reported that there was none.
        return v == 0 or v < 0x10000 or v >= 0xFFFF000000000000

    best = None
    for i in range(len(words) - 4):
        code = words[i]
        # A bugcheck code is a small number. The exceptions that are not
        # (0xc0000005 and friends) are exception codes, not bugcheck codes.
        if not (0 < code < 0x1000):
            continue
        params = words[i + 1:i + 5]
        score = sum(1 for p in params if p >= 0xFFFF000000000000)
        if any(not plausible(p) for p in params):
            continue
        if score < 2:
            continue
        if best is None or score > best[0]:
            best = (score, WINDOW_START + i * 8, [code] + params)
    if best is None:
        return None, None
    return best[1], best[2]


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
    ap.add_argument("--keep", action="store_true",
                    help="leave the machine running, to be looked at afterwards")
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

        # The screen says when the bugcheck has happened, and it says so as
        # soon as Windows draws it -- a bluescreen is on screen well inside
        # two minutes on this machine, so a fixed watch long enough for the
        # slowest case wastes minutes on every run.
        log("waiting for the screen to go blue")
        watch = subprocess.run(
            [sys.executable, "tests/deploy/bluescreen_watch.py",
             "--qmp-port", str(args.qmp_port),
             "--timeout", str(args.catch_timeout),
             "--shots", os.path.join(work, "shots")],
            capture_output=True, text=True)
        log(watch.stdout.strip())
        if watch.returncode != 0:
            log("no bluescreen appeared: %s" % watch.stderr.strip())
            return 1

        # The colour is necessary and not sufficient: it says a bugcheck was
        # drawn, not which one. The code and its arguments come from memory.
        qmp = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20)
        qmpFile = qmp.makefile("rwb")
        qmpFile.readline()
        cmd(qmpFile, {"execute": "qmp_capabilities"})

        pc = programCounter(qmpFile)
        if pc is None:
            log("the program counter could not be read")
            return 1
        base = findKernelBase(qmpFile, pc)
        if base is None:
            log("no vector table found under PC=0x%x" % pc)
            return 1
        log("kernel base = 0x%x" % base)

        offset, words = findBugCheckRecord(qmpFile, base)
        if words is None:
            log("no bugcheck record found in the data section")
            return 1

        log("")
        log("record at kbase+0x%x" % offset)
        log("bugcheck 0x%x" % words[0])
        for i, v in enumerate(words[1:], start=1):
            # The registers are gone by now, so what a parameter means has to
            # come from what it is. An address inside the kernel is named by
            # its offset, which is the form the symbol tables are read in.
            extra = ""
            if base <= v < base + 0x2000000:
                extra = "  (kbase+0x%x)" % (v - base)
            log("  parameter %d = 0x%-18x%s" % (i, v, extra))
        return 0
    finally:
        if not args.keep:
            try:
                os.killpg(os.getpgid(qemu.pid), signal.SIGTERM)
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
