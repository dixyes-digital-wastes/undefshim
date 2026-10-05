/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * ARM64 PE images, see pe.h for the contract
 */

#include "core/pe.h"

/* File offsets of the few header fields that have to be read before the
 * section table is known */
#define PE_OFF_DOS_PE_RVA 0x3CU

typedef struct PeFileHeader_t {
    uint16_t machine;
    uint16_t sectionCount;
    uint32_t timeDateStamp;
    uint16_t optionalHeaderSize;
    uint16_t characteristics;
} PeFileHeader;

typedef struct PeOptionalHeader_t {
    uint16_t magic;
    uint32_t entryRVA;
    uint64_t preferredBase;
    uint32_t sectionAlignment;
    uint32_t fileAlignment;
    uint32_t sizeOfImage;
    uint32_t sizeOfHeaders;
    uint16_t subsystem;
    uint32_t numberOfDirectories;
} PeOptionalHeader;

typedef struct PeSectionHeader_t {
    char     name[8];
    uint32_t virtualSize;
    uint32_t virtualAddress;
    uint32_t rawSize;
    uint32_t rawOffset;
    uint32_t characteristics;
} PeSectionHeader;

#define PE_OPTIONAL_MAGIC_64 0x20BU

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static bool nameEq(const char *a, uint32_t aLen, const char *b) {
    uint32_t i = 0;
    for (; i < aLen && b[i] != '\0'; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return i == aLen && b[i] == '\0';
}

static bool readHeaders(UsImage *img) {
    const uint8_t *b = img->base;
    uint32_t peOffset;
    const uint8_t *pe;
    PeFileHeader fh;
    PeOptionalHeader oh;
    uint32_t optOffset;
    uint32_t sectionTable;

    if (img->size < 0x40 || rd16(b) != US_PE_DOS_MAGIC) {
        return false;
    }
    peOffset = rd32(b + PE_OFF_DOS_PE_RVA);
    if (peOffset > img->size || img->size - peOffset < 24) {
        return false;
    }
    pe = b + peOffset;
    if (rd32(pe) != US_PE_MAGIC) {
        return false;
    }

    fh.machine = rd16(pe + 4);
    fh.sectionCount = rd16(pe + 6);
    fh.optionalHeaderSize = rd16(pe + 20);
    fh.characteristics = rd16(pe + 22);
    if (fh.machine != US_PE_MACHINE_ARM64) {
        return false;
    }
    if (fh.sectionCount == 0 || fh.sectionCount > US_PE_MAX_SECTIONS) {
        return false;
    }
    if (fh.optionalHeaderSize < 112) {
        return false;
    }

    optOffset = peOffset + 24;
    if (optOffset > img->size || img->size - optOffset < fh.optionalHeaderSize) {
        return false;
    }
    oh.magic = rd16(b + optOffset);
    if (oh.magic != PE_OPTIONAL_MAGIC_64) {
        return false;
    }
    oh.entryRVA = rd32(b + optOffset + 16);
    oh.preferredBase = rd64(b + optOffset + 24);
    oh.sectionAlignment = rd32(b + optOffset + 32);
    oh.sizeOfImage = rd32(b + optOffset + 56);
    oh.sizeOfHeaders = rd32(b + optOffset + 60);
    oh.subsystem = rd16(b + optOffset + 68);
    oh.numberOfDirectories = rd32(b + optOffset + 108);

    /* The data directories follow the fixed part of the optional header:
     * 112 bytes of fields, then one 8 byte pair per directory */
    uint32_t directories = oh.numberOfDirectories;
    if (directories > US_PE_MAX_DIRECTORIES) {
        directories = US_PE_MAX_DIRECTORIES;
    }
    for (uint32_t i = 0; i < directories; i++) {
        uint32_t at = optOffset + 112U + i * 8U;

        if (optOffset + fh.optionalHeaderSize < at + 8U) {
            directories = i;
            break;
        }
        img->dataDirectoryRVA[i] = rd32(b + at);
        img->dataDirectorySize[i] = rd32(b + at + 4);
    }
    img->dataDirectoryCount = directories;

    sectionTable = optOffset + fh.optionalHeaderSize;
    if (sectionTable > img->size || img->size - sectionTable < (size_t)fh.sectionCount * 40) {
        return false;
    }

    img->preferredBase = oh.preferredBase;
    img->sizeOfImage = oh.sizeOfImage;
    img->sizeOfHeaders = oh.sizeOfHeaders;
    img->entryRVA = oh.entryRVA;
    img->subsystem = oh.subsystem;
    img->sectionAlignment = (uint16_t)oh.sectionAlignment;
    img->sectionCount = fh.sectionCount;
    img->sectionTableOffset = sectionTable;

    for (uint16_t i = 0; i < fh.sectionCount; i++) {
        const uint8_t *s = b + sectionTable + (size_t)i * 40;
        UsPESection *out = &img->sections[i];

        out->nameLen = 0;
        while (out->nameLen < 8 && s[out->nameLen] != '\0') {
            out->nameLen++;
        }
        for (uint32_t k = 0; k < out->nameLen; k++) {
            out->name[k] = (char)s[k];
        }
        out->name[out->nameLen] = '\0';

        out->virtualSize = rd32(s + 8);
        out->virtualAddress = rd32(s + 12);
        out->rawSize = rd32(s + 16);
        out->rawOffset = rd32(s + 20);
        out->characteristics = rd32(s + 36);
    }

    return true;
}

bool usImageInitFile(UsImage *img, const void *data, size_t size) {
    *img = (UsImage){ 0 };
    if (data == NULL || size < 0x40) {
        return false;
    }
    img->base = data;
    img->size = size;
    img->view = UsImageViewFile;

    if (!readHeaders(img)) {
        return false;
    }

    /* In a file view every section must lie inside the bytes we hold,
     * otherwise a later lookup could walk off the end */
    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPESection *s = &img->sections[i];
        if (s->rawSize == 0) {
            continue;
        }
        if (s->rawOffset > size || size - s->rawOffset < s->rawSize) {
            return false;
        }
    }

    img->valid = true;
    return true;
}

