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

UsPatchApplyResult usPatchApplyFile(const UsPatchFile *file, const UsPatchSite *sites,
                                    const char *imageName, uint8_t *text,
                                    uint32_t textBytes, UsPatchStats *stats) {
    uint8_t digest[32];

    stats->files++;
    if (!usPatchTargetMatches(file, imageName)) {
        stats->wrongTarget++;
        return UsPatchNotThisImage;
    }
    if (text == NULL || textBytes == 0U) {
        return UsPatchNoText;
    }

    /*
     * The hash is what makes a list safe to carry around: a list written for
     * another build of the same image would put instructions where they do not
     * belong, and there is no way to tell from the sites alone.
     */
    usSha256(text, textBytes, digest);
    for (uint32_t i = 0; i < 32U; i++) {
        if (digest[i] != file->hash[i]) {
            stats->wrongBuild++;
            return UsPatchWrongBuild;
        }
    }

    stats->accepted++;
    for (uint32_t i = 0; i < file->sites; i++) {
        const UsPatchSite *site = &sites[i];
        uint32_t width = site->width;

        if (width == 0U || site->rva > textBytes || width > textBytes - site->rva) {
            stats->outOfRange++;
            continue;
        }
        if (!usPatchMatches(site, text + site->rva)) {
            stats->refused++;
            continue;
        }
        usPatchWrite(site, text + site->rva);
        stats->applied++;
    }
    return UsPatchApplied;
}
