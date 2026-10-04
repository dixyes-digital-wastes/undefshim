/*
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
     * does not patch them, so they are the same in the file and in memory */
    uint8_t       pdbGuid[16];
    uint32_t      pdbAge;
    bool          hasPdbIdentity;
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

#endif
