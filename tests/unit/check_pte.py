#!/usr/bin/env python3
"""Check the self-map computation without a machine.

The constants are read out of the header rather than written again here, so
this fails if they drift. What it then checks are the properties a wrong
computation would break, one of which is an expected value the kernel itself
supplied: the literal 0xfffff6fb7dbedf68 sits beside the base constant in
.text and is the descriptor for the self-map's own entry, which the formula
has to produce exactly.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
text = (ROOT / "core/pgtable.h").read_text()


def const(name):
    m = re.search(r"#define\s+%s\s+UINT64_C\(0x([0-9A-Fa-f]+)\)" % name, text)
    assert m, "missing " + name
    return int(m.group(1), 16)


BASE = const("US_PTE_SELFMAP_BASE")
END = const("US_PTE_SELFMAP_END")
MASK = const("US_PTE_VA_MASK")
AP2 = 1 << 7

failures = 0
checks = 0


def check(name, cond):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print("FAIL " + name)


def pte(va):
    return BASE + ((va & MASK) >> 9)


def with_write(desc, writable):
    return (desc & ~AP2) if writable else (desc | AP2)


# The kernel's own literal: the descriptor of VA 0xfffff6fb7dbed000.
check("the self-map's own descriptor comes out where the kernel puts it",
      pte(0xFFFFF6FB7DBED000) == 0xFFFFF6FB7DBEDF68)

# Whatever the address, the descriptor is inside the region the kernel also
# describes with a literal, and on a descriptor boundary.
for va in (0, 0xFFF, 0x1000, 0x7FF000, 0xFFFFF802DC204800, 0xFFFFF802DC204FFF,
           0xFFFFFFFFFFFFF000, 0xFFFF0000ABCDE000):
    p = pte(va)
    check("in range: 0x%x" % va, BASE <= p <= END)
    check("aligned: 0x%x" % va, p % 8 == 0)

# One page, one descriptor; the next page, the next one.
check("the whole of a page shares its descriptor",
      pte(0xFFFFF802DC204000) == pte(0xFFFFF802DC204FFF))
check("the next page is the next descriptor",
      pte(0xFFFFF802DC205000) - pte(0xFFFFF802DC204000) == 8)

# Writing through the self-map may only move AP[2]; everything else - the
# output address, the attributes, the valid bit - has to come back untouched.
for desc in (0x0000000000000001, 0x0000FFFFFFFFF70F, 0x8000000000000403,
             0x00000000270DB4FF):
    for writable in (True, False):
        got = with_write(desc, writable)
        check("only AP[2] moves (0x%x)" % desc, (got ^ desc) in (0, AP2))
        check("writable when asked (0x%x)" % desc, (got & AP2 == 0) == writable)
        check("and back (0x%x)" % desc, with_write(got, (desc & AP2) == 0) == desc)

print("%d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
