#!/usr/bin/env python3
"""Compare an image in memory against the file it was loaded from.

What this answers is "did we change something we should not have". A boot that
goes wrong after our work is done leaves three possibilities -- we wrote
somewhere we did not mean to, we wrote the right place with the wrong bytes, or
the kernel did something on its own -- and a byte comparison separates the
first two from the third.

The expected differences are known in advance, because this project writes in
exactly two places: the synchronous slots of the vector tables, and the stub
holes it fills with code of its own. Everything else matching is the useful
result; everything else *differing* is the finding.

Read-only sections only, by default: a data section legitimately changes as the
kernel runs, and comparing those would bury the signal.

Usage: image_diff.py --image FILE --kbase HEX [--qmp-port N] [--all]
"""

import argparse
import json
import re
import socket
import struct
import sys


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


def read(sock_file, addr, count):
    """`count` doublewords of virtual memory, in blocks.

    One command per word would take minutes for a section; the monitor takes a
    few hundred at a time.
    """
    out = []
    done = 0
    while done < count:
        want = min(count - done, 512)
        text = hmp(sock_file, "x /%dgx 0x%x" % (want, addr + done * 8))
        got = 0
        for line in text.splitlines():
            if ":" not in line:
                continue
            row = [int(v, 16) for v in re.findall(r"0x([0-9a-f]+)", line.split(":", 1)[1])]
            out += row
            got += len(row)
        if got == 0:
            break
        done += got
    return b"".join(w.to_bytes(8, "little") for w in out)


def sections(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    out = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        name = d[o:o + 8].rstrip(b"\0").decode()
        vsz, va, rsz, ra = struct.unpack_from("<IIII", d, o + 8)
        ch = struct.unpack_from("<I", d, o + 36)[0]
        out.append({"name": name, "va": va, "vsz": vsz, "rsz": rsz, "ra": ra,
                    "ch": ch, "file": d})
    return out


IMAGE_SCN_MEM_WRITE = 0x80000000
IMAGE_SCN_MEM_EXECUTE = 0x20000000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--kbase", required=True, type=lambda s: int(s, 0))
    ap.add_argument("--qmp-port", type=int, default=4447)
    # A writable section changes as the kernel runs, so comparing it says
    # nothing. Both of this project's own writes are in executable sections.
    ap.add_argument("--all", action="store_true",
                    help="compare writable sections too")
    ap.add_argument("--max-runs", type=int, default=40)
    args = ap.parse_args()

    sock = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20)
    f = sock.makefile("rwb")
    f.readline()
    cmd(f, {"execute": "qmp_capabilities"})

    totalDiff = 0
    print("%-10s %-10s %s" % ("section", "bytes", "differences"))
    for s in sections(args.image):
        if s["rsz"] == 0 or s["vsz"] == 0:
            continue
        if not args.all and (s["ch"] & IMAGE_SCN_MEM_WRITE):
            continue
        want = min(s["vsz"], s["rsz"])
        # Compare whole words: an instruction is four bytes and the monitor's
        # unit is eight, so a mismatch is reported at its own alignment.
        words = (want + 7) // 8
        mem = read(f, args.kbase + s["va"], words)
        fileBytes = s["file"][s["ra"]:s["ra"] + want]
        n = min(len(mem), len(fileBytes)) & ~7

        runs = []
        i = 0
        while i < n:
            if mem[i:i + 8] != fileBytes[i:i + 8]:
                start = i
                while i < n and mem[i:i + 8] != fileBytes[i:i + 8]:
                    i += 8
                runs.append((start, i))
            else:
                i += 8

        same = 0 if not runs else sum(b - a for a, b in runs)
        totalDiff += same
        print("%-10s 0x%-8x %d run(s), %d byte(s)"
              % (s["name"], n, len(runs), same))
        for a, b in runs[:args.max_runs]:
            rva = s["va"] + a
            fm = fileBytes[a:a + 8]
            mm = mem[a:a + 8]
            print("    +0x%06x  file %s   mem %s"
                  % (rva, fm.hex(" "), mm.hex(" ")))
        if len(runs) > args.max_runs:
            print("    ... and %d more runs" % (len(runs) - args.max_runs))

    print()
    print("total differing bytes: %d" % totalDiff)
    print("expected: the vector slot branches and the stub holes this project")
    print("wrote, and nothing else")
    return 0


if __name__ == "__main__":
    sys.exit(main())
