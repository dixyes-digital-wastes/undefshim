#!/usr/bin/env python3
"""First-fault and mapping report for a machine kept at its halt.

The machine is left running by bugcheck_probe.py --keep. Everything the
investigation had was read from the pool: what the payload saw. What was
never read is what the kernel saw when it decided to die, and whether the
pages the payload lives on were still mapped when it did.

Reads, in order:

  - the live system registers, including TTBR0/TTBR1/VBAR
  - KiBugCheckData, directly at its known offset, rather than by the shape
    search that has been reporting "no bugcheck" on recent runs
  - a 4KB-granule walk of both translation regimes for the payload and the
    pool: mapped or not, and at what attributes
  - the transfer record out of the payload blob, which says where the pool
    was expected to be once the kernel's tables took over

Usage: death_report.py [--qmp-port 4447]
"""

import argparse
import json
import re
import socket
import sys

# From the symbols file and the structure definitions; see status.md for how
# these were checked.
KI_BUGCHECK_DATA_RVA = 0xdba5e0
VECTOR_TABLE_RVA = 0x604800

# Offsets inside the payload blob, from build/payload/payload_blob.h.
BLOB_BASE = 0x13BC00000
BLOB_TRANSFER_OFFSET = 3696

# The regions whose mapping decides whether the payload can run at all.
REGIONS = [
    ("payload code", 0x13BC00000),
    ("payload entry SP0", 0x13BC01430),
    ("payload entry SPx", 0x13BC01240),
    ("pool base", 0x1380E0000),
    ("pool stack 0 top", 0x1380E6000),
]


def cmd(f, obj):
    f.write((json.dumps(obj) + "\n").encode())
    f.flush()
    while True:
        line = f.readline()
        if not line:
            raise SystemExit("the monitor closed the connection")
        r = json.loads(line)
        if "return" in r or "error" in r:
            return r


def hmp(f, line):
    out = cmd(f, {"execute": "human-monitor-command",
                  "arguments": {"command-line": line}})
    return out.get("return", "")


def words(f, addr, count, physical=False):
    """Read doublewords; virtual by default, physical on request.

    A read that fails returns nothing, which for the virtual case is itself
    an answer: the page is not mapped.
    """
    unit = "xp" if physical else "x"
    text = hmp(f, "%s /%dgx 0x%x" % (unit, count, addr))
    values = []
    for line in text.splitlines():
        if ":" not in line:
            continue
        row = [int(v, 16) for v in
               re.findall(r"0x([0-9a-f]+)", line.split(":", 1)[1])]
        values += row
    return values[:count]


def systemRegs(f):
    text = hmp(f, "info registers")
    wanted = ("pc ", "TTBR0_EL1", "TTBR1_EL1", "VBAR_EL1", "SCTLR_EL1",
              "TCR_EL1", "ESR_EL1", "ELR_EL1", "FAR_EL1", "SP", "X16",
              "X17", "X18")
    out = {}
    for line in text.splitlines():
        for w in wanted:
            if w in line and w.rstrip() not in out:
                m = re.search(re.escape(w.rstrip()) + r"[=:]?\s*(0x[0-9a-f]+)",
                              line)
                if m:
                    out[w.rstrip()] = int(m.group(1), 16)
    return out, text


def findKernelBase(f, pc):
    """kbase from the vector table's shape; the same idea as the probe's."""
    prologue = 0xD5384112   # mrs x18, sp_el0

    def word(addr):
        v = words(f, addr, 1)
        return (v[0] & 0xFFFFFFFF) if v else None

    top = pc - (pc % 0x200000)
    for i in range(16):
        base = top - i * 0x200000
        heads = [word(base + VECTOR_TABLE_RVA + slot * 0x80)
                 for slot in range(4)]
        if any(h is None for h in heads):
            continue
        if any(h != prologue for h in heads[1:]):
            continue
        if heads[0] != prologue and (heads[0] & 0xFC000000) != 0x14000000:
            continue
        return base
    return None


