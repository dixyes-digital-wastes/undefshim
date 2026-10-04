/*
 * The patch list files the driver applies to an image before it runs.
 *
 * One file describes one target:
 *
 *     USPATCHV1
 *     peFile ntoskrnl
 *     textSHA256Hash 3f2a...c9
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
 * is left out and counted, not a reason to reject the file; a file whose
 * version, target or hash does not match is rejected whole.
 */
#ifndef US_PATCHLIST_H
#define US_PATCHLIST_H

#include <stdbool.h>
#include <stdint.h>

/* One instruction is four bytes; the format allows up to a whole cache line,
 * which is what a future patch of something else might want. */
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
    UsPatchNoTarget,      /* no peFile instruction */
    UsPatchNoHash,        /* no textSHA256Hash instruction, or not 32 bytes */
    UsPatchTooManySites,  /* the caller's array is full */
} UsPatchStatus;

typedef struct {
    UsPatchStatus status;
    const char   *target;         /* points into the text that was parsed */
    uint32_t      targetLength;
    uint8_t       hash[32];
    uint32_t      sites;          /* filled in */
    uint32_t      skipped;        /* site lines left out for their shape */
    uint32_t      errorAt;        /* offset of the token that was refused */
} UsPatchFile;

/*
 * Parse a whole file. The caller owns the site array; a file that parses is
 * rejected when it does not fit, rather than being applied in part.
 */
UsPatchStatus usPatchParse(const char *text, uint32_t length, UsPatchSite *sites,
                           uint32_t capacity, UsPatchFile *out);

/* Does this memory hold what the site says it should? */
bool usPatchMatches(const UsPatchSite *site, const uint8_t *memory);

/* Put the replacement there. */
void usPatchWrite(const UsPatchSite *site, uint8_t *memory);

#endif
