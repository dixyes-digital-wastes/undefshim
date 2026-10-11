/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * What is said about a list that was not applied, and about the build an image
 * is
 */

#include <uefi.h>

#include "core/patchapply.h"
#include "core/patchlist.h"
#include "uefi/console.h"
#include "uefi/image_identity.h"
#include "uefi/patch_apply.h"
#include "uefi/patch_report.h"

/*
 * Why a list was refused. What a reader needs here is what the machine is,
 * said the way the list says what it is for: the build's uuid in both
 * spellings, because a debugger prints one of them and the image's own record
 * holds the other, and a uuid that looks the same in both is exactly the
 * mistake this report exists to make visible
 */
void usPatchReportRefusal(const char *name, const UsPatchFile *file,
                          const UsPatchMatchers *matchers, const UsPatchStats *stats) {
    char hex[US_IMAGE_HASH_CHARS];
    char guid[US_IMAGE_GUID_CHARS];
    char turned[US_IMAGE_GUID_CHARS];
    uint8_t turnedBytes[16];

    switch (stats->mismatch) {
    case UsPatchMismatchName: {
        /* The name in the list is a run of the file's own text, not a
         * terminated string, and it is short by construction */
        char target[16];
        uint32_t bytes = file->targetLength;

        if (bytes > sizeof(target) - 1U) {
            bytes = sizeof(target) - 1U;
        }
        memcpy(target, file->target, bytes);
        target[bytes] = '\0';
        usLogW("patch", "%s names %s, this image is %s\n", name, target,
               matchers->imageName);
        return;
    }
    case UsPatchMismatchDigest:
        usImageFormatHash(matchers->digest, hex);
        usLogW("patch", "%s is for another build: text sha256\n", name);
        usLogI("patch", "  image " US_VALUE("%s") "\n", hex);
        return;
    case UsPatchMismatchIdentity:
        usImageFormatGUID(file->pdbGUID, guid);
        usLogW("patch", "%s is for another build: pdb\n", name);
        usLogI("patch", "  the list asks for " US_VALUE("%s") "-" US_VALUE("%u") "\n",
               guid, (unsigned)file->pdbAge);
        usImageGUIDTurnedRound(matchers->identity->guid, turnedBytes);
        usImageFormatGUID(matchers->identity->guid, guid);
        usImageFormatGUID(turnedBytes, turned);
        usLogI("patch", "  this image is " US_VALUE("%s") "-" US_VALUE("%u")
               ", which a debugger writes " US_VALUE("%s") "-" US_VALUE("%u") "\n",
               guid, (unsigned)matchers->identity->age,
               turned, (unsigned)matchers->identity->age);
        return;
    case UsPatchMismatchNone:
    default:
        break;
    }
    if (file->hasPDBIdentity && matchers->identity == NULL) {
        usLogW("patch", "%s asks for a build by its program database, and this "
               "image states none\n", name);
        return;
    }
    usLogW("patch", "%s is not for this image\n", name);
}

/*
 * The site a list could not use, at the level the count is worth reading: one
 * address is what tells a list written for another build from one the machine
 * has already had applied
 */
void usPatchReportSites(const UsPatchStats *stats) {
    if (stats->refused != 0U) {
        usLogI("patch", "  first of them " US_VALUE("%#x")
               ", the bytes there are not the ones the list names\n",
               (unsigned)stats->firstRefused);
    }
    if (stats->outOfRange != 0U) {
        usLogI("patch", "  first outside the image " US_VALUE("%#x") "\n",
               (unsigned)stats->firstOutOfRange);
    }
}

/*
 * Says which build an image is, in the two ways anything downstream is keyed
 * on it: the digest of its text, which a list is written against, and the
 * identity of the program database it was linked against, which a symbol
 * server is. Both are read out of the image itself, so a build can be named
 * from the machine without ever having had the file
 *
 * The digest is over megabytes of text, so nothing is hashed until something
 * is going to be said
 */
void usImageReportBuild(UsImage *image) {
    uint32_t bytes = 0;
    uint8_t *code;
    uint8_t digest[32];
    char hex[US_IMAGE_HASH_CHARS];
    UsPatchIdentity identity;

    if (image == NULL || !usConsoleWants(UsLogVerbose)) {
        return;
    }
    code = usImageTextBytes(image, &bytes);
    if (code != NULL) {
        usImageTextDigest(image, code, bytes, digest);
        usImageFormatHash(digest, hex);
        usLogV("image", "text sha256 %s\n", hex);
    }
    if (usImageIdentity(image, &identity)) {
        /* Both spellings of the build: the bytes as the record holds them,
         * which is what the image's own file states and what a list written
         * from it carries, and the same value as a debugger writes it, which
         * is what a symbol path shows and what a person is likely to copy */
        char guid[US_IMAGE_GUID_CHARS];
        char turned[US_IMAGE_GUID_CHARS];
        uint8_t turnedBytes[16];

        usImageFormatGUID(identity.guid, guid);
        usImageGUIDTurnedRound(identity.guid, turnedBytes);
        usImageFormatGUID(turnedBytes, turned);
        usLogV("image", "pdb %s-" US_VALUE("%u") ", written %s-" US_VALUE("%u") "\n",
               guid, (unsigned)identity.age, turned, (unsigned)identity.age);
    }
}
