#!/usr/bin/env python3
"""Watch a COM port until a milestone shows up, then get out of the way.

The kernel's debugger wait is short, so the window for attaching is the moment
the boot gets far enough to be worth attaching to. Sitting on the port by hand
to catch that moment is a race no one wins.

This reads the port, prints what it reads, and exits the instant the marker
appears - so the port is free and the next thing started can have it. Which is
the point: KD needs the port, and it needs it immediately.

    serial_wait.py COM2
    serial_wait.py COM2 --marker "milestone: M7" --timeout 300

Everything read is kept in COMx-YYYYMMDDHHMMSS.log beside where this is run,
raw, so a run that stops is still readable afterwards.

Windows, and pyserial:

    py -m pip install pyserial

Exits 0 when the marker was seen, 1 when the time ran out, 2 when the port
could not be opened - in which case something else is holding it, and the name
of that something is worth knowing before starting KD.

--selftest runs the matching without a port.
"""

import argparse
import datetime
import re
import sys
import time


# What the driver writes around a line: the levels are colours, and a terminal
# would eat them, but a pipe to a file or another program sees them raw. The
# marker is matched on the text, so they come off first
SGR = re.compile(r"\x1b\[[0-9;]*m")


def plain(text):
    return SGR.sub("", text)


class Finder:
    """Matches a marker against a stream, across however it is chopped up.

    A read returns whatever has arrived, which has nothing to do with where
    lines end - the marker can be split between two of them. So the tail of
    what has been read is kept and the marker looked for in the join.
    """

    # Enough to hold the marker and the partial line before it. A read is
    # short, and holding more than this would only be holding the log
    KEEP = 4096

    def __init__(self, marker):
        self.marker = marker
        self.tail = ""

    def feed(self, text):
        """The text, and whether the marker is in it now."""
        joined = self.tail + plain(text)
        found = self.marker in joined
        self.tail = joined[-self.KEEP:]
        return found


class Gate:
    """Holds back output until a start pattern has gone by.

    Same shape as Finder and for the same reason: what is looked for can be
    split across two reads, so the join is what is searched - and the search
    has to happen before the join is trimmed, or the pattern is cut in half by
    the very thing that keeps the buffer bounded.
    """

    def __init__(self, start):
        self.start = start
        self.tail = ""
        self.open = start == ""

    def allow(self, text):
        """Whether this text is to be shown."""
        if self.open:
            return True
        joined = self.tail + text
        if self.start in joined:
            self.open = True
            return True
        self.tail = joined[-len(self.start):]
        return False


