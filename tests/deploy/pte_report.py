#!/usr/bin/env python3
"""Say which descriptor translates an address, and where that descriptor is.

The rewrite needs to make a read-only page writable for one store, and the only
thing that can do that is the descriptor that translates the page. Windows maps
the translation tables into the address space, laid out in address order, so
that descriptor is a computation and not a search.

The computation has four forms, one per level, and which one matters is a
property of the mapping rather than of the address: an image is mapped in
blocks, so its page descriptor slot holds nothing, and the descriptor to change
is the block one at the level above. This reads all four slots, says which of
them is the leaf, and then checks the answer against physical memory: the
descriptor's physical address must hold what the virtual address holds.

Nothing here is trusted from the arithmetic alone. The check that the arithmetic
is right is that the physical read and the virtual read agree.

Usage: pte_report.py --va 0x... [--va 0x...] [--kbase 0x...] [--qmp-port 4448]
                     [--serial build/x-run/serial.log] [--stop]
"""

import argparse
import json
import os
import re
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bugcheck_probe                                    # noqa: E402  (its readers)

# The self map. The image's own MiGetPteAddress computes descriptor addresses
# as `0xfffff68000000000 + ((va >> 12) & 0xfffffffff) * 8` from a literal in
# .text, but that literal is the x64 one and this kernel runs a 47-bit address
# space (TCR_EL1.T1SZ is 17), where that address is not canonical and does not
# translate at all. The live base is the variable MmPteBase, which the kernel
# reads wherever it needs a descriptor address; MiGetPteAddress's own literal
# is only ever right on a 48-bit configuration.
SELFMAP_LITERAL = 0xFFFFF68000000000
PAGE_MASK = 0x0000FFFFFFFFF000
# MmPteBase, from the 26100PE public symbols: RVA 0xe06368, section ALMOSTRO.
MM_PTE_BASE_RVA = 0xE06368

# Index field of each level, how many entries it has, and what a leaf there
# covers; the top one first, the same order the kernel's walk uses.
LEVEL_FIELDS = [(39, 9, 1 << 39), (30, 18, 1 << 30),
                (21, 27, 1 << 21), (12, 36, 1 << 12)]


def levels(base):
    """The four levels, top first: where each one's descriptors are, the field
    that indexes them, how many entries there are, and what a leaf covers.

    Each level's descriptors are mapped by the level below, so the base of the
    next one up is the page descriptor of this one: the same computation the
    kernel applies to an address, applied to the base itself.
    """
    bases = [base]
    for _ in range(3):
        bases.append(base + ((bases[-1] & PAGE_MASK) >> 9))
    return [(b, f[0], f[1], f[2])
            for b, f in zip(bases[::-1], LEVEL_FIELDS)]


def slotFor(base, va, shift, bits):
    return base + ((va >> shift) & ((1 << bits) - 1)) * 8


def describe(desc, size):
    if not desc & 1:
        return "invalid"
    kind = desc & 3
    if kind == 3:
        return "table -> %#x" % (desc & PAGE_MASK)
    if kind != 1:
        return "reserved (bits 1:0 = %d)" % kind
    mask = BLOCK_MASKS.get(size)
    if mask is None:
        return "block (impossible at this level)"
    ap = (desc >> 6) & 3
    pxn = (desc >> 53) & 1
    uxn = (desc >> 54) & 1
    return ("block %s pa %#x  AP %d%d (EL1 %s, EL0 %s) AF %d SH %d "
            "PXN %d UXN %d" % (
                "1G" if size == 1 << 30 else "2M", desc & mask,
                (ap >> 1) & 1, ap & 1,
                "RO" if ap & 2 else "RW", "no" if not ap & 1 else "RW",
                (desc >> 10) & 1, (desc >> 8) & 3, pxn, uxn))


def pageFields(desc):
    ap = (desc >> 6) & 3
    return ("page pa %#x  AP %d%d (EL1 %s, EL0 %s) AF %d SH %d nG %d "
            "PXN %d UXN %d" % (
                desc & PAGE_MASK, (ap >> 1) & 1, ap & 1,
                "RO" if ap & 2 else "RW", "no" if not ap & 1 else "RW",
                (desc >> 10) & 1, (desc >> 8) & 3, (desc >> 11) & 1,
                (desc >> 53) & 1, (desc >> 54) & 1))


def physicalRead(qmp_file, addr, count):
    return bugcheck_probe.readWords(qmp_file, addr, count, physical=True)