def walk(f, ttbr, va):
    """One 4KB-granule, 48-bit walk. Returns None or (pa, attrs, level).

    Attrs are the descriptor's upper attribute bits, reported raw: the
    interesting question is mapped-or-not, and after that executable,
    writable, and user-vs-kernel, all of which are readable from the raw
    word by eye.
    """
    pa = ttbr & 0x000FFFFFFFFFF000
    if pa == 0:
        return None
    for level in range(4):
        shift = 39 - 9 * level
        idx = (va >> shift) & 0x1FF
        desc_addr = pa + idx * 8
        d = words(f, desc_addr, 1, physical=True)
        if not d:
            return ("table read failed at 0x%x" % desc_addr,)
        d = d[0]
        if d & 1 == 0:
            return None
        if d & 2 == 0:
            # A block. Only valid below level 0, but report whatever is there.
            return ((d & 0x000FFFFFFFFFF000) | (va & ((1 << shift) - 1)),
                    d >> 8, level)
        if level == 3:
            return ((d & 0x000FFFFFFFFFF000) | (va & 0xFFF), d >> 8, 3)
        pa = d & 0x000FFFFFFFFFF000
    return None


def transferRecord(f):
    w = words(f, BLOB_BASE + BLOB_TRANSFER_OFFSET, 12, physical=True)
    if not w or w[0] != 0x00534E4152545355:   # "USTRANS"
        return w[:1] if w else None
    names = ["magic", "poolPa", "poolVaBefore", "poolVaAfter", "kernelEntry",
             "mappedSize", "probes/mapped", "exhausted/faulted",
             "foundKernel/reserved", "kernelBase", "kernelSize", "loaderBlock"]
    out = {}
    for i, n in enumerate(names):
        if i in (6, 7, 8):
            lo = w[i] & 0xFFFFFFFF
            hi = (w[i] >> 32) & 0xFFFFFFFF
            out[names[i].split("/")[0]] = lo
            out[names[i].split("/")[1]] = hi
        else:
            out[n] = w[i]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp-port", type=int, default=4447)
    ap.add_argument("--serial", help="serial log from this run")
    ap.add_argument("--payload-header", help="payload_blob.h from the deployed build")
    ap.add_argument("--layout-defs", help="layout_defs.inc from the deployed build")
    args = ap.parse_args()
    supplied = (args.serial, args.payload_header, args.layout_defs)
    if any(supplied) and not all(supplied):
        ap.error("--serial, --payload-header and --layout-defs must be supplied together")
    constants = {}
    if all(supplied):
        global BLOB_BASE, BLOB_TRANSFER_OFFSET, REGIONS
        with open(args.serial, encoding="latin1") as src:
            serial = src.read()
        match = re.search(r"payload: at (0x[0-9a-f]+)", serial)
        if match is None:
            ap.error("this serial log has no payload placement")
        BLOB_BASE = int(match[1], 16)
        for path in (args.payload_header, args.layout_defs):
            with open(path) as src:
                for line in src:
                    match = re.fullmatch(r"#define (US_\w+) (0x[0-9a-fA-F]+|[0-9]+)\s*", line)
                    if match:
                        constants[match[1]] = int(match[2], 0)
        BLOB_TRANSFER_OFFSET = constants["US_PAYLOAD_TRANSFER_OFFSET"]
        REGIONS = [
            ("payload code PA alias", BLOB_BASE),
            ("payload entry PA alias", BLOB_BASE + constants["US_PAYLOAD_ENTRY_OFFSET"]),
        ]
    else:
        print("WARNING: legacy hardcoded regions/offsets; not evidence for a new build")

    s = socket.create_connection(("127.0.0.1", args.qmp_port), timeout=20)
    f = s.makefile("rwb")
    f.readline()
    cmd(f, {"execute": "qmp_capabilities"})

    regs, raw = systemRegs(f)
    print("== live registers ==")
    for k in ("pc", "SP", "TTBR0_EL1", "TTBR1_EL1", "VBAR_EL1", "SCTLR_EL1",
              "TCR_EL1", "ESR_EL1", "ELR_EL1", "FAR_EL1", "X16", "X17", "X18"):
        if k in regs:
            print("  %-10s 0x%x" % (k, regs[k]))

    pc = regs.get("pc")
    kbase = findKernelBase(f, pc) if pc else None
    print("\n== kernel ==")
    print("  kbase = %s" % ("0x%x" % kbase if kbase else "not found"))
    if kbase:
        bc = words(f, kbase + KI_BUGCHECK_DATA_RVA, 5)
        print("  KiBugCheckData:")
        if bc:
            names = ["code", "arg1", "arg2", "arg3", "arg4"]
            for n, v in zip(names, bc):
                extra = ""
                if kbase <= v < kbase + 0x10000000:
                    extra = "  (kbase+0x%x)" % (v - kbase)
                print("    %-5s 0x%016x%s" % (n, v, extra))
        else:
            print("    unreadable")

    if constants:
        print("\n== UEFI conversion record (physical) ==")
        record = words(f, BLOB_BASE + constants["US_PAYLOAD_VAMAP_OFFSET"], 15, physical=True)
        for name, value in zip(("magic", "fired", "poolBefore", "poolAfter",
                                "payloadBefore", "payloadAfter", "poolStatus", "payloadStatus",
                                "hookFired", "hookMapSize", "hookDescs", "hookStatus",
                                "svmOriginal", "rt", "convertPointer"), record):
            print("  %-14s 0x%x" % (name, value))
        print("\n== payload configuration and publication (physical) ==")
        config = words(f, BLOB_BASE + constants["US_PAYLOAD_CONFIG_OFFSET"],
                       constants["US_CONFIG_SIZE"] // 8, physical=True)
        if len(config) != constants["US_CONFIG_SIZE"] // 8:
            raise SystemExit("could not read this build's configuration")
        def field(name, offset=0, width=8):
            at = constants[name] + offset
            value = config[at // 8] >> ((at % 8) * 8)
            return value & ((1 << (width * 8)) - 1)
        for name in ("US_CONFIG_SELF_VA", "US_CONFIG_POOL", "US_CONFIG_HIGH_VA",
                     "US_CONFIG_HIGH_POOL_VA", "US_CONFIG_STUB_COUNT"):
            print("  %-24s 0x%x" % (name, field(name)))
        highVa = field("US_CONFIG_HIGH_VA")
        poolVa = field("US_CONFIG_POOL")
        if record and len(record) >= 4:
            REGIONS.append(("pool PA alias", record[2]))
        if highVa:
            REGIONS.extend((("payload high code", highVa),
                            ("payload high entry", highVa + constants["US_PAYLOAD_ENTRY_OFFSET"]),
                            ("pool high VA", poolVa),
                            ("stack0 high top-16", field("US_CONFIG_STACK_TOP") - 16)))
        count = field("US_CONFIG_STUB_COUNT")
        if count > 32:
            raise SystemExit("invalid stub count: %d" % count)
        for i in range(count):
            offset = constants["US_CONFIG_STUBS"] + i * constants["US_STUB_STRIDE"]
            print("  stub %d:" % i)
            for name in ("US_STUB_ADDRESS", "US_STUB_TABLE", "US_STUB_IMAGE",
                         "US_STUB_TABLE_PA", "US_STUB_ADDRESS_PA",
                         "US_STUB_TARGET", "US_STUB_PUBLISHED"):
                width = 4 if name in ("US_STUB_TARGET", "US_STUB_PUBLISHED") else 8
                print("    %-20s 0x%x" % (name, field(name, offset, width)))
            code = words(f, field("US_STUB_ADDRESS_PA", offset), 12, physical=True)
            print("    code (PA) = %s" % " ".join("%016x" % value for value in code))

    print("\n== mapping of our regions ==")
    for name, ttbr in (("TTBR0", regs.get("TTBR0_EL1")),
                       ("TTBR1", regs.get("TTBR1_EL1"))):
        print("  via %s (0x%x):" % (name, ttbr or 0))
        if not ttbr:
            print("    no table")
            continue
        for label, va in REGIONS:
            r = walk(f, ttbr, va)
            if r is None:
                print("    %-20s 0x%x  NOT MAPPED" % (label, va))
            elif len(r) == 1:
                print("    %-20s 0x%x  %s" % (label, va, r[0]))
            else:
                print("    %-20s 0x%x -> pa 0x%x  attrs 0x%x  L%d"
                      % (label, va, r[0], r[1], r[2]))

    print("\n== transfer record (from the blob, physical) ==")
    t = transferRecord(f)
    if t is None:
        print("  unreadable")
    elif isinstance(t, list):
        print("  magic wrong: %s" % ([hex(v) for v in t]))
    else:
        for k, v in t.items():
            print("  %-14s 0x%x" % (k, v))

    print("\n== raw info registers (first 40 lines) ==")
    for line in raw.splitlines()[:40]:
        print("  " + line)


if __name__ == "__main__":
    main()
