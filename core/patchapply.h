/*
 * Putting a parsed patch list onto an image
 *
 * The decisions live here, away from the file system, so they can be tested
 * on the host with a buffer standing in for an image: is this list for this
 * image, is it for this build of it, and does each site still hold what the
 * list says it should
 */
#ifndef US_PATCHAPPLY_H
#define US_PATCHAPPLY_H

#include <stdbool.h>
#include <stdint.h>

#include "core/patchlist.h"

typedef enum {
    UsPatchApplied = 0,    /* every matcher the list carries agreed */
    UsPatchNotThisImage,   /* it names another image */
    UsPatchWrongBuild,     /* the digest, or the program database's identity, is another build */
    UsPatchNoText,         /* the image has no text to write into */
    UsPatchUnverifiable,   /* it carries a matcher the caller cannot answer */
} UsPatchApplyResult;

/* The build as the program database states it: what the image's own CodeView
 * record holds */
typedef struct {
    uint8_t  guid[16];
    uint32_t age;
} UsPatchIdentity;

/*
 * What the caller knows about the image, for the matchers a list may carry.
 * A NULL field means "cannot answer", and a list that asks a question nobody
 * can answer is refused rather than applied on trust
 */
typedef struct {
    const char            *imageName;   /* compared on the stem */
    const uint8_t         *digest;      /* 32 bytes of the text, or NULL */
    const UsPatchIdentity *identity;    /* or NULL */
} UsPatchMatchers;

typedef struct {
    uint32_t files;
    uint32_t accepted;
    uint32_t applied;
    uint32_t refused;      /* sites whose bytes did not match */
    uint32_t outOfRange;   /* sites whose bytes are not all in the text */
    uint32_t wrongTarget;
    uint32_t wrongBuild;
} UsPatchStats;

/*
 * Is the name in this list the image the caller is holding? The comparison is
 * on the stem, without case or extension, so "ntoskrnl" and "ntoskrnl.exe"
 * are the same image
 */
bool usPatchTargetMatches(const UsPatchFile *file, const char *imageName);

/*
 * Check the hash and write every site that still matches. The text is written
 * in place; nothing is written for a site whose bytes are not what the list
 * says, and that is what refused counts
 */
UsPatchApplyResult usPatchApplyMatched(const UsPatchFile *file, const UsPatchSite *sites,
                                       const UsPatchMatchers *matchers, uint32_t textRva,
                                       uint8_t *text, uint32_t textBytes,
                                       UsPatchStats *stats);

#endif
