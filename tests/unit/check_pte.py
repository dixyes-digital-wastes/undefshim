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
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check as checklib

REPORT = checklib.Check("check_pte")
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
PDE_BASE = const("US_PDE_SELFMAP_BASE")
PPE_BASE = const("US_PPE_SELFMAP_BASE")
PXE_BASE = const("US_PXE_SELFMAP_BASE")
AP2 = 1 << 7

failures = 0
checks = 0


def check(name, cond):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        REPORT.fail(name)


def pte(va):
    return BASE + ((va & MASK) >> 9)


def with_write(desc, writable):
    return (desc & ~AP2) if writable else (desc | AP2)


# The kernel's own literal: the descriptor of VA 0xfffff6fb7dbed000.
check("the self-map's own descriptor comes out where the kernel puts it",
      pte(0xFFFFF6FB7DBED000) == 0xFFFFF6FB7DBEDF68)

# Each level's descriptors are mapped by the level below, so the bases are a
# recursion through the same formula, and two of them are values the x64 kernel
# uses for the same levels.
check("the page descriptor base's own descriptor is the next base",
      pte(BASE) == PDE_BASE)
check("that one is the value x64 uses for the same level",
      PDE_BASE == 0xFFFFF6FB40000000)
check("one level up follows the same rule", pte(PDE_BASE) == PPE_BASE)
check("and is the x64 value as well", PPE_BASE == 0xFFFFF6FB7DA00000)
check("as does the top", pte(PPE_BASE) == PXE_BASE)
check("and the image carries a descriptor for the top one",
      pte(PXE_BASE) == 0xFFFFF6FB7DBEDF68)
check("the four bases are distinct",
      len({BASE, PDE_BASE, PPE_BASE, PXE_BASE}) == 4)

# Which descriptor maps an address, and how far a leaf at that level reaches.
LEVELS = [(PXE_BASE, 39, 9, None), (PPE_BASE, 30, 18, 1 << 30),
          (PDE_BASE, 21, 27, 1 << 21), (BASE, 12, 36, 1 << 12)]


def slot(level, va):
    base, shift, bits, _ = LEVELS[level]
    return base + ((va >> shift) & ((1 << bits) - 1)) * 8


# Indexing a level must not spill into the next one's base, which is what a
# missing mask does at the bottom level for a kernel half address.
for va in (0xFFFFF802DC204800, 0xFFFFF6FB7DBED000, 0, 0x7FF8F3B354F0):
    for level in range(4):
        s = slot(level, va)
        check("level %d slot is 8-aligned: 0x%x" % (level, va), s % 8 == 0)
# Dropping the mask is the mistake that makes this arithmetic overflow: the
# page number of a kernel half address shifted into place does not fit in
# sixty-four bits, so the sum wraps into a different region entirely.
kernel = 0xFFFFF802DC204800
check("the masked computation lands in the self-map window",
      BASE <= slot(3, kernel) < BASE + (1 << 39))
check("the unmasked one does not fit at all",
      BASE + ((kernel >> 12) << 3) >= 1 << 64)
# A block at one level and a page at the next cannot both be the answer: the
# descriptor changes level with the address, which is why the level has to be
# read out of the walk rather than assumed from the address.
check("one 2MB region shares its block descriptor",
      slot(2, 0xFFFFF802DC200000) == slot(2, 0xFFFFF802DC3FF000))
check("the next 2MB region has the next one",
      slot(2, 0xFFFFF802DC400000) - slot(2, 0xFFFFF802DC200000) == 8)

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

sys.exit(REPORT.summary(checks, failures))
