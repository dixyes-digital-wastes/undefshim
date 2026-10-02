#!/usr/bin/env python3
"""What the loader passes to the kernel, read at the handover.

The loader's module list is the authoritative answer to "where did ntoskrnl
land", and the reference implementation gets it from the structure the kernel
is started with. This checks that the same value is available where this
project already runs -- the handover stub -- before anything is built on the
assumption.

The handover is reached by writing a spin there with the debug patch table, so
the machine stops with the loader's argument still in the register that is
about to be handed over. Nothing is instrumented: the patch goes in through
the mechanism that already exists, and the value is read over the monitor.

Usage: handover_probe.py [--work DIR] [--patch-rva RVA] [--keep]
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

# Where winload branches to the kernel. The walk here only needs to be right
# about that one address; everything else is read out of the structure.
DEFAULT_PATCH_RVA = 0x109C

QEMU = os.environ.get("QEMU", "../qemu/build/qemu-system-aarch64")
FIRMWARE = os.environ.get("QEMU_FW", "../winemu/linaro_ovmf.fd")
WIN_DISK = os.environ.get("WIN_DISK", "../winemu/files/winpe_26100.qcow2")
QMP_PORT = int(os.environ.get("QMP_PORT", "4448"))

# KLDR_DATA_TABLE_ENTRY, arm64. InLoadOrderLinks is first, so a list entry is
# the structure; the rest are the fields this needs.
KLDR_DLL_BASE = 0x30
KLDR_SIZE_OF_IMAGE = 0x40
KLDR_ENTRY_POINT = 0x38
KLDR_BASE_DLL_NAME = 0x58          # UNICODE_STRING
KLDR_BASE_NAME_BUFFER = 0x60

# LOADER_PARAMETER_BLOCK, arm64. Four ULONGs before the first list.
LDR_LOAD_ORDER_LIST = 0x10


def log(msg):
    print(msg, flush=True)


def qmp_connect(port, timeout=60):
    deadline = time.monotonic() + timeout
    while True:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=10)
            f = s.makefile("rwb")
            f.readline()
            cmd(f, {"execute": "qmp_capabilities"})
            return s, f
        except (ConnectionRefusedError, OSError):
            if time.monotonic() > deadline:
                raise SystemExit("the monitor never listened on %d" % port)
            time.sleep(0.2)


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


def hmp(sock_file, command):
    return cmd(sock_file, {"execute": "human-monitor-command",
                           "arguments": {"command-line": command}}).get("return", "")


def registers(sock_file):
    out = hmp(sock_file, "info registers")
    regs = {}
    for name, value in re.findall(r"([A-Z0-9]+)=([0-9a-f]+)", out):
        regs[name] = int(value, 16)
    return regs


def readWords(sock_file, addr, count):
    """Read `count` doublewords of virtual memory.

    Asked for in blocks: one command per word makes a page take minutes.
    """
    values = []
    done = 0
    while done < count:
        want = min(count - done, 128)
        out = hmp(sock_file, "x /%dgx 0x%x" % (want, addr + done * 8))
        got = 0
        for line in out.splitlines():
            if ":" not in line:
                continue
            row = [int(v, 16) for v in re.findall(r"0x([0-9a-f]+)", line.split(":", 1)[1])]
            values += row
            got += len(row)
        if got == 0:
            break
        done += got
    return values


def readUtf16(sock_file, addr, count=16):
    """A UTF-16 string, as the loader stores module names.

    Read as words and decoded here rather than by the monitor: the monitor
    prints values, and what this needs is characters.
    """
    words = readWords(sock_file, addr, (count + 3) // 4)
    if not words:
        return None
    raw = b"".join(w.to_bytes(8, "little") for w in words)
    chars = []
    for i in range(0, count * 2, 2):
        c = raw[i] | (raw[i + 1] << 8)
        if c == 0:
            break
        chars.append(chr(c))
    return "".join(chars)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default="build/handover")
    ap.add_argument("--patch-rva", type=lambda s: int(s, 0), default=DEFAULT_PATCH_RVA)
    ap.add_argument("--find-timeout", type=float, default=900)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    os.chdir(ROOT)
    work = os.path.join(ROOT, args.work)
    os.makedirs(work, exist_ok=True)

    config = os.path.join(work, "run.toml")
    with open(config, "w") as f:
        f.write("""version = 1

