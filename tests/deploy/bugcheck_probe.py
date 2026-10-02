#!/usr/bin/env python3
"""Catch the bugcheck and read the arguments Windows passed to it.

The stop screen names the bugcheck but not where it came from, and the four
arguments are the difference between "a driver faulted" and "the kernel's own
startup took a trap". They exist in registers for one instant, so the only way
to have them is to be stopped there.

Two things make this less obvious than it sounds.

The first is which address to stop at. The driver reports the kernel's identity
mapped address, and the kernel does not run there: winload rebuilds the address
space and the kernel ends up on a KASLR'd high address that cannot be worked
out from the identity one.

It cannot be read out of a system register either. This target's debug
interface exposes no AArch64 system registers: `p $vbar_el1` answers `void`
without an error, and the monitor's register dump stops at the general purpose
ones. A value that cannot be read looks exactly like a value that is not ready
yet, which is how an earlier version of this spent ten minutes polling for
something that was never going to arrive.

So the base is found in memory instead, by the shape of what is there. The
kernel's vector table has a signature that is hard to hit by accident: its
first slot is a branch into the payload and the following three are the same
`mrs x18, sp_el0` prologue, repeated. Searching a handful of two megabyte
aligned candidates under the program counter finds it.

The second is that the kernel's code is read only by then, so the breakpoints
have to be hardware ones. A software breakpoint writes, and the write faults
instead of stopping.

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
KEBUGCHECK_RVA = 0x25ba40         # in the symbol tables this project keeps
KEBUGCHECKEX_RVA = 0x25c740       # ... which sit a page below the image

QEMU = os.environ.get("QEMU", "../qemu/build/qemu-system-aarch64")
FIRMWARE = os.environ.get("QEMU_FW", "../winemu/linaro_ovmf.fd")
WIN_DISK = os.environ.get("WIN_DISK", "../winemu/files/winpe_26100.qcow2")
GDB = os.environ.get("GDB", "aarch64-linux-gnu-gdb")


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


def gdbRun(script, timeout):
    """Run a batch gdb session and return its output."""
    try:
        out = subprocess.run([GDB, "-q", "-batch", "-x", script],
                             capture_output=True, text=True, timeout=timeout)
        return out.stdout + out.stderr
    except subprocess.TimeoutExpired as e:
        return (e.stdout or "") + (e.stderr or "")


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default="build/bugcheck")
    ap.add_argument("--qmp-port", type=int, default=4447)
    ap.add_argument("--gdb-port", type=int, default=1234)
    ap.add_argument("--find-timeout", type=float, default=1200)
    ap.add_argument("--catch-timeout", type=float, default=1200)
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
""")

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

        # VBAR_EL1 would say this outright and cannot be read, so the answer
        # comes from memory: the vector table is a page boundary, and its
        # first four slots have a shape nothing else has.
        qmp = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20)
        qmpFile = qmp.makefile("rwb")
        qmpFile.readline()
        cmd(qmpFile, {"execute": "qmp_capabilities"})

        pcText = cmd(qmpFile, {"execute": "human-monitor-command",
                               "arguments": {"command-line": "info registers"}})
        m = re.search(r"PC=([0-9a-f]+)", pcText.get("return", ""))
        if not m:
            log("the program counter could not be read")
            return 1
        pc = int(m.group(1), 16)

        base = findKernelBase(qmpFile, pc)
        if base is None:
            log("no vector table found under PC=0x%x" % pc)
            return 1
        log("PC=0x%x -> kernel base 0x%x" % (pc, base))
        qmp.close()

        # Every spelling of the two entry points, because the symbol tables
        # this project keeps are a page below the image.
        script = os.path.join(work, "catch.gdb")
        with open(script, "w") as f:
            f.write("set pagination off\nset confirm off\nset architecture aarch64\n")
            f.write("target remote :%d\n" % args.gdb_port)
            for rva in (KEBUGCHECK_RVA, KEBUGCHECK_RVA + 0x1000,
                        KEBUGCHECKEX_RVA, KEBUGCHECKEX_RVA + 0x1000):
                f.write("hbreak *0x%x\n" % (base + rva))
            f.write("continue\n")
            f.write('printf "\\n=== stopped at 0x%lx (kbase+0x%lx) ===\\n", $pc, $pc - 0x%x\n'
                    % base)
            f.write('printf "  x0  bugcheck code   = 0x%lx\\n", $x0\n')
            for r in ("x1", "x2", "x3", "x4"):
                f.write('printf "  %-3s parameter       = 0x%lx\\n", "%s", $%s\n'
                        % (r, r, r))
            f.write('printf "  x30 caller          = 0x%lx  kbase+0x%lx\\n", $x30, $x30 - 0x%x\n'
                    % base)
            f.write('printf "  sp                  = 0x%lx\\n", $sp\n')
            # The system registers are not readable here, so what the fault
            # was is only in the log QEMU keeps with -d int.
            f.write("bt 20\n")
            f.write("detach\nquit\n")

        log("waiting for the bugcheck (this can take minutes)")
        out = gdbRun(script, timeout=args.catch_timeout + 60)
        log(out)
        return 0
    finally:
        if not args.keep:
            try:
                os.killpg(os.getpgid(qemu.pid), signal.SIGTERM)
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