def selftest():
    """The one thing that can silently be wrong: a split read.

    Each case is a run's worth of reads, and the index of the read the marker
    is complete on - which is when this has to answer, because answering late
    is answering after the window. A fresh finder per case, since one that has
    found the marker has it in its tail and would say yes to anything after.
    """
    cases = [
        (["M7 rewritten\n"], 0),
        (["M7 rewrit", "ten\n"], 1),
        (["M7 re", "writ", "ten\n"], 2),
        (["\x1b[96mM7", " rewritten\x1b[0m\n"], 1),
        (["M8 done\n", "M7 rewritten\n"], 1),
        (["M8 done\n", "nothing here\n"], None),
        (["M7 rewritten"], 0),
    ]
    for chunks, want in cases:
        finder = Finder("M7 rewritten")
        got = None
        for i, chunk in enumerate(chunks):
            if finder.feed(chunk):
                got = i
                break
        if got != want:
            print("selftest FAILED %r: want %s got %s" % (chunks, want, got))
            return 1

    # The milestone line is what is waited for, and it is named by the tag it
    # carries rather than by "M7" alone: the firmware's own log talks about
    # the Cortex-M7 when it brings its secure cores up, so a bare M7 fires
    # before the driver has even been loaded
    for line in ["milestone: M7 rewritten\n", "milestone: M7 nothing to rewrite\n"]:
        finder = Finder("milestone: M7")
        if not finder.feed(line):
            print("selftest FAILED: %r is not the milestone" % line)
            return 1
    finder = Finder("milestone: M7")
    if finder.feed("NOTICE: Cortex-M7 secure core up\n"):
        print("selftest FAILED: the firmware's M7 is not the milestone")
        return 1

    print("selftest: the marker is found however the reads are chopped")

    # The gate: nothing before the banner, everything after it, and the banner
    # itself found however it is chopped - including when it is split across
    # reads, which is the case that was wrong first time round
    cases = [
        (["BIOS junk\n", "undefshim: 0.1.0\n", "after\n"], [False, True, True]),
        (["BIOS junk\n", "und", "efshim: 0.1.0\n", "after\n"],
         [False, False, True, True]),
        (["BIOS junk\n", "nothing here\n"], [False, False]),
        (["undefshim: 0.1.0\n"], [True]),
    ]
    for chunks, want in cases:
        gate = Gate("undefshim:")
        got = [gate.allow(c) for c in chunks]
        if got != want:
            print("selftest FAILED gate %r: want %s got %s" % (chunks, want, got))
            return 1

    # An empty start shows everything
    gate = Gate("")
    if not all(gate.allow(c) for c in ["a", "b"]):
        print("selftest FAILED: an empty start should hold nothing back")
        return 1

    print("selftest: and the banner is found the same way")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", help="a serial port, e.g. COM2")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--marker", default="milestone: M7",
                    help="what to wait for; the middle of a line is fine. "
                         "The default is the boot-time rewrite's milestone, "
                         "which is said whatever the switches were: what it "
                         "came to follows ('rewritten', 'nothing to rewrite', "
                         "'skip rewrite'), so up to the M7 covers them all. It "
                         "is not matched until --from has gone by, so a "
                         "pattern the firmware also says cannot fire early")
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--from", dest="start",
                    default="undefshim:",
                    help="do not print, and do not look for the marker, "
                         "until this has been seen; the firmware talks a "
                         "lot before the driver does. Empty starts open")
    ap.add_argument("--quiet", action="store_true",
                    help="do not echo what is read")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.port:
        ap.error("a port is needed, or --selftest")

    # After the argument handling, so --selftest does not need it
    try:
        import serial
    except ImportError:
        print("needs pyserial:  py -m pip install pyserial", file=sys.stderr)
        return 2

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as why:
        print("cannot open %s: %s" % (args.port, why), file=sys.stderr)
        print("something else has it - KD, or another copy of this",
              file=sys.stderr)
        return 2

    finder = Finder(args.marker)
    deadline = time.monotonic() + args.timeout

    # Kept beside wherever this is run from, named for the port and the moment
    # so a directory of them is a list of the runs. Written as it arrives and
    # flushed each time, because the interesting run is the one that stops
    name = "%s-%s.log" % (args.port,
                          datetime.datetime.now().strftime("%Y%m%d%H%M%S"))
    try:
        log = open(name, "wb")
    except OSError as why:
        print("cannot write %s: %s" % (name, why), file=sys.stderr)
        port.close()
        return 2
    print("keeping the stream in %s" % name, file=sys.stderr)

    # What came before the banner is the firmware and the boot manager talking,
    # and there is a lot of it. It is held back from the terminal, and the
    # marker is not looked for until it has gone by - the firmware's own log
    # mentions M7-worthy things of its own, so a marker without that condition
    # fires on a boot that never loaded this driver
    gate = Gate(args.start)

    try:
        while time.monotonic() < deadline:
            data = port.read(4096)
            if not data:
                continue
            # What the port said, escape sequences and all: the point of
            # keeping it is to still have it afterwards
            log.write(data)
            log.flush()
            decoded = data.decode("latin1")
            text = plain(decoded)
            # The gate advances whether or not anything is printed: what it
            # gates is the marker as well as the output
            show = gate.allow(text)
            if show and not args.quiet:
                sys.stdout.write(text)
                sys.stdout.flush()
            if gate.open and finder.feed(decoded):
                if not args.quiet:
                    sys.stdout.write("\n")
                print("%s: %s - the port is free, start KD" % (args.port, args.marker),
                      file=sys.stderr)
                return 0
    except KeyboardInterrupt:
        return 1
    finally:
        port.close()
        log.close()

    print("%s: nothing after %g seconds" % (args.port, args.timeout),
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