[scan]
ldapr_rewrite = true

[log]
level = "info"

[debug]
enabled = true

[[debug.patch]]
target = "winload"
rva    = 0x%x
value  = 0x14000000
width  = 4
tag    = "handover spin"
""" % args.patch_rva)

    esp = os.path.join(work, "run.img")
    serialLog = os.path.join(work, "serial.log")
    serialSock = os.path.join(work, "serial.sock")
    for p in (serialLog, serialSock):
        if os.path.exists(p):
            os.unlink(p)

    log("building the volume")
    subprocess.run(["tests/deploy/build_esp.sh"],
                   env=dict(os.environ, CONFIG=config, ESP=esp), check=True,
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
        "-qmp", "tcp:127.0.0.1:%d,server=on,wait=off" % QMP_PORT,
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-storage,drive=esp,bootindex=1",
        "-drive", "file=%s,if=none,format=raw,id=esp" % esp,
        "-device", "virtio-blk-pci,drive=win,bootindex=2",
        "-drive", "file=%s,if=none,format=qcow2,id=win,readonly=on" % WIN_DISK,
        "-serial", "unix:%s,server=on,wait=off,logfile=%s" % (serialSock, serialLog),
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)

    try:
        sock, f = qmp_connect(QMP_PORT, args.find_timeout)

        # Wait for the machine to stop moving. A spin is a fixed PC, and a
        # boot that is progressing is not, so the two are told apart by
        # sampling rather than by a fixed wait.
        log("waiting for the spin")
        deadline = time.monotonic() + args.find_timeout
        last = None
        still = 0
        pc = None
        while time.monotonic() < deadline:
            regs = registers(f)
            pc = regs.get("PC")
            if pc is not None and pc == last:
                still += 1
                if still >= 6:
                    break
            else:
                still = 0
            last = pc
            time.sleep(2)

        log("PC = 0x%x" % (pc or 0))
        out = hmp(f, "info registers")
        log(out.splitlines()[0] if out else "")
        for name in ("PC", "X00", "X01", "X19", "X20", "X29", "SP"):
            if name in regs:
                log("  %-4s = 0x%x" % (name, regs[name]))

        arg = regs.get("X00", 0)
        entry = regs.get("X20", 0)
        log("")
        log("x0 = 0x%x  (the loader's argument, if this is the handover)" % arg)
        log("x20 = 0x%x (the kernel entry)" % entry)

        if arg < 0x10000:
            log("x0 is not a pointer, so this is not the handover -- or not yet")
            return 1

        head = readWords(f, arg + LDR_LOAD_ORDER_LIST, 4)
        log("")
        log("LoadOrderListHead at 0x%x: %s" % (arg + LDR_LOAD_ORDER_LIST,
                                               " ".join("0x%x" % v for v in head)))
        if not head or head[0] < 0x10000:
            log("the list head does not look like a list")
            return 1

        # Walk it, and for each entry read the name and the base. The name is
        # what makes the walk meaningful: without it a wrong offset still
        # produces addresses, just wrong ones.
        log("")
        log("walking the list:")
        node = head[0]
        for i in range(16):
            if node == arg + LDR_LOAD_ORDER_LIST:
                log("  (back at the head)")
                break
            words = readWords(f, node, 0x70 // 8)
            if not words or len(words) < 0x70 // 8:
                log("  0x%x: unreadable" % node)
                break
            nameAddr = words[KLDR_BASE_NAME_BUFFER // 8]
            base = words[KLDR_DLL_BASE // 8]
            size = words[KLDR_SIZE_OF_IMAGE // 8] & 0xffffffff
            name = readUtf16(f, nameAddr, 20) if nameAddr > 0x10000 else None
            log("  [%2d] 0x%x %-24s base=0x%x size=0x%x" %
                (i, node, name or "<no name>", base, size))
            nxt = words[0]
            if nxt < 0x10000 or nxt == node:
                break
            node = nxt
        return 0
    finally:
        if not args.keep:
            try:
                os.killpg(os.getpgid(qemu.pid), signal.SIGTERM)
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
