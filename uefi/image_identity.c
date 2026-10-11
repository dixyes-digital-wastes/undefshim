/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Which build an image is, read out of the image
 */

#include "core/patchapply.h"
#include "core/pe.h"
#include "core/sha256.h"
#include "uefi/image_identity.h"

/* Little endian fields, as they are in the image */
static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

static uint32_t rd16le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/*
 * The bytes a list's addresses refer to: the raw part of the text section, as
 * it is in the file and as it is in memory, which for this image are the same
 * bytes at the same relative place
 */
uint8_t *usImageTextBytes(UsImage *image, uint32_t *outBytes) {
    const UsPESection *text = usImageFindSection(image, ".text");

    if (text == NULL || text->rawSize == 0) {
        return NULL;
    }
    *outBytes = text->rawSize;
    return (uint8_t *)(uintptr_t)(image->base + text->virtualAddress);
}

/*
 * The build's identity, as the image itself carries it: the debug directory's
 * CodeView entry holds the program database's GUID and age. Those bytes are
 * the same in the file and in memory - nothing relocates them and the loader
 * does not patch them - so a list written from the file can be checked
 * against the running image without ever having run it
 */
bool usImageIdentity(UsImage *image, UsPatchIdentity *out) {
    uint32_t size = 0;
    const uint8_t *table = usImageDataDirectory(image, 6U, &size);

    if (table == NULL) {
        return false;
    }
    for (uint32_t at = 0; at + 28U <= size; at += 28U) {
        uint32_t type = rd32le(table + at + 12);
        uint32_t bytes = rd32le(table + at + 16);
        uint32_t rva = rd32le(table + at + 20);
        const uint8_t *record;

        if (type != 2U || bytes < 24U) {
            continue;
        }
        record = usImageRVAToPtr(image, rva);
        if (record == NULL || rd32le(record) != 0x53445352U) {   /* RSDS */
            continue;
        }
        for (uint32_t i = 0; i < 16U; i++) {
            out->guid[i] = record[4U + i];
        }
        out->age = rd32le(record + 20U);
        return true;
    }
    return false;
}

/*
 * A uuid as a debugger prints it, which is how a list and a symbol path both
 * write it: the sixteen bytes with hyphens after the fourth, sixth, eighth and
 * tenth, in the order the caller hands them over
 */
void usImageFormatGUID(const uint8_t *bytes, char out[US_IMAGE_GUID_CHARS]) {
    static const char digits[] = "0123456789abcdef";
    uint32_t at = 0;

    for (uint32_t i = 0; i < 16U; i++) {
        if (i == 4U || i == 6U || i == 8U || i == 10U) {
            out[at++] = '-';
        }
        out[at++] = digits[bytes[i] >> 4];
        out[at++] = digits[bytes[i] & 0xfU];
    }
    out[at] = '\0';
}

/* The first three fields of a uuid are numbers: the record holds them least
 * significant byte first, and everything that prints one writes them the other
 * way. The eight bytes after them are a byte string and do not move */
void usImageGUIDTurnedRound(const uint8_t *record, uint8_t out[16]) {
    static const uint8_t turnRound[16] = {
        3U, 2U, 1U, 0U, 5U, 4U, 7U, 6U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U,
    };

    for (uint32_t i = 0; i < 16U; i++) {
        out[i] = record[turnRound[i]];
    }
}

void usImageFormatHash(const uint8_t *bytes, char out[US_IMAGE_HASH_CHARS]) {
    static const char digits[] = "0123456789abcdef";

    for (uint32_t i = 0; i < 32U; i++) {
        out[i * 2U] = digits[bytes[i] >> 4];
        out[i * 2U + 1U] = digits[bytes[i] & 0xfU];
    }
    out[32U * 2U] = '\0';
}

/* Most a kernel's relocation table is going to hold. Anything past this is
 * not counted, which can only make the digest stricter, never wrong */
#define US_PATCH_MAX_RELOCATIONS 65536U

static uint32_t relocations[US_PATCH_MAX_RELOCATIONS];
static uint32_t relocationCount;

/*
 * The loader writes each relocated address into the image at the offsets the
 * relocation table names, so those bytes are a function of where the image
 * was loaded and differ from boot to boot. Everything else in the text is the
 * build. A digest that has to be the same every time therefore has to leave
 * them out, and it has to leave out exactly what the list's author did
 */
static void collectRelocations(UsImage *image, uint32_t textRVA, uint32_t textBytes) {
    uint32_t size = 0;
    const uint8_t *table;

    relocationCount = 0;
    table = usImageDataDirectory(image, US_PE_DIRECTORY_RELOCATIONS, &size);
    if (table == NULL) {
        return;
    }
    for (uint32_t at = 0; at + 8U <= size;) {
        uint32_t page = rd32le(table + at);
        uint32_t blockSize = rd32le(table + at + 4);

        if (blockSize < 8U || at + blockSize > size) {
            break;
        }
        for (uint32_t entry = at + 8U; entry + 2U <= at + blockSize; entry += 2U) {
            uint32_t value = (uint32_t)rd16le(table + entry);
            uint32_t type = value >> 12;
            uint32_t rva = page + (value & 0xfffU);

            /* Every target is skipped by the same width, whatever the entry
             * says it is: the two sides of the comparison only have to agree
             * with each other, and a wider skip is the safer agreement */
            (void)type;
            if (rva < textRVA || rva >= textRVA + textBytes) {
                continue;
            }
            if (relocationCount < US_PATCH_MAX_RELOCATIONS) {
                relocations[relocationCount++] = rva - textRVA;
            }
        }
        at += blockSize;
    }
    /* Sorted, so the digest can be fed the gaps in one pass */
    for (uint32_t i = 1; i < relocationCount; i++) {
        uint32_t value = relocations[i];
        uint32_t j = i;

        while (j > 0 && relocations[j - 1] > value) {
            relocations[j] = relocations[j - 1];
            j--;
        }
        relocations[j] = value;
    }
}

/* The digest the driver prints and the lists carry */
void usImageTextDigest(UsImage *image, const uint8_t *code, uint32_t bytes,
                       uint8_t out[32]) {
    UsSHA256 ctx;
    uint32_t at = 0;

    collectRelocations(image, (uint32_t)((uintptr_t)code - (uintptr_t)image->base), bytes);
    usSHA256Init(&ctx);
    for (uint32_t i = 0; i < relocationCount; i++) {
        uint32_t start = relocations[i];
        uint32_t end = start + 8U;

        if (start > bytes) {
            break;
        }
        if (end > bytes) {
            end = bytes;
        }
        if (start > at) {
            usSHA256Update(&ctx, code + at, start - at);
        }
        if (end > at) {
            at = end;
        }
    }
    if (bytes > at) {
        usSHA256Update(&ctx, code + at, bytes - at);
    }
    usSHA256Final(&ctx, out);
}
