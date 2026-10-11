#!/usr/bin/env python3
"""Read the serial line to the boot milestone, then give it to Windows kd.

This runs on the Windows machine that owns the serial port, and it is the whole
arrangement: one script, one port, one debugger. Nothing else has to be
installed there, and no connection to that machine is needed.

The line carries the driver's own log until the kernel starts, and after that
it carries the kernel debugger. Two readers want different things from one
stream: the log is read to find out how far the boot got, and KD wants the line
to itself at the moment it starts talking. A debugger started early gets it a
timeout, and a reader started late loses the log.

So this opens the port, reads it and prints it, and the instant the milestone
goes by it closes the port and starts kd. kd opens the same port itself, which
is the reason the port has to be let go of first.

kd's two handles, its input and its output, are served on TCP, so the debugger
can be driven from anywhere on the LAN:

    nc 192.0.2.1 5555

When kd exits, the port is taken again and the wait starts over. That is what
makes a rebooted guest work: kd leaves with the guest, and the next boot is
caught the same way. --once stops after the first debugger instead.

Windows, and pyserial:

    py -m pip install pyserial

There are two logs, because the two streams are not the same thing. The raw
line goes to --log byte for byte, and a KD stream can be replayed from it. What
kd says goes to --console-log as text. One file with both would not be either.

Exits 0 when the run was asked to stop and ended, 1 when it was interrupted, 2
when the port or the listen socket could not be opened.

--selftest checks the part that is served on TCP, without a port and without a
debugger.
"""

import argparse
import datetime
import os
import signal
import socket
import subprocess
import sys
import threading
import time

# The marker and the gate are the same problem serial_wait.py has, and the same
# code answers it: a read is not a line, so the marker can be split between two
# of them. That file is beside this one and is imported rather than copied,
# because a second copy is a second thing to keep in step.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from serial_wait import Finder, Gate, plain
except ImportError as why:
    print("needs serial_wait.py beside this file: %s" % why, file=sys.stderr)
    sys.exit(2)


# What is kept for a client that has not connected yet. kd says its banner and
# the target's first words without waiting to be asked, and a client that
# connected a moment later would miss them
KEEP = 1 << 20

# How long to wait after letting go of the port. kd opens the same port this
# just closed, and Windows does not hand the name over in the same instant
PORT_GAP = 0.3

# How long to keep trying for the port, and how often. The last round's kd may
# still be closing it, and another program may be holding it
PORT_TRIES = 20
PORT_TRY_GAP = 0.25


class Link:
    """kd's console, and the client on TCP that drives it.

    What kd says before a client connects is kept and given to that client
    first. What the client types goes to kd's input, and while kd is not
    running the serial log goes to the client instead, so one connection shows
    the whole story: the boot, then the debugger.

    A client that leaves does not end the run. kd is still holding the target,
    and a client restarted wants the same console again, so the next one gets
    what has arrived since. Two at once would interleave keystrokes into one
    console, so they are served one at a time.
    """

    def __init__(self, note):
        self.note = note
        self.lock = threading.Lock()
        self.pending = []
        self.client = None
        self.kd = None

    def setKd(self, kd):
        with self.lock:
            self.kd = kd

    def toClient(self, data):
        """One read, on its way to the client."""
        with self.lock:
            client = self.client
            if client is None:
                self.pending.append(data)
                self.clip()
                return
        try:
            client.sendall(data)
        except OSError as why:
            self.note("the client went away: %s" % why)
            self.drop()
            # It did not land, so it is not lost: it waits for the next
            # client, the same as anything said while there was none
            with self.lock:
                self.pending.append(data)
                self.clip()

    def toKd(self, data):
        """What the client typed, on its way to kd's input."""
        with self.lock:
            kd = self.kd
        if kd is None or kd.stdin is None:
            self.note("kd is not running, so there is nothing to type into")
            return
        try:
            kd.stdin.write(data)
            kd.stdin.flush()
        except OSError as why:
            self.note("kd's input went away: %s" % why)

    def clip(self):
        """Hold the gap down to KEEP bytes. Called with the lock held."""
        total = 0
        keep = 0
        for i in range(len(self.pending) - 1, -1, -1):
            total += len(self.pending[i])
            if total > KEEP:
                break
            keep += 1
        if keep < len(self.pending):
            self.pending = self.pending[len(self.pending) - keep:]

    def drop(self):
        with self.lock:
            client = self.client
            self.client = None
        if client is not None:
            try:
                client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            client.close()

    def serve(self, listener, stop):
        """One client at a time, for as long as this runs."""
        while not stop.is_set():
            try:
                client, from_ = listener.accept()
            except OSError as why:
                self.note("the socket was not accepted: %s" % why)
                return
            with self.lock:
                self.client = client
                held = self.pending
                self.pending = []
            self.note("a client from %s:%d" % from_[:2])
            ok = True
            # Whatever kd said before the client arrived, in the order it said
            for chunk in held:
                try:
                    client.sendall(chunk)
                except OSError:
                    ok = False
                    break
            if ok:
                try:
                    while True:
                        data = client.recv(4096)
                        if not data:
                            break
                        self.toKd(data)
                except OSError as why:
                    self.note("the client went away: %s" % why)
            self.drop()
            self.note("the client left; the next one gets what arrives from "
                      "here")


