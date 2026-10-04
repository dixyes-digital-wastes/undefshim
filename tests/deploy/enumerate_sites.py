#!/usr/bin/env python3
"""Write a patch list for every RCpc load in the kernel's text, with the
symbol each one lives in.

The enumeration is disassembly, not pattern matching: each function in the
exception directory is decoded from its own entry point, so a byte sequence
in the middle of a function that happens to look like a load is not mistaken
for one, and nothing in the image's data is read as code at all. The list it
writes carries the digest the driver prints for this image -- the text with
the loader's relocation targets left out, which is why the two agree -- and a
comment per site naming the symbol it is in, so a list can be read.

Run pdb_publics.py first if symbols are wanted; without them the comments
carry the function's own address instead.
"""

import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys

try:
    import capstone
except ImportError:
    sys.exit("capstone is needed: pip install capstone (or use the system one)")

LDAPR_MASK = 0x3FFFFC00
LDAPR_FIXED = 0x38BFC000
LDAR_BASE = 0x08DFFC00
MAX_WIDTH = 4


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        sys.exit("not a PE image")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optSize = struct.unpack_from("<H", data, pe + 20)[0]
    out = {}
    for index in range(count):
        at = pe + 24 + optSize + index * 40
        name = data[at:at + 8].rstrip(b"\0").decode("ascii", "replace")
        virtualSize, virtualAddress, rawSize, rawOffset = struct.unpack_from(
            "<IIII", data, at + 8)
        out[name] = {"rva": virtualAddress, "vsize": virtualSize,
                     "raw": rawOffset, "rawSize": rawSize}
    opt = pe + 24
    directoriesAt = opt + 112
    directories = struct.unpack_from("<I", data, opt + 108)[0]
    dirs = []
    for index in range(min(directories, 16)):
        dirs.append(struct.unpack_from("<II", data, directoriesAt + index * 8))
    return out, dirs