bool usImageInitMemory(UsImage *img, const void *data, size_t size) {
    *img = (UsImage){ 0 };
    if (data == NULL || size < 0x40) {
        return false;
    }
    img->base = data;
    img->size = size;
    img->view = UsImageViewMemory;

    if (!readHeaders(img)) {
        return false;
    }
    /* A loaded image is at least as large as the headers it claims */
    if (img->sizeOfImage > size) {
        return false;
    }

    img->valid = true;
    return true;
}

const uint8_t *usImageRVASpan(const UsImage *img, uint32_t rva, size_t *available) {
    const uint8_t *p = NULL;
    size_t avail = 0;

    if (img == NULL || !img->valid) {
        return NULL;
    }
    if (rva == 0 && img->view == UsImageViewMemory) {
        return NULL;
    }

    if (img->view == UsImageViewMemory) {
        if (rva >= img->size) {
            return NULL;
        }
        p = img->base + rva;
        avail = img->size - rva;
    } else {
        if (rva < img->sizeOfHeaders) {
            if (rva >= img->size) {
                return NULL;
            }
            p = img->base + rva;
            avail = img->size - rva;
        } else {
            const UsPESection *s = usImageSectionOfRVA(img, rva);
            if (s == NULL || s->rawSize == 0) {
                return NULL;
            }
            uint32_t delta = rva - s->virtualAddress;
            if (delta >= s->rawSize) {
                return NULL;
            }
            if (s->rawOffset > img->size || img->size - s->rawOffset <= delta) {
                return NULL;
            }
            p = img->base + s->rawOffset + delta;
            avail = (s->rawSize - delta) < (img->size - (size_t)(s->rawOffset + delta))
                        ? (s->rawSize - delta)
                        : (img->size - (size_t)(s->rawOffset + delta));
        }
    }

    if (p == NULL) {
        return NULL;
    }
    if (available != NULL) {
        *available = avail;
    }
    return p;
}

const uint8_t *usImageRVAToPtr(const UsImage *img, uint32_t rva) {
    return usImageRVASpan(img, rva, NULL);
}

const UsPESection *usImageSectionOfRVA(const UsImage *img, uint32_t rva) {
    if (img == NULL || !img->valid) {
        return NULL;
    }
    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPESection *s = &img->sections[i];
        uint32_t span = s->virtualSize > s->rawSize ? s->virtualSize : s->rawSize;
        if (rva >= s->virtualAddress && rva - s->virtualAddress < span) {
            return s;
        }
    }
    return NULL;
}