def haveSerial():
    """pyserial, or a message that says how to get it and None.

    Imported here rather than at the top so --selftest runs on a machine that
    has never had a serial port, and asked for before the logs are made so a
    machine without it does not leave two empty files behind
    """
    try:
        import serial
        return serial
    except ImportError:
        print("needs pyserial:  py -m pip install pyserial", file=sys.stderr)
        return None


def openPort(args, note):
    """The port, or None when something else is holding it."""
    import serial

    last = None
    for attempt in range(PORT_TRIES):
        try:
            port = serial.Serial(args.port, args.baud, timeout=0.2)
            if attempt:
                note("%s is free again" % args.port)
            return port
        except serial.SerialException as why:
            last = why
            if attempt == 0:
                note("%s is held: %s" % (args.port, why))
            time.sleep(PORT_TRY_GAP)
    print("cannot open %s: %s" % (args.port, last), file=sys.stderr)
    print("something else has it - a debugger, or another copy of this",
          file=sys.stderr)
    return None


def readToMarker(port, args, note, link, serialLog, midLine):
    """The line, until the milestone. Prints it and keeps all of it."""
    finder = Finder(args.marker)
    gate = Gate(args.start)
    started = time.monotonic()
    while True:
        try:
            data = port.read(4096)
        except OSError as why:
            note("the port went away: %s" % why)
            return False
        if data:
            serialLog.write(data)
            serialLog.flush()
            link.toClient(data)
            text = plain(data.decode("latin1"))
            if not args.quiet and gate.allow(text):
                sys.stdout.write(text)
                sys.stdout.flush()
                midLine[0] = not text.endswith("\n")
            # The marker is only looked for once the driver has been seen: the
            # firmware says a lot before it, and a marker without that would
            # fire on a boot that never loaded this driver
            if gate.open and finder.feed(data.decode("latin1")):
                return True
            continue
        if args.marker_timeout > 0 \
                and time.monotonic() - started > args.marker_timeout:
            return False


