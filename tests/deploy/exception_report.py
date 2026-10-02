#!/usr/bin/env python3
"""Read QEMU's exception log and say where each exception was delivered.

`-d int` records every exception with the PC it was taken to, which is the
route a fault travelled. That is the only place the route is written down:
the guest cannot print once the kernel is running, and a fault that is
delivered to the wrong table looks exactly like a fault that was never taken.

Two things are worked out from the log rather than given:

  - the kernel's base. Delivery PCs inside the kernel are a fixed offset into
    its vector table, so the base falls out of any one of them
  - which slot a delivery belongs to. A vector table entry is 0x80 bytes, and
    the offset within it says the exception kind and the stack pointer that
    was selected. That is what tells "our stub was entered" from "the kernel's
    own handler ran instead"

Usage: exception_report.py <serial.log> <int.log> [--last N]
"""

import collections
import re
import sys

# The kernel's vector table, as an offset into the kernel image. The table has
# sixteen entries of 0x80 bytes; each quarter of it is one exception kind.
VECTOR_TABLE_RVA = 0x604800
VECTOR_ENTRY = 0x80

# The entry within the table, by the two fields the hardware uses to choose
# one: the stack pointer in use and the kind.
QUARTERS = ["EL1t", "EL1h", "EL0a", "EL0b"]
KINDS = ["Sync", "IRQ", "FIQ", "SError"]


def parseRecords(raw):
    """Every exception, in order, with what the log says about it."""
    out = []
    for block in re.split(r"(?=Taking exception )", raw):
        m = re.match(r"Taking exception (\d+) \[([^\]]+)\] on CPU (\d+)", block)
        if m is None:
            continue

        def field(pattern):
            mm = re.search(pattern, block)
            return int(mm.group(1), 16) if mm else None

        out.append({
            "kind": m.group(2),
            "cpu": int(m.group(3)),
            "pcLevel": (re.search(r"to (EL\d) PC", block) or [None, "?"])[1]
            if re.search(r"to (EL\d) PC", block) else "?",
            "pc": field(r"to EL\d PC 0x([0-9a-f]+)"),
            "elr": field(r"with ELR 0x([0-9a-f]+)"),
            "far": field(r"with FAR 0x([0-9a-f]+)"),
            "esr": field(r"with ESR [0-9a-f]+/0x([0-9a-f]+)"),
        })
    return out


def kernelBase(records):
    """The kernel's base, from any delivery that landed in its vector table.

    Only the low twelve bits are taken from the record, because those are the
    part that says which table and which entry it was; the rest of the address
    is the base. A delivery to the loader or to the payload does not have that
    shape and is skipped.
    """
    for r in records:
        pc = r["pc"]
        if pc is None or pc < 0xFFFF000000000000:
            continue
        for rva in (VECTOR_TABLE_RVA, VECTOR_TABLE_RVA + 0x200):
            if (pc - rva) & 0xFFF == 0:
                return pc - rva
    return None


def describe(pc, base):
    """What a delivery PC is, in terms that name a table and an entry."""
    if pc is None:
        return "?"
    if base is not None:
        off = pc - base
        if 0 <= off < 0x10000 and off % VECTOR_ENTRY == 0:
            entry = off // VECTOR_ENTRY
            if entry < 16:
                quarter = QUARTERS[entry // 4]
                kind = KINDS[entry % 4]
                # The first entry of each quarter is offset 0 within it, which
                # is where an exception lands when no change of stack pointer
                # happened on the way in.
                return "kernel %s-%s (kbase+0x%x)" % (quarter, kind, off)
    return "0x%x" % pc


def main():
    serialPath, intPath = sys.argv[1], sys.argv[2]
    last = 8
    if "--last" in sys.argv:
        last = int(sys.argv[sys.argv.index("--last") + 1])

    raw = open(intPath, "rb").read().decode("latin1", "replace")
    serial = open(serialPath, "rb").read().decode("latin1", "replace")
    records = parseRecords(raw)

    base = kernelBase(records)
    print("exceptions: %d" % len(records))
    print("kinds: %s" % dict(collections.Counter(r["kind"] for r in records)))

    # The loader's base comes from the driver's own log, which is the only
    # place it is written down.
    loader = None
    for pat in (r"winload base 0x([0-9a-f]+)", r"gmm: winload found at 0x([0-9a-f]+)"):
        m = re.search(pat, serial)
        if m:
            loader = int(m.group(1), 16)
            break
    print("kernel base: %s   loader base: %s"
          % (hex(base) if base else "?", hex(loader) if loader else "?"))

    sync = [r for r in records if r["kind"] != "IRQ" and r["pc"] is not None]
    print("--- where synchronous exceptions were delivered ---")
    for pc, n in collections.Counter(r["pc"] for r in sync).most_common(8):
        print("  %-34s %5d" % (describe(pc, base), n))

    if base is not None:
        print("--- what was running when they were taken ---")
        for elr, n in collections.Counter(r["elr"] for r in sync if r["elr"]).most_common(6):
            off = elr - base
            where = "kbase+0x%x" % off if 0 <= off < 0x2000000 else hex(elr)
            print("  %-24s %5d" % (where, n))

    print("--- last %d synchronous, in order ---" % last)
    for r in sync[-last:]:
        print("  %-18s cpu=%d elr=%-18s pc=%s far=%s"
              % (r["kind"], r["cpu"],
                 ("kbase+0x%x" % (r["elr"] - base))
                 if (base is not None and r["elr"] and 0 <= r["elr"] - base < 0x2000000)
                 else (hex(r["elr"]) if r["elr"] else "-"),
                 describe(r["pc"], base),
                 hex(r["far"]) if r["far"] else "-"))


if __name__ == "__main__":
    main()
