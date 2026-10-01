#!/usr/bin/env python3
"""Stream the guest serial port and stop as soon as a pattern shows up.

QEMU exposes the PL011 as a unix socket, so we can watch output live instead of
polling a log file until some fixed timeout expires. Exits 0 on match.

A fault that appears some seconds after a known marker would otherwise force
every clean run to wait out the whole timeout. --arm names that marker and
--grace how long to keep watching after it: the stop pattern still ends the
wait at once, but a run that stays quiet until the grace period is over counts
as a success.
"""

import argparse
import re
import socket
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("socket", help="path of the serial unix socket")
    ap.add_argument("--stop", required=True, help="regex that ends the wait")
    ap.add_argument("--arm", help="regex after which --grace is armed")
    ap.add_argument("--grace", type=float, default=0.0,
                    help="seconds to keep watching after --arm matches")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--log", help="also append the stream to this file")
    args = ap.parse_args()

    stop = re.compile(args.stop)
    arm = re.compile(args.arm) if args.arm else None
    armed = False
    logf = open(args.log, "ab", buffering=0) if args.log else None

    deadline = time.monotonic() + args.timeout
    conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        conn.settimeout(5.0)
        while True:
            try:
                conn.connect(args.socket)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.monotonic() > deadline:
                    print("serial: socket never appeared", file=sys.stderr)
                    return 1
                time.sleep(0.05)

        conn.settimeout(0.5)
        buf = b""
        while True:
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                # Nothing to read yet, which is not the same as the guest
                # having gone away: a reset machine drops the chardev, and
                # with -no-reboot that means QEMU itself is gone.
                if time.monotonic() > deadline:
                    if armed:
                        print("serial: quiet for the grace period", file=sys.stderr)
                        return 0
                    print("serial: timeout", file=sys.stderr)
                    return 1
                continue
            if not chunk:
                print("serial: the guest went away", file=sys.stderr)
                return 2

            buf += chunk
            text = buf.decode("utf-8", "replace")
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
            if logf:
                logf.write(chunk)

            if stop.search(text):
                print("serial: matched", file=sys.stderr)
                return 0

            if arm is not None and not armed and arm.search(text):
                armed = True
                deadline = time.monotonic() + args.grace
    finally:
        conn.close()
        if logf:
            logf.close()


if __name__ == "__main__":
    sys.exit(main())
