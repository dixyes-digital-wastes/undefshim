#!/usr/bin/env python3
"""Check that the tools' idea of the pool's layout is the compiler's.

Every reading tool walks the pool by hand-computed byte offsets, and the C side
of the same layout is a struct the compiler decides. Nothing connects the two,
so an offset that is wrong by one field reads a neighbour and looks entirely
plausible -- which has happened, twice, and sent a diagnosis the wrong way both
times.

So every field the tools name is compared with the offset the compiler gives
it, and a field the tools name but this file does not know is an error rather
than something quietly skipped.

Usage: check_layout.py <root>
"""

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# The offsets come from the tools, so the tools have to be the file on disk and
# not a cached copy of an older one: a stale cache here is a check that passes
# while what it is checking has changed, which is worse than no check
shutil.rmtree(Path(__file__).resolve().parents[1] / "deploy" / "__pycache__",
              ignore_errors=True)

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "deploy"))
import pool_dump  # noqa: E402

# The tool's name for a field, and the C name of the same one. They differ only
# where a trailing acronym is capitalised in C.
ALIASES = {
    "handbackEsr": "handbackESR", "handbackElr": "handbackELR",
    "stuckEsr": "stuckESR", "stuckElr": "stuckELR",
    "stuckSpsr": "stuckSPSR", "stuckVbar": "stuckVBAR",
    "nestedEsr": "nestedESR", "nestedFar": "nestedFAR",
    "nestedElr": "nestedELR", "nestedSelfVa": "nestedSelfVA",
    "nestedCpu": "nestedCPU", "lastEsr": "lastESR", "lastElr": "lastELR",
    "lastFar": "lastFAR", "lastCpu": "lastCPU", "lastSp": "lastSP",
    "emuElr": "emuELR",
    "panicElr": "panicELR", "panicEsr": "panicESR", "panicFar": "panicFAR",
    "panicSpsr": "panicSPSR",
}

SOURCE = r"""
#include <stddef.h>
#include <stdio.h>
#include "common/layout.h"

int main(void) {
    printf("UsPool_entry %%zu\n", offsetof(UsPool, entry));
    printf("UsPoolFB_at %%zu\n", offsetof(UsPool, fb));
%s
    return 0;
}
"""


def expected():
    """Every offset the tools use, by the name they use for it."""
    want = {}
    for i, name in enumerate(pool_dump.PUBLIC):
        want[name] = i * 8
    for name, at in pool_dump.entryOffsets().items():
        if name == "rewrite":          # an array of records, not a field
            continue
        want[name] = at
    return want


def expectedFB():
    """The frame buffer record's fields, as offsets from the pool."""
    return pool_dump.fbOffsets()


def compilerOffsets(root, names, fields):
    body = "".join('    printf("%s %%zu\\n", offsetof(UsPoolEntry, %s));\n'
                   % (c, c) for c in names)
    # The wide fields, then the four narrow ones, each as its own offset from
    # the pool: which of them is where is exactly what is being checked
    body += "".join(
        '    printf("UsPoolFB_%s %%zu\\n", offsetof(UsPool, fb)'
        ' + offsetof(UsPoolFB, %s));\n' % (f, f) for f in fields)
    body += "".join(
        '    printf("UsPoolFB_%s %%zu\\n", offsetof(UsPool, fb)'
        ' + offsetof(UsPoolFB, %s));\n' % (n, n)
        for n in pool_dump.FB_TAIL_FIELDS)
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "offsets.c"
        exe = Path(tmp) / "offsets"
        src.write_text(SOURCE % body)
        built = subprocess.run(["clang", "-std=gnu23", "-I", str(root),
                                str(src), "-o", str(exe)],
                               capture_output=True, text=True)
        if built.returncode != 0:
            print("check_layout: could not compile common/layout.h")
            print(built.stderr.strip())
            return None
        out = subprocess.run([str(exe)], capture_output=True, text=True).stdout
    got = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].isdigit():
            got[parts[0]] = int(parts[1])
    return got


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    want = expected()
    cnames = sorted(set(ALIASES.get(n, n) for n in want))
    fbWant = expectedFB()
    # The names the compiler is asked for are the structure's own, which for
    # this record are the ones the tools use
    fbfields = sorted(fbWant)
    got = compilerOffsets(root, cnames, fbfields)
    if got is None:
        return 2

    failures = 0
    # The header the tools add to reach the entry, which is the pool's own two
    # fields and nothing else
    if got.get("UsPool_entry") != 8:
        print("check_layout: UsPool.entry is at %s in the header, and the "
              "tools add 8" % got.get("UsPool_entry"))
        failures += 1

    checked = 0
    for name in sorted(want):
        cname = ALIASES.get(name, name)
        if cname not in got:
            print("check_layout: the compiler did not report %s (from %s)"
                  % (cname, name))
            failures += 1
            continue
        checked += 1
        if got[cname] != want[name]:
            print("check_layout: %s is at %d in the header and %d in the tools"
                  % (name, got[cname], want[name]))
            failures += 1

    # The frame buffer record, which the tools reach by its own offset in the
    # pool plus a field offset: two hand counted numbers, and either being
    # wrong reads a plausible value out of the wrong word
    checked += 1
    if got.get("UsPoolFB_at") != pool_dump.FB_AT:
        print("check_layout: UsPool.fb is at %s in the header, and the tools "
              "add %d" % (got.get("UsPoolFB_at"), pool_dump.FB_AT))
        failures += 1
    for name in fbfields:
        key = "UsPoolFB_" + name
        if key not in got:
            print("check_layout: the compiler did not report %s" % key)
            failures += 1
            continue
        checked += 1
        if got[key] != fbWant[name]:
            print("check_layout: UsPoolFB.%s is at %d in the header and %d in "
                  "the tools" % (name, got[key], fbWant[name]))
            failures += 1

    print("check_layout: %d offsets checked, %d failures" % (checked, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
