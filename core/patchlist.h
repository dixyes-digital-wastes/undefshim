/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The patch list files the driver applies to an image before it runs
 *
 * One file describes one target:
 *
 *     USPATCHV1
 *     peFile ntoskrnl                 # optional: the image's name
 *     textSHA256Hash 3f2a...c9        # optional: the text's digest
 *     pdbUUID 9a0b1c2d-3e4f-5061-7283-94a5b6c7d8e9-1   # optional: the build
 *     0x458b18 f8bfc3ea c8dfffea             # rva match replace
 *     0x458b24 38bfc3ea 28dfffea ffffffff    # rva match replace mask
 *     0x458b30 c8dfffea                      # rva replace, no match to check
 *
 * The fields are runs of hexadecimal digits with no separators, upper or
 * lower case, and the digits are in pairs: a byte array of one to sixteen
 * bytes, the number of bytes is the field's width, and an odd number of
 * bytes is as ordinary as an even one. A field is written
 * most significant byte first, the way an instruction is written, and applied
 * least significant byte first, the way the machine keeps it. Whitespace and
 * comments may appear between any two tokens, so the parse follows the
 * grammar rather than the lines. A site whose fields cannot describe a write
 * is left out and counted, not a reason to reject the file
 *
 * The three lines above are matchers: a file may carry any of them, each at
 * most once, and whichever it carries has to agree with the image before a
 * single site is written. One it does not carry is not asked about, which is
 * what lets a list be written for a kernel with no program database (peFile
 * and the hash) or, better, for one that has (the uuid, which is the same in
 * the file and in memory). A file with none of them, or with two copies of
 * one, is refused whole: there would be nothing to check it against
 *
 * The uuid is one value with two spellings and both are taken, because both
 * are in use: a debugger prints the symbol path of a kernel from the first
 * spelling and the image's own record holds the second. See
 * usPatchGUIDMatches() for which bytes differ
 */
#ifndef US_PATCHLIST_H
#define US_PATCHLIST_H

#include <stdbool.h>
#include <stdint.h>

/* One instruction is four bytes; the format allows up to a whole cache line,
 * which is what a future patch of something else might want */
#define US_PATCH_MAX_WIDTH 16U

typedef struct {
    uint32_t rva;
    uint8_t  width;
    uint8_t  match[US_PATCH_MAX_WIDTH];
    uint8_t  mask[US_PATCH_MAX_WIDTH];
    uint8_t  replace[US_PATCH_MAX_WIDTH];
} UsPatchSite;

typedef enum {
    UsPatchOk = 0,
    UsPatchUnsupported,   /* the first instruction is not a version we know */
    UsPatchSyntax,        /* a token where the grammar does not allow one */
    UsPatchNoTarget,      /* kept for callers that predate the matchers */
    UsPatchNoHash,
    UsPatchTooManySites,  /* the caller's array is full */
    UsPatchDuplicate,     /* a matcher that appears twice */
    UsPatchNoMatchers,    /* nothing to check the list against */
} UsPatchStatus;

typedef struct {
    UsPatchStatus status;
    const char   *target;         /* points into the text that was parsed */
    uint32_t      targetLength;
    uint8_t       hash[32];
    bool          hasHash;
    /* The build as the program database states it: the pair the image's own
     * CodeView record carries. Nothing relocates those bytes and the loader
     * does not patch them, so they are the same in the file and in memory.
     * The bytes are kept in the order the line spells them, which is one of
     * the two spellings usPatchGUIDMatches() accepts */
    uint8_t       pdbGUID[16];
    uint32_t      pdbAge;
    bool          hasPDBIdentity;
    uint32_t      sites;          /* filled in */
    uint32_t      skipped;        /* site lines left out for their shape */
    uint32_t      errorAt;        /* offset of the token that was refused */
} UsPatchFile;

/*
 * Parse a whole file. The caller owns the site array; a file that parses is
 * rejected when it does not fit, rather than being applied in part
 */
UsPatchStatus usPatchParse(const char *text, uint32_t length, UsPatchSite *sites,
                           uint32_t capacity, UsPatchFile *out);

/* Does this memory hold what the site says it should? */
bool usPatchMatches(const UsPatchSite *site, const uint8_t *memory);

/* Put the replacement there */
void usPatchWrite(const UsPatchSite *site, uint8_t *memory);

/*
 * Is the uuid a list spells the build an image states? The first two arguments
 * are the same sixteen bytes, one as a line wrote them and one as the image's
 * CodeView record holds them
 *
 * The first three fields of a uuid are numbers, and a number is written with
 * its most significant digit first while the record keeps its least
 * significant byte first. So 1713AA50-9114-C4C6-99D3-46F086029846, which is
 * how a debugger prints a kernel's symbol path, is 50aa1317-1491-c6c4-99d3-
 * 46f086029846 in the record. The last eight bytes are a byte string and are
 * the same either way. Both spellings name one build, so a list matches when
 * it writes either of them: one a person copied from a debugger, and one
 * scripts/enumerate_sites.py prints from the record itself
 */
bool usPatchGUIDMatches(const uint8_t *written, const uint8_t *record);

#endif
