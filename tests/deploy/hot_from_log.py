#!/usr/bin/env python3
"""Read the sites a run was trapping on out of its logs.

The payload keeps the last exception, and pool_dump prints it, so a run's
serial log and pool dump are enough to say where the time went without
touching the machine again. Sites are merged into a list, with the symbol
each one lives in as a comment when symbols are known.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def readSites(path):
    sites = {}
    if not path or not os.path.exists(path):
        return sites
    text = open(path, "rb").read().decode("latin1", "replace")
    for line in text.splitlines():
        body = line.split("#", 1)[0]
        match = re.match(r"\s*0x([0-9a-fA-F]+)\s+([0-9a-fA-F]{8})\b", body)
        if match:
            sites[int(match.group(1), 16)] = int(match.group(2), 16)
    return sites


def hotFromLog(path):
    """The addresses this log says were trapping, most recent first."""
    text = open(path, "rb").read().decode("latin1", "replace")
    hot = []
    kbase = None
    base = re.search(r"^kbase = (0x[0-9a-f]+)", text, re.M)
    if base:
        kbase = int(base.group(1), 16)
    for match in re.finditer(r"lastElr\s*=\s*(0x[0-9a-f]+)", text):
        hot.append(int(match.group(1), 16))
    for match in re.finditer(r"\belr\s*=\s*(0x[0-9a-f]+)", text):
        hot.append(int(match.group(1), 16))
    if kbase is None:
        return []
    return sorted({value - kbase for value in hot if value > kbase})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", help="serial logs or pool dumps")
    parser.add_argument("--sites", default="", help="an existing list to merge into")
    parser.add_argument("--symbols", default="build/symbols.tsv")
    parser.add_argument("--out", default="")
    args = parser.parse_args()

    symbols = []
    if os.path.exists(args.symbols):
        for line in open(args.symbols, encoding="utf-8", errors="replace"):
            parts = line.split("\t")
            if len(parts) >= 3 and parts[0].startswith("0x") and parts[1].strip() == ".text":
                try:
                    symbols.append((int(parts[0], 16), parts[2].strip()))
                except ValueError:
                    pass
        symbols.sort()

    def nameOf(rva):
        name = None
        for start, candidate in symbols:
            if start > rva:
                break
            name = candidate
        return name or ("sub_%x" % rva)

    sites = readSites(args.sites)
    for log in args.logs:
        for rva in hotFromLog(log):
            sites.setdefault(rva, None)
    print("%d sites" % len(sites))
    for rva in sorted(sites):
        print("  0x%08x  # %s" % (rva, nameOf(rva)))
    if args.out:
        with open(args.out, "w", encoding="ascii") as handle:
            for rva in sorted(sites):
                handle.write("0x%x  # %s\n" % (rva, nameOf(rva)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