def functions(data, sectionsByName, directories, textRva, textBytes):
    """(start, end) for every function in the exception directory."""
    reloc = directories[3]
    if reloc[0] == 0:
        return []
    name, section = next(((n, s) for n, s in sectionsByName.items()
                          if s["rva"] <= reloc[0] < s["rva"] + max(s["vsize"], s["rawSize"])),
                         (None, None))
    if section is None:
        return []
    at = section["raw"] + (reloc[0] - section["rva"])
    # An AArch64 .pdata entry is (begin, unwind data), not (begin, end): the
    # second word points at the unwind information, or packs it, and reading it
    # as an end address throws most of the functions away. What is reliable is
    # that the entries are sorted, so a function runs to the next one's start.
    starts = []
    for index in range(reloc[1] // 8):
        begin = struct.unpack_from("<I", data, at + index * 8)[0]
        if begin == 0 or not (textRva <= begin < textRva + textBytes):
            continue
        starts.append(begin)
    starts.sort()
    return [(starts[i], starts[i + 1] if i + 1 < len(starts) else textRva + textBytes)
            for i in range(len(starts))]


def loadSymbols(path):
    symbols = []
    if path and os.path.exists(path):
        for line in open(path, "r", encoding="utf-8", errors="replace"):
            parts = line.split("\t")
            if len(parts) >= 3 and parts[0].startswith("0x") and parts[1].strip() == ".text":
                try:
                    symbols.append((int(parts[0], 16), parts[2].strip()))
                except ValueError:
                    pass
    symbols.sort()
    return symbols


def symbolFor(symbols, rva, functionsList):
    name = None
    for start, candidate in symbols:
        if start > rva:
            break
        name = candidate
    if name is None:
        name = "sub_%x" % rva
    return name


def digestFor(textRaw, textRva, textBytes, sectionsByName, directories, data):
    """The digest the driver prints: the text with relocation targets left out."""
    skips = []
    reloc = directories[5]
    if reloc[0] != 0:
        name, section = next(((n, s) for n, s in sectionsByName.items()
                              if s["rva"] <= reloc[0] < s["rva"] + max(s["vsize"], s["rawSize"])),
                             (None, None))
        if section is not None:
            at = section["raw"] + (reloc[0] - section["rva"])
            size = reloc[1]
            pos = 0
            while pos + 8 <= size:
                page, blockSize = struct.unpack_from("<II", data, at + pos)
                if blockSize < 8 or pos + blockSize > size:
                    break
                for entry in range(pos + 8, pos + blockSize, 2):
                    value = struct.unpack_from("<H", data, at + entry)[0]
                    rva = page + (value & 0xFFF)
                    if textRva <= rva < textRva + textBytes:
                        skips.append(rva - textRva)
                pos += blockSize
    skips.sort()
    digest = hashlib.sha256()
    cursor = 0
    for start in skips:
        end = min(start + 8, textBytes)
        if start > cursor:
            digest.update(textRaw[cursor:start])
        cursor = max(cursor, end)
    if textBytes > cursor:
        digest.update(textRaw[cursor:textBytes])
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", default="/home/dixyes/92024h2/windowsthings/26100pe/ntoskrnl.exe")
    parser.add_argument("--symbols", default="build/symbols.tsv",
                        help="output of analysis/scripts/pdb_publics.py")
    parser.add_argument("--pefile", default="ntoskrnl")
    parser.add_argument("--out", default="config/usPatch/all-ldapr.txt")
    parser.add_argument("--limit", type=int, default=0, help="write at most this many")
    parser.add_argument("--digest-from", default="",
                        help="a run log whose driver-printed digest to stamp the list with: "
                             "the loader patches a little of the text itself, so the file's "
                             "digest and the machine's are not always the same value")
    args = parser.parse_args()

    data = open(args.kernel, "rb").read()
    byName, directories = sections(args.kernel and data)
    text = byName[".text"]
    textRva, textBytes = text["rva"], text["rawSize"]
    textRaw = data[text["raw"]:text["raw"] + textBytes]
    symbols = loadSymbols(args.symbols)
    digest = digestFor(textRaw, textRva, textBytes, byName, directories, data)
    if args.digest_from:
        found = re.search(r"patch: text sha256 ([0-9a-f]{64})",
                          open(args.digest_from, "rb").read().decode("latin1", "replace"))
        if found:
            print("using the digest the machine printed: %s" % found.group(1))
            digest = found.group(1)

    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True

    # Decode from each function's entry point and follow what it can reach,
    # rather than sweeping to the next function: a literal pool or a jump
    # table inside a function decodes as something, and a sweep that walks
    # into one never comes back out, so everything after it is misread. The
    # worklist is the addresses to decode from; an address already decoded is
    # not decoded twice.
    byAddress = {}
    for begin, end in functions(data, byName, directories, textRva, textBytes):
        byAddress[begin] = end
    sites = []
    decoded = set()
    worklist = sorted(byAddress)
    limit = textRva + textBytes
    while worklist:
        pc = worklist.pop()
        while textRva <= pc < limit and pc not in decoded:
            decoded.add(pc)
            start = text["raw"] + (pc - textRva)
            insn = next(md.disasm(data[start:start + 4], pc), None)
            if insn is None:
                break
            word = (insn.bytes[0] | (insn.bytes[1] << 8) | (insn.bytes[2] << 16)
                    | (insn.bytes[3] << 24))
            if (word & LDAPR_MASK) == LDAPR_FIXED and insn.mnemonic.startswith("ldapr"):
                sites.append((pc, insn.bytes, insn.mnemonic))
            mnemonic = insn.mnemonic
            if mnemonic in ("ret", "br", "eret", "udf", "b"):
                # b is unconditional: it ends the run, and its target starts one.
                pass
            if mnemonic in ("b", "bl") or mnemonic.startswith("b."):
                try:
                    target = insn.operands[0].imm
                except (IndexError, AttributeError):
                    target = None
                if target is not None and textRva <= target < limit:
                    worklist.append(target)
            if mnemonic in ("ret", "br", "eret", "udf"):
                break
            if mnemonic == "b":
                break
            pc += 4
    sites.sort()
    print("%d sites in %d bytes of text, digest %s" % (len(sites), textBytes, digest))

    lines = ["# Every RCpc load in this kernel's text, found by disassembling each",
             "# function in the exception directory rather than by scanning for a",
             "# pattern. One site per line, with the symbol it lives in.",
             "USPATCHV1",
             "peFile " + args.pefile,
             "textSHA256Hash " + digest,
             ""]
    for rva, raw, mnemonic in sites[:args.limit or None]:
        word = raw[0] | (raw[1] << 8) | (raw[2] << 16) | (raw[3] << 24)
        ld = (word & 0xC0000000) | LDAR_BASE | (word & 0x1FE0) | (word & 0x1F)
        lines.append("0x%x %08x %08x  # %s: %s" % (rva, word, ld, symbolFor(symbols, rva, None), mnemonic))
    with open(args.out, "w", encoding="ascii") as handle:
        handle.write("\n".join(lines) + "\n")
    print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
