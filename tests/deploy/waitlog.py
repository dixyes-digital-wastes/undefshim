#!/usr/bin/env python3
"""Wait for something to happen in a log, and print what it says as it does.

A run under QEMU takes minutes, and waiting for it with a fixed sleep is
either a wait that outlasts the run or one that ends before it did. This waits
on the things that actually change: a line matching a pattern, or the process
writing the log going away.

    waitlog.py --file build/x.log --pattern '^\\[(PASS|FAIL|SKIP)\\]' --pid 1234
    waitlog.py --file build/x.log --pid 1234 --timeout 1800

Every new line is printed as it arrives, through the same filter the checks
read logs with, so what is watched is what they would see.

Exits 0 when the pattern was seen, 3 when the writer went away first (with
whatever it last said printed above), 1 when the time ran out.
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import logtext                                          # noqa: E402

# How long to go between noticing changes. Small enough that a line is read as
# soon as it is written as far as anyone watching is concerned, and large
# enough that this is not the thing the machine is busy with
STEP = 0.2


def alive(pid):
    if pid is None:
        return True
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", required=True)
    ap.add_argument("--pid", type=int, help="the writer; a wait ends when it does")
    ap.add_argument("--pattern", help="a regular expression to wait for")
    ap.add_argument("--timeout", type=float, default=3600)
    ap.add_argument("--from-start", action="store_true",
                    help="print what is already there too, rather than only "
                         "what arrives")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    pattern = re.compile(args.pattern) if args.pattern else None
    offset = 0                 # bytes of the log already read
    if not args.from_start and os.path.exists(args.file):
        offset = os.path.getsize(args.file)
    pending = ""               # the tail, for matching a pattern line by line
    deadline = time.monotonic() + args.timeout
    writerWasThere = args.pid is not None

    # The file may not exist yet: a run creates its log when it starts
    while True:
        if os.path.exists(args.file):
            with open(args.file, "rb") as f:
                f.seek(offset)
                data = f.read()
                offset = f.tell()
            if data:
                text = logtext.plain(data.decode("latin1"))
                if not args.quiet and text:
                    sys.stdout.write(text)
                    sys.stdout.flush()
                pending += text
                if pattern is not None:
                    for line in pending.splitlines(keepends=True):
                        if pattern.search(line):
                            return 0
                # keep only the tail: a pattern is matched line by line, and
                # holding the whole log would grow without bound
                if len(pending) > 65536:
                    pending = pending[-65536:]
                if pattern is None and not alive(args.pid) and writerWasThere:
                    return 3
        elif args.pid is not None and not alive(args.pid) and writerWasThere:
            # It went away before writing anything
            return 3

        if not alive(args.pid) and writerWasThere:
            # One more read, in case the last lines landed as it exited
            time.sleep(STEP)
            writerWasThere = False
            continue
        if not writerWasThere and args.pid is not None:
            return 3
        if time.monotonic() >= deadline:
            print("waitlog: nothing after %g seconds" % args.timeout, file=sys.stderr)
            return 1
        time.sleep(STEP)


if __name__ == "__main__":
    sys.exit(main())
