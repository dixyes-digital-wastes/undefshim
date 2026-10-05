#!/usr/bin/env python3
"""Start the machine, wait for the driver, then poll it and report liveness.

Written to answer one question that the existing watch could not: when the
machine stops answering, is that the machine or the tooling? The watch reports
"the machine went away" for any failure of the monitor conversation, which is
indistinguishable from the machine having reset -- and a reset is a result.

So this keeps the QEMU process handle and reports its exit status separately
from any read that failed, and it reads one thing at a time so that which read
failed is known.

Usage: alive_probe.py [--image PATH] [--timeout S]
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

QEMU = os.environ.get("QEMU", "../qemu/build/qemu-system-aarch64")
FIRMWARE = os.environ.get("QEMU_FW", "../winemu/linaro_ovmf.fd")
WIN_DISK = os.environ.get("WIN_DISK", "../winemu/files/winpe_26100.qcow2")

QMP_PORT = 4447


def cmd(f, obj):
    f.write((json.dumps(obj) + "\n").encode())
    f.flush()
    while True:
        line = f.readline()
        if not line:
            raise OSError("the monitor closed the connection")
        r = json.loads(line)
        if "return" in r or "error" in r:
            return r


def hmp(f, command):
    return cmd(f, {"execute": "human-monitor-command",
                   "arguments": {"command-line": command}}).get("return", "")


def pcs(f):
    out = hmp(f, "info registers -a")
    return tuple(int(m.group(2), 16)
                 for m in re.finditer(r"CPU#(\d+)\s*\n\s*PC=([0-9a-f]+)", out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True, help="the ESP built by the probe")
    ap.add_argument("--timeout", type=float, default=420)
    ap.add_argument("--interval", type=float, default=2.0)
    args = ap.parse_args()

    os.chdir(ROOT)
    image = os.path.abspath(args.image)
    serialLog = "/tmp/alive.serial"
    serialSock = "/tmp/alive.sock"
    for p in (serialLog, serialSock):
        if os.path.exists(p):
            os.unlink(p)

    subprocess.run(["pkill", "-9", "-f", "qemu-system-aarch64"],
                   stderr=subprocess.DEVNULL)
    time.sleep(1)

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
        "-drive", "file=%s,if=none,format=raw,id=esp" % image,
        "-device", "virtio-blk-pci,drive=win,bootindex=2",
        "-drive", "file=%s,if=none,format=qcow2,id=win,readonly=on" % WIN_DISK,
        "-serial", "unix:%s,server=on,wait=off,logfile=%s" % (serialSock, serialLog),
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True)

    print("qemu pid %d" % qemu.pid, flush=True)

    try:
        # The guest's own marker ends the wait, as everywhere else.
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            if os.path.exists(serialLog):
                text = open(serialLog, "rb").read().decode("latin1")
                if "M6.5 armed" in text or "M7 rewritten" in text:
                    break
            if qemu.poll() is not None:
                print("qemu exited during boot, rc=%s" % qemu.returncode, flush=True)
                return 1
            time.sleep(0.5)
        else:
            print("the driver never reached its marker", flush=True)
            return 1
        if qemu.poll() is not None:
            print("qemu exited before the driver could be asked anything", flush=True)
            return 1

        print("the driver is ready; polling", flush=True)

        sock = socket.create_connection(("127.0.0.1", QMP_PORT), timeout=10)
        f = sock.makefile("rwb")
        f.readline()
        cmd(f, {"execute": "qmp_capabilities"})

        n = 0
        asof = {}          # which marker the guest had reached, for context
        lastPc = None
        while time.monotonic() < deadline:
            n += 1
            rc = qemu.poll()
            if rc is not None:
                print("frame %d: qemu exited rc=%s -- not a monitor failure"
                      % (n, rc), flush=True)
                return 3

            step = "info registers -a"
            try:
                pc = pcs(f)
            except Exception as e:
                print("frame %d: read of '%s' failed: %s" % (n, step, e), flush=True)
                rc = qemu.poll()
                print("  qemu poll after the failure: %s" % (rc if rc is not None
                                                             else "still running"),
                      flush=True)
                return 4

            step = "screendump"
            try:
                hmp(f, "screendump /tmp/alive.ppm")
            except Exception as e:
                print("frame %d: read of '%s' failed: %s" % (n, step, e), flush=True)
                rc = qemu.poll()
                print("  qemu poll after the failure: %s" % (rc if rc is not None
                                                             else "still running"),
                      flush=True)
                return 4

            if pc != lastPc:
                print("frame %d: pcs %s" % (n, " ".join("0x%x" % p for p in pc)),
                      flush=True)
                lastPc = pc
            else:
                print("frame %d: unchanged" % n, flush=True)
            time.sleep(args.interval)

        print("the poll ran out with qemu still running", flush=True)
        return 0
    finally:
        if qemu.poll() is None:
            os.killpg(os.getpgid(qemu.pid), signal.SIGTERM)


if __name__ == "__main__":
    sys.exit(main())
