#include <stddef.h>

#include "core/patchapply.h"
#include "core/sha256.h"
#include "core/sha256.h"

static uint32_t stemLength(const char *name) {
    uint32_t i = 0;

    while (name[i] != '\0' && name[i] != '.') {
        i++;
    }
    return i;
}

static char lowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool stemEquals(const char *left, uint32_t leftLength, const char *right) {
    uint32_t rightLength = stemLength(right);

    if (leftLength != rightLength) {
        return false;
    }
    for (uint32_t i = 0; i < leftLength; i++) {
        if (lowerAscii(left[i]) != lowerAscii(right[i])) {
            return false;
        }
    }
    return true;
}

bool usPatchTargetMatches(const UsPatchFile *file, const char *imageName) {
    if (file->target == NULL || imageName == NULL) {
        return false;
    }
    return stemEquals(file->target, file->targetLength, imageName);
}

static bool memcmpBytes(const uint8_t *left, const uint8_t *right, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++) {
        if (left[i] != right[i]) {
            return true;
        }
    }
    return false;
}

static bool digestMatches(const UsPatchFile *file, const uint8_t *digest) {
    if (digest == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < 32U; i++) {
        if (digest[i] != file->hash[i]) {
            return false;
        }
    }
    return true;
}

UsPatchApplyResult usPatchApplyMatched(const UsPatchFile *file, const UsPatchSite *sites,
                                       const UsPatchMatchers *matchers, uint32_t textRva,
                                       uint8_t *text, uint32_t textBytes,
                                       UsPatchStats *stats) {
    stats->files++;

    /* Whichever matchers the file carries have to agree; the ones it does not
     * carry are not asked about, which is what lets one format serve kernels
     * that publish a program database and kernels that do not */
    if (file->target != NULL) {
        if (matchers == NULL || matchers->imageName == NULL) {
            return UsPatchUnverifiable;
        }
        if (!usPatchTargetMatches(file, matchers->imageName)) {
            stats->wrongTarget++;
            return UsPatchNotThisImage;
        }
    }
    if (file->hasHash) {
        if (matchers == NULL || !digestMatches(file, matchers->digest)) {
            stats->wrongBuild++;
            return matchers == NULL || matchers->digest == NULL ? UsPatchUnverifiable
                                                                : UsPatchWrongBuild;
        }
    }
    if (file->hasPdbIdentity) {
        if (matchers == NULL || matchers->identity == NULL) {
            return UsPatchUnverifiable;
        }
        if (matchers->identity->age != file->pdbAge
            || memcmpBytes(matchers->identity->guid, file->pdbGuid, 16U)) {
            stats->wrongBuild++;
            return UsPatchWrongBuild;
        }
    }
    if (text == NULL || textBytes == 0U) {
        return UsPatchNoText;
    }

    stats->accepted++;
    for (uint32_t i = 0; i < file->sites; i++) {
        const UsPatchSite *site = &sites[i];
        uint32_t width = site->width;
        uint32_t offset = site->rva - textRva;

        if (width == 0U || site->rva < textRva || offset > textBytes
            || width > textBytes - offset) {
            stats->outOfRange++;
            continue;
        }
        if (!usPatchMatches(site, text + offset)) {
            stats->refused++;
            continue;
        }
        usPatchWrite(site, text + offset);
        stats->applied++;
    }
    return UsPatchApplied;
}

UsPatchApplyResult usPatchApplyFile(const UsPatchFile *file, const UsPatchSite *sites,
                                    const char *imageName, uint32_t textRva,
                                    uint8_t *text, uint32_t textBytes,
                                    UsPatchStats *stats) {
    uint8_t digest[32];
    UsPatchMatchers matchers = { imageName, digest, NULL };

    if (text == NULL || textBytes == 0U) {
        stats->files++;
        return UsPatchNoText;
    }
    usSha256(text, textBytes, digest);
    return usPatchApplyMatched(file, sites, &matchers, textRva, text, textBytes, stats);
}
