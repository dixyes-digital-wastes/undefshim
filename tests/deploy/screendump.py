#!/usr/bin/env python3
"""Save what the machine's screen shows, as a PPM.

The screen is the one oracle that does not depend on the guest being able to
report anything: a machine that is still running may be booting or may be
spinning, and the difference is on the screen. Convert the result before
looking at it (vision models do not read PPM):

    tests/deploy/screendump.py --qmp-port 4448 --out build/x/screen.ppm
    ffmpeg -y -i build/x/screen.ppm build/x/screen.png
"""

import argparse
import json
import socket


def cmd(sock, obj):
    sock.write((json.dumps(obj) + "\n").encode())
    sock.flush()
    while True:
        line = sock.readline()
        if not line:
            raise SystemExit("the monitor closed the connection")
        reply = json.loads(line)
        if "return" in reply or "error" in reply:
            return reply


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp-port", type=int, default=4448)
    ap.add_argument("--out", default="build/screen.ppm")
    args = ap.parse_args()

    sock = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20)
    f = sock.makefile("rwb")
    f.readline()
    cmd(f, {"execute": "qmp_capabilities"})
    reply = cmd(f, {"execute": "screendump", "arguments": {"filename": args.out}})
    print(reply)
    return 0 if "return" in reply else 1


if __name__ == "__main__":
    raise SystemExit(main())