def startKd(args, note):
    """kd on the port, with its console on pipes."""
    command = [args.kd, "-k", "com:port=%s,baud=%d" % (args.port, args.baud)]
    if args.kd_log:
        command += ["-logo", args.kd_log]
    note("starting %s" % " ".join(command))
    try:
        return subprocess.Popen(command, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
    except OSError as why:
        print("cannot start %s: %s" % (args.kd, why), file=sys.stderr)
        print("give the full path with --kd if it is not on PATH",
              file=sys.stderr)
        return None


def pumpKd(proc, link, consoleLog):
    """kd's output, to the client, to its own log, and to this console."""
    while True:
        try:
            # read1, not read: read waits for the whole length asked for, and
            # an interactive console would then arrive in one lump at the end
            data = proc.stdout.read1(4096)
        except (OSError, ValueError):
            return
        if not data:
            return
        consoleLog.write(data)
        consoleLog.flush()
        link.toClient(data)
        sys.stdout.write(data.decode("latin1"))
        sys.stdout.flush()


def selftest():
    """The TCP side, which is the part that has no other way to be checked.

    Three things can be wrong here and none shows up as an error. What kd said
    before the client arrived can be dropped, which looks like a debugger that
    never started. What the client types can fail to reach kd, which looks like
    a target that does not answer. And the client leaving can end the run,
    which looks like a session that cannot be picked up again.
    """
    import io

    class FakeKd:
        """kd's input, kept where the test can read it."""

        def __init__(self):
            self.stdin = io.BytesIO()

    notes = []
    link = Link(notes.append)
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    where = listener.getsockname()

    stop = threading.Event()
    side = threading.Thread(target=link.serve, args=(listener, stop),
                            daemon=True)
    side.start()

    def connect():
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        for _ in range(200):
            try:
                client.connect(where)
                client.settimeout(3)
                return client
            except OSError:
                time.sleep(0.01)
        return None

    def read(client):
        """What the client got, or nothing when nothing came in time.

        A timeout here is the failure this test exists for, so it comes back as
        an empty answer and the caller says which one it was, rather than as a
        traceback from somewhere below
        """
        try:
            return client.recv(64)
        except TimeoutError:
            return b""

    def readUntil(client, want, seconds=3.0):
        """The client's bytes up to and including want.

        A socket deliver is not a write: the two sends below can arrive as one
        read or as two, and a single recv would sometimes see only the first
        """
        got = b""
        deadline = time.monotonic() + seconds
        while want not in got and time.monotonic() < deadline:
            chunk = read(client)
            if not chunk:
                continue
            got += chunk
        return got

    # What kd said before anyone was listening
    link.toClient(b"kd> ")
    link.toClient(b"waiting for target")

    first = connect()
    if first is None:
        print("selftest FAILED: the socket was never there")
        return 1
    got = readUntil(first, b"waiting for target")
    if got != b"kd> waiting for target":
        print("selftest FAILED: the first client got %r" % got)
        return 1

    # The client's keystrokes, which have to reach kd
    kd = FakeKd()
    link.setKd(kd)
    first.sendall(b"kb\n")
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline and kd.stdin.getvalue() != b"kb\n":
        time.sleep(0.01)
    if kd.stdin.getvalue() != b"kb\n":
        print("selftest FAILED: kd's input got %r" % kd.stdin.getvalue())
        return 1

    # And typing while kd is not running is a note, not a crash
    link.setKd(None)
    first.sendall(b"g\n")
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline \
            and not any("nothing to type into" in n for n in notes):
        time.sleep(0.01)
    if not any("nothing to type into" in n for n in notes):
        print("selftest FAILED: typing with no kd was not reported")
        return 1

    # The second client, which is a debugger console restarted
    first.close()
    second = connect()
    if second is None:
        print("selftest FAILED: the socket was gone after the first client")
        return 1
    link.toClient(b"after the restart")
    got = readUntil(second, b"after the restart")
    if got != b"after the restart":
        print("selftest FAILED: the second client got %r" % got)
        return 1

    second.close()
    stop.set()
    listener.close()
    side.join(timeout=3)
    print("selftest: the gap is kept, both directions pass, and a client can "
          "come back")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM2",
                    help="the COM port the debug cable is on")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--host", default="0.0.0.0",
                    help="the address to serve kd's console on; the default is "
                         "every interface, because the client is usually "
                         "another machine")
    ap.add_argument("--tcp-port", type=int, default=5555, dest="tcpPort")
    ap.add_argument("--marker", default="milestone: M7",
                    help="what to wait for; the middle of a line is fine. The "
                         "default is the boot-time rewrite's milestone, which "
                         "is said whatever the switches were: what it came to "
                         "follows, so up to the M7 covers them all")
    ap.add_argument("--from", dest="start", default="undefshim:",
                    help="do not print, and do not look for the marker, until "
                         "this has been seen; the firmware talks a lot before "
                         "the driver does. Empty starts open")
    ap.add_argument("--marker-timeout", type=float, default=0.0,
                    help="give the line to kd after this many seconds even "
                         "without the marker. The default is never: a machine "
                         "being reinstalled can take much longer than any "
                         "timeout worth guessing, and giving the line up early "
                         "is what this is here to avoid. Give a positive "
                         "number only when a boot that never reaches the "
                         "marker is the thing being looked for")
    ap.add_argument("--kd", default="kd.exe",
                    help="kd on this machine; give the full path if it is not "
                         "on PATH")
    ap.add_argument("--kd-log",
                    help="ask kd for its own copy of the session here; use it "
                         "if kd's console arrives late or not at all, because "
                         "kd may hold its output while that output is a pipe")
    ap.add_argument("--log", help="where to keep the raw line, byte for "
                                  "byte, so a boot can be read again "
                                  "afterwards; default is "
                                  "COMx-YYYYMMDDHHMMSS.log beside this")
    ap.add_argument("--console-log",
                    help="where to keep what kd says, as text; default is "
                         "kd-console-YYYYMMDDHHMMSS.log beside this. It is "
                         "a second file because the raw line is what a KD "
                         "stream can be replayed from, and kd's text in the "
                         "middle of it would end that")
    ap.add_argument("--once", action="store_true",
                    help="stop after the first debugger rather than waiting "
                         "for the next boot")
    ap.add_argument("--quiet", action="store_true",
                    help="do not echo the line")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if haveSerial() is None:
        return 2

    # A term signal has to stop this the way an interrupt does, because a
    # debugger left running against a console nobody reads is the one outcome
    # to avoid, and only the cleanup below stops it in the right order
    try:
        signal.signal(signal.SIGTERM, signal.default_int_handler)
    except (ValueError, OSError):
        pass

    # The milestone is matched on a part of a line, so the echo can stop in the
    # middle of one and the notes would land on its end
    midLine = [False]

    def note(line):
        if midLine[0]:
            sys.stderr.write("\n")
        print("serial_kd: %s" % line, file=sys.stderr)
        midLine[0] = False

    # The listener first: a port already in use should not leave two empty
    # logs behind, and it is the failure a second copy of this makes
    try:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.host, args.tcpPort))
        listener.listen(1)
    except OSError as why:
        print("cannot serve on %s:%d: %s"
              % (args.host, args.tcpPort, why), file=sys.stderr)
        return 2

    stamp = datetime.datetime.now().strftime("%Y%m%d%H%M%S")
    names = [(args.log or "%s-%s.log" % (args.port, stamp), "the line"),
             (args.console_log or "kd-console-%s.log" % stamp, "kd")]
    logs = []
    try:
        for path, what in names:
            logs.append((path, what, open(path, "wb", 0)))
    except OSError as why:
        for _, _, opened in logs:
            opened.close()
        listener.close()
        print("cannot write a log: %s" % why, file=sys.stderr)
        return 2
    # The raw line and what kd says are two files because they are two things:
    # a KD stream can be replayed from the first, and kd's text in the middle
    # of it would end that
    serialLog = logs[0][2]
    consoleLog = logs[1][2]
    for path, what in names:
        note("keeping %s in %s" % (what, path))
    note("kd's console will be on %s:%d" % (args.host, args.tcpPort))

    link = Link(note)
    stop = threading.Event()
    threading.Thread(target=link.serve, args=(listener, stop),
                     daemon=True).start()

    kd = None
    try:
        while True:
            port = openPort(args, note)
            if port is None:
                return 2
            note("reading %s at %d baud" % (args.port, args.baud))
            try:
                found = readToMarker(port, args, note, link, serialLog, midLine)
            finally:
                port.close()
            if found:
                note("the milestone went by")
            else:
                note("no %r within %g seconds; giving the line up anyway"
                     % (args.marker, args.marker_timeout))
            note("the port is free")

            # Windows does not hand the name over in the same instant
            time.sleep(PORT_GAP)
            kd = startKd(args, note)
            if kd is None:
                return 2
            link.setKd(kd)
            pump = threading.Thread(target=pumpKd,
                                    args=(kd, link, consoleLog), daemon=True)
            pump.start()
            print()
            print("kd is on this machine's %s; drive it with:" % args.port)
            print("    nc %s %d" % (args.host, args.tcpPort))
            print()
            sys.stdout.flush()
            code = kd.wait()
            pump.join(timeout=5)
            link.setKd(None)
            kd = None
            note("kd exited with %s" % code)
            if args.once:
                break
            # kd left with the guest, so the port is free for the next boot
    except KeyboardInterrupt:
        return 1
    finally:
        stop.set()
        listener.close()
        if kd is not None:
            # kd resumes the target on the way out, which is the point: a
            # guest halted with no debugger is a guest that hangs
            try:
                kd.stdin.write(b"q\n")
                kd.stdin.flush()
            except (OSError, ValueError):
                pass
            try:
                kd.wait(timeout=5)
            except subprocess.TimeoutExpired:
                kd.terminate()
                try:
                    kd.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    kd.kill()
        for _, _, opened in logs:
            opened.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