const uint8_t *usImageDataDirectory(const UsImage *img, uint32_t index,
                                    uint32_t *outSize) {
    if (img == NULL || !img->valid || index >= img->dataDirectoryCount
        || img->dataDirectoryRVA[index] == 0) {
        return NULL;
    }
    if (outSize != NULL) {
        *outSize = img->dataDirectorySize[index];
    }
    return usImageRVAToPtr(img, img->dataDirectoryRVA[index]);
}

const UsPESection *usImageFindSection(const UsImage *img, const char *name) {
    if (img == NULL || !img->valid || name == NULL) {
        return NULL;
    }
    for (uint16_t i = 0; i < img->sectionCount; i++) {
        const UsPESection *s = &img->sections[i];
        if (nameEq(s->name, s->nameLen, name)) {
            return s;
        }
    }
    return NULL;
}

bool usImageHasSection(const UsImage *img, const char *name) {
    return usImageFindSection(img, name) != NULL;
}

/* Byte search inside the image, used by the classifier */
static bool imageContains(const UsImage *img, const uint8_t *needle, size_t needleLen) {
    if (needleLen == 0 || img->size < needleLen) {
        return false;
    }
    size_t limit = img->size - needleLen;
    for (size_t i = 0; i <= limit; i++) {
        size_t k = 0;
        while (k < needleLen && img->base[i + k] == needle[k]) {
            k++;
        }
        if (k == needleLen) {
            return true;
        }
    }
    return false;
}

static bool imageContainsUtf16(const UsImage *img, const char *ascii) {
    uint8_t buf[32];
    size_t n = 0;

    for (; ascii[n] != '\0'; n++) {
        if (n * 2 + 1 >= sizeof(buf)) {
            return false;
        }
        buf[n * 2] = (uint8_t)ascii[n];
        buf[n * 2 + 1] = 0;
    }
    return imageContains(img, buf, n * 2);
}

UsImageKind usImageClassify(const UsImage *img) {
    if (img == NULL || !img->valid) {
        return UsImageUnknown;
    }

    /*
     * INITKDBG and the PAGE family come from the kernel's linker script and
     * appear in no boot loader; OSLOADER.XSL is a load option winload must
     * carry, and it is how winload is told apart from a kernel, which also
     * has PAGE
     *
     * bootmgfw is deliberately not identified here. It is the image the
     * firmware was asked to load, so its identity comes from the protocol
     * that loaded it, and guessing from content would only add a way to be
     * wrong. This function answers "is this the kernel or the loader".
     */
    if (usImageHasSection(img, "INITKDBG") || usImageHasSection(img, "PAGEDATA")
        || usImageHasSection(img, "ALMOSTRO")) {
        return UsImageNtoskrnl;
    }
    if (imageContainsUtf16(img, "OSLOADER.XSL")) {
        return UsImageWinload;
    }
    return UsImageUnknown;
}

const char *usImageKindName(UsImageKind kind) {
    switch (kind) {
    case UsImageNtoskrnl:
        return "ntoskrnl";
    case UsImageWinload:
        return "winload";
    case UsImageBootmgfw:
        return "bootmgfw";
    default:
        return "unknown";
    }
}

/* Images are mapped page aligned, so a header can only ever start on a page */
#define US_PE_SCAN_STEP 4096U

int usPEScanRegion(const uint8_t *base, size_t size, UsImageVisitor visit, void *ctx) {
    int found = 0;

    if (base == NULL || visit == NULL || size < 0x40) {
        return 0;
    }

    for (size_t off = 0; off + 0x40 <= size; off += US_PE_SCAN_STEP) {
        const uint8_t *candidate = base + off;
        UsImage img;
        UsImageKind kind;

        /* The cheapest test there is, and it rejects almost everything: only
         * then is it worth validating a whole header */
        if (rd16(candidate) != US_PE_DOS_MAGIC) {
            continue;
        }
        if (!usImageInitMemory(&img, candidate, size - off)) {
            continue;
        }

        kind = usImageClassify(&img);
        if (kind == UsImageUnknown) {
            /* A PE that is neither of the stages we look for. There are many
             * in memory, and reporting them would drown the signal */
            continue;
        }

        found++;
        if (visit(&img, kind, ctx) == UsImageVisitStop) {
            break;
        }
    }

    return found;
}
