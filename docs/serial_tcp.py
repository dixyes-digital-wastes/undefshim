#!/usr/bin/env python3
"""Serve a serial port on TCP, so the reader of it can be somewhere else.

The debugged machine's serial line ends at this machine, and the thing that
wants to read it is a virtual machine that cannot see the port. This reads the
port and serves it to whoever connects, in both directions, so the far side
has the line without having the hardware.

    serial_tcp.py COM2
    serial_tcp.py COM2 --port 5555 --log run.log

Windows, and pyserial:

    py -m pip install pyserial

The port is opened once and held for as long as this runs. A debugger that
took it back for itself would be reading a line this is also reading, and two
readers of one serial line is a way to have neither.

Everything read is written to --log as it arrives, raw, so a run that goes
wrong is still readable afterwards.

One client at a time. A second one waits for the first to go away rather than
sharing, for the same reason.

Exits 0 when the port was served and the client left, 1 when the run was
interrupted, 2 when the port or the listen socket could not be opened - in
which case the port is held by something else, and the name of that something
is worth knowing before starting a debugger.

--selftest checks the framing that can silently be wrong, without a port.
"""

import argparse
import datetime
import socket
import sys
import threading
import time


def pumpSerial(port, conn, log, stop, note):
    """The port to the client, keeping a copy on the way past."""
    while not stop.is_set():
        try:
            data = port.read(4096)
        except OSError as why:
            note("the port went away: %s" % why)
            return
        if not data:
            continue
        log.write(data)
        log.flush()
        try:
            conn.sendall(data)
        except OSError as why:
            note("the client went away: %s" % why)
            return


def pumpClient(conn, port, stop, note):
    """The client to the port. This is the debugger's side of the line."""
    while not stop.is_set():
        try:
            data = conn.recv(4096)
        except OSError as why:
            note("the client went away: %s" % why)
            return
        if not data:
            note("the client left")
            return
        try:
            port.write(data)
        except OSError as why:
            note("the port went away: %s" % why)
            return


def serve(port, listener, log, note):
    """One client, served until it leaves."""
    while True:
        conn, from_ = listener.accept()
        note("a client from %s:%d" % from_[:2])
        stop = threading.Event()
        up = threading.Thread(target=pumpSerial,
                              args=(port, conn, log, stop, note), daemon=True)
        up.start()
        try:
            pumpClient(conn, port, stop, note)
        finally:
            stop.set()
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            conn.close()
            up.join(timeout=1)
        note("the line is free again")


def selftest():
    """The framing, which is the part that cannot be seen by looking.

    A socket deliver is not a serial read: what arrives is whatever was sent,
    possibly several writes at once and possibly half of one. So the copy that
    is kept and the copy that is passed on are the same bytes in the same
    order, and this checks that they stay so across arbitrary chops.

    The two directions are checked one at a time. Run together they race: a
    client with nothing more to say looks exactly like a client that has left,
    and the run would end before the port had finished talking.
    """
    import io

    expected = b"undefshim: 0.1.0\r\nmilestone: M7\r\n"
    chunks = [b"undefshim", b": 0.1.0\r\n", b"milestone: M7\r\n"]

    class FakePort:
        """Reads its chunks once, then has nothing to say."""

        def __init__(self, chunks):
            self.chunks = list(chunks)
            self.written = b""

        def read(self, _n):
            if not self.chunks:
                return b""
            return self.chunks.pop(0)

        def write(self, data):
            self.written += data

    class FakeConn:
        """Sends what it was given, then either ends or says nothing more.

        Saying nothing more is not the same as ending, and the difference is
        the whole of the race above, so the two are separate switches.
        """

        def __init__(self, chunks=(), thenEnd=True):
            self.chunks = list(chunks)
            self.thenEnd = thenEnd
            self.sent = b""
            self.idle = threading.Event()

        def recv(self, _n):
            if self.chunks:
                return self.chunks.pop(0)
            if self.thenEnd:
                return b""
            self.idle.wait(2)
            return b""

        def sendall(self, data):
            self.sent += data

        def shutdown(self, _how):
            pass

        def close(self):
            pass

    # The port to the client, which is what a debugger reads
    port = FakePort(chunks)
    conn = FakeConn(thenEnd=False)
    log = io.BytesIO()
    stop = threading.Event()
    up = threading.Thread(target=pumpSerial,
                          args=(port, conn, log, stop, lambda _s: None),
                          daemon=True)
    up.start()
    for _ in range(500):
        if conn.sent == expected:
            break
        time.sleep(0.002)
    stop.set()
    conn.idle.set()
    up.join(timeout=1)

    if log.getvalue() != expected:
        print("selftest FAILED: logged %r, wanted %r" % (log.getvalue(), expected))
        return 1
    if conn.sent != expected:
        print("selftest FAILED: sent %r, wanted %r" % (conn.sent, expected))
        return 1

    # The client to the port, which is the debugger talking
    port = FakePort([])
    conn = FakeConn([b"kd ", b"packet"])
    stop = threading.Event()
    pumpClient(conn, port, stop, lambda _s: None)
    if port.written != b"kd packet":
        print("selftest FAILED: the port got %r" % port.written)
        return 1

    print("selftest: what is read is kept and passed on, byte for byte")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", help="a serial port, e.g. COM2")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--host", default="0.0.0.0",
                    help="the address to serve on; the default is every "
                         "interface, because the reader is usually another "
                         "machine")
    ap.add_argument("--port-number", type=int, default=5555, dest="tcpPort",
                    help="the TCP port to serve on")
    ap.add_argument("--log", help="where to keep what is read; default is "
                                  "COMx-YYYYMMDDHHMMSS.log beside this")
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

    def note(line):
        print("%s: %s" % (args.port, line), file=sys.stderr)

    try:
        port = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as why:
        print("cannot open %s: %s" % (args.port, why), file=sys.stderr)
        print("something else has it - a debugger, or another copy of this",
              file=sys.stderr)
        return 2
    note("open at %d baud" % args.baud)

    name = args.log or "%s-%s.log" % (
        args.port, datetime.datetime.now().strftime("%Y%m%d%H%M%S"))
    try:
        log = open(name, "wb")
    except OSError as why:
        print("cannot write %s: %s" % (name, why), file=sys.stderr)
        port.close()
        return 2
    note("keeping the line in %s" % name)

    try:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.host, args.tcpPort))
        listener.listen(1)
    except OSError as why:
        print("cannot serve on %s:%d: %s"
              % (args.host, args.tcpPort, why), file=sys.stderr)
        log.close()
        port.close()
        return 2

    note("serving the line on %s:%d" % (args.host, args.tcpPort))
    try:
        serve(port, listener, log, note)
    except KeyboardInterrupt:
        return 1
    finally:
        listener.close()
        log.close()
        port.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
