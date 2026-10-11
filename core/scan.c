/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Finding things inside an image, see scan.h for the contract
 */

// us-build: driver

#include "core/scan.h"

/* ---------------------------------------------------------------------------
 * The signatures
 *
 * All of these are instruction encodings rather than text, so they are
 * written as the instruction word and laid out little endian by the macro.
 * Writing them as loose bytes invites getting the order wrong, which is not
 * the kind of mistake a scan will tell you about: it just finds nothing
 *
 * None of them is allowed to be a heuristic. A locator that can match twice
 * is a locator that can pick the wrong place, so the callers insist on
 * exactly one
 * ------------------------------------------------------------------------- */

#define US_LE32(w)                          \
    (uint8_t)((w) & 0xFF),                  \
    (uint8_t)(((w) >> 8) & 0xFF),           \
    (uint8_t)(((w) >> 16) & 0xFF),          \
    (uint8_t)(((w) >> 24) & 0xFF)

/* msr VBAR_EL1, xN. N is free, hence the wildcard in the low byte */
static const uint8_t kMsrVBAREL1[] = { US_LE32(0xD518C000) };
static const uint8_t kMsrVBAREL1Mask[] = { US_LE32(0xFFFFFFE0) };

const UsPattern usPatMsrVBAREL1 = {
    .bytes = kMsrVBAREL1,
    .mask = kMsrVBAREL1Mask,
    .len = 4,
};

/*
 * The transfer to the kernel
 *
 * This is the sequence that switches SP to the kernel's own stack and then
 * branches to the kernel's entry point. It is the point at which everything
 * before it is finished and nothing of the kernel has run yet, which is the
 * only moment the kernel image can be edited without disturbing it
 *
 *   mov sp, x2          the kernel's stack, loaded two instructions earlier
 *   mov x19, x0         whatever the kernel entry wants
 *   mov x20, x1         the kernel entry point
 *   bl  <anywhere>      one more call before handing over
 *   mov x0, x19
 *   br  x20             the handover itself
 *
 * The call in the middle has a different target in every version, so it is
 * masked out. The rest is fixed, and the last instruction is what carries the
 * meaning: a branch through a register that was loaded with the entry point
 */
static const uint8_t kTransferLeaf[] = {
    US_LE32(0x9100005F), US_LE32(0xAA0003F3), US_LE32(0xAA0103F4), US_LE32(0x94000000),
    US_LE32(0xAA1303E0), US_LE32(0xD61F0280),
};
static const uint8_t kTransferLeafMask[] = {
    US_LE32(0xFFFFFFFF), US_LE32(0xFFFFFFFF), US_LE32(0xFFFFFFFF), US_LE32(0xFC000000),
    US_LE32(0xFFFFFFFF), US_LE32(0xFFFFFFFF),
};

const UsPattern usPatTransferLeaf = {
    .bytes = kTransferLeaf,
    .mask = kTransferLeafMask,
    .len = sizeof(kTransferLeaf),
};

/*
 * The LDAPR family. The size and the opcode live in the top two bytes and are
 * fixed; the two register fields are free, which the mask carries
 */
static const uint8_t kLDAPRMask[] = { US_LE32(0xFFFFFC00) };

static const uint8_t kLDAPRWBytes[] = { US_LE32(0xB8BFC000) };
static const uint8_t kLDAPRXBytes[] = { US_LE32(0xF8BFC000) };
static const uint8_t kLDAPRBBytes[] = { US_LE32(0x38BFC000) };
static const uint8_t kLDAPRHBytes[] = { US_LE32(0x78BFC000) };

const UsPattern usPatLDAPRW = { .bytes = kLDAPRWBytes, .mask = kLDAPRMask, .len = 4 };
const UsPattern usPatLDAPRX = { .bytes = kLDAPRXBytes, .mask = kLDAPRMask, .len = 4 };
const UsPattern usPatLDAPRB = { .bytes = kLDAPRBBytes, .mask = kLDAPRMask, .len = 4 };
const UsPattern usPatLDAPRH = { .bytes = kLDAPRHBytes, .mask = kLDAPRMask, .len = 4 };

/* ------------------------------------------------------------------------ */

bool patMatchAt(const uint8_t *at, const UsPattern *pat) {
    for (size_t i = 0; i < pat->len; i++) {
        uint8_t mask = pat->mask != NULL ? pat->mask[i] : 0xFFU;
        if ((at[i] & mask) != (pat->bytes[i] & mask)) {
            return false;
        }
    }
    return true;
}

void usScanRegion(const uint8_t *buf, size_t len, uint32_t rvaAt,
                  const UsPattern *pat, UsMatchList *out) {
    out->count = 0;
    out->total = 0;

    if (buf == NULL || pat == NULL || pat->len == 0 || len < pat->len) {
        return;
    }

    size_t limit = len - pat->len;
    for (size_t i = 0; i <= limit; i++) {
        if (!patMatchAt(buf + i, pat)) {
            continue;
        }
        out->total++;
        if (out->count < US_SCAN_MAX_MATCHES) {
            out->matches[out->count].offset = i;
            out->matches[out->count].rva = rvaAt + (uint32_t)i;
            out->count++;
        }
    }
}

UsMatchList usScanImage(UsImage *img, const UsPattern *pat) {
    UsMatchList all = { 0 };

    if (img == NULL || !img->valid) {
        return all;
    }

    /*
     * Only executable sections. A pattern that also matches data is not a
     * signature, and restricting the search is what turns a byte string into
     * one: LDAPR encodings occur in jump tables and literals, and instructions
     * that load a page table register occur in dead code that the linker kept
     */
    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPESection *s = &img->sections[i];
        const uint8_t *p;
        size_t avail = 0;

        if ((s->characteristics & US_PE_SECTION_EXECUTABLE) == 0) {
            continue;
        }
        if (s->virtualSize == 0) {
            continue;
        }
        p = usImageRVASpan(img, s->virtualAddress, &avail);
        if (p == NULL) {
            continue;
        }
        if (avail > s->virtualSize) {
            avail = s->virtualSize;
        }

        UsMatchList one;
        usScanRegion(p, avail, s->virtualAddress, pat, &one);
        all.total += one.total;
        for (size_t k = 0; k < one.count && all.count < US_SCAN_MAX_MATCHES; k++) {
            all.matches[all.count++] = one.matches[k];
        }
    }

    return all;
}