def report(qmp_file, va, check, levelList):
    print("va = %#x" % va)
    leaf = None
    for (name, size), (base, shift, bits, _) in zip(NAMES_SIZES, levelList):
        at = slotFor(base, va, shift, bits)
        word = bugcheck_probe.readWords(qmp_file, at, 1)
        if not word:
            print("  %-3s %#018x : unreadable" % (name, at))
            continue
        desc = word[0]
        text = ("%s" % pageFields(desc)) if size == 1 << 12 and desc & 3 == 3 \
            else describe(desc, size)
        print("  %-3s %#018x : %#018x  %s" % (name, at, desc, text))
        if leaf is None and desc & 1:
            if (desc & 3) == 1 or (size == 1 << 12 and (desc & 3) == 3):
                leaf = (name, at, desc, size)
    if leaf is None:
        print("  no valid descriptor in the chain")
        return 1
    name, at, desc, size = leaf
    if size == 1 << 12:
        pa = (desc & PAGE_MASK) + (va & 0xFFF)
    else:
        pa = (desc & BLOCK_MASKS[size]) + (va & (size - 1))
    print("  leaf: %s at %#x, covers %#x bytes, pa %#x" % (name, at, size, pa))
    if not check:
        return 0
    virt = bugcheck_probe.readWords(qmp_file, va, 1)
    phys = physicalRead(qmp_file, pa, 1)
    if not virt or not phys:
        print("  cross-check: could not read both sides (virt %s, phys %s)"
              % (virt, phys))
        return 1
    same = virt[0] == phys[0]
    print("  cross-check: virt %#018x  phys %#018x  %s"
          % (virt[0], phys[0], "equal" if same else "DIFFERENT"))
    return 0 if same else 1


NAMES_SIZES = list(zip(["pxe", "ppe", "pde", "pte"],
                       [1 << 39, 1 << 30, 1 << 21, 1 << 12]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--va", type=lambda s: int(s, 0), action="append", default=[],
                    help="an address to report on; may be repeated")
    ap.add_argument("--kbase", type=lambda s: int(s, 0),
                    help="the kernel's base, when it is already known")
    ap.add_argument("--rva", type=lambda s: int(s, 0), action="append", default=[],
                    help="kernel-relative offsets to add to kbase; may be repeated")
    ap.add_argument("--selfmap-base", type=lambda s: int(s, 0),
                    help="use this descriptor base instead of the image's literal")
    ap.add_argument("--serial", help="a serial log to take the payload's address from")
    ap.add_argument("--qmp-port", type=int, default=4448)
    ap.add_argument("--no-check", action="store_true",
                    help="do not compare the physical and virtual reads")
    ap.add_argument("--stop", action="store_true",
                    help="stop the processors for the duration of the reads")
    args = ap.parse_args()

    qmp = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20)
    qmpFile = qmp.makefile("rwb")
    qmpFile.readline()
    bugcheck_probe.cmd(qmpFile, {"execute": "qmp_capabilities"})
    if args.stop:
        bugcheck_probe.cmd(qmpFile, {"execute": "stop"})

    pc = bugcheck_probe.programCounter(qmpFile)
    kbase = args.kbase
    if kbase is None and pc is not None:
        kbase = bugcheck_probe.findKernelBase(qmpFile, pc)
    print("pc = %#x" % (pc or 0))
    if kbase is not None:
        print("kbase = %#x" % kbase)
        if pc is not None and pc >= kbase:
            print("  (pc is kbase+%#x)" % (pc - kbase))

    vas = list(args.va)
    if kbase is not None:
        vas += [kbase + rva for rva in args.rva]

    # Where the descriptors are is a property of the running kernel, not of the
    # image: the image's own literal is only valid on a 48-bit configuration,
    # and this kernel's variable is what its code actually dereferences. Reading
    # it needs the kernel's base, so a report that could not find one falls back
    # to the literal and says so.
    base = args.selfmap_base
    if base is None and kbase is not None:
        words = bugcheck_probe.readWords(qmpFile, kbase + MM_PTE_BASE_RVA, 1)
        if words and words[0] >> 48 == 0xFFFF:
            base = words[0]
            print("MmPteBase (kbase+%#x) = %#x" % (MM_PTE_BASE_RVA, base))
        else:
            print("MmPteBase (kbase+%#x) is %s; using the image's literal"
                  % (MM_PTE_BASE_RVA,
                     ("%#x" % words[0]) if words else "unreadable"))
    if base is None:
        base = SELFMAP_LITERAL
        print("descriptor base = %#x (the image's literal)" % base)
    slots = levels(base)
    for (name, _), (lbase, _, _, _) in zip(NAMES_SIZES, slots):
        print("  %s base = %#x" % (name, lbase))
    if args.serial:
        text = open(args.serial, "rb").read().decode("latin1")
        m = re.search(r"vamap: record=0x([0-9a-f]+)", text)
        if m:
            words = physicalRead(qmpFile, int(m.group(1), 16), 6)
            if len(words) == 6 and words[5]:
                print("payload's own address = %#x" % words[5])
                vas.append(words[5])
    if not vas:
        print("nothing to report on: give --va, or a --kbase with --rva")
        return 1

    rc = 0
    for va in vas:
        rc |= report(qmpFile, va, not args.no_check, slots)
    return rc


if __name__ == "__main__":
    sys.exit(main())
