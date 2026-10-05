/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * ARM64 PE images, as views rather than as file formats
 *
 * The same code has to describe an image twice: once as a file on disk, while
 * it is being examined on the host, and once as a loaded image in firmware
 * memory. The two differ only in how an RVA is turned into a pointer, so that
 * is the one thing UsImage abstracts. Everything else, the section table and
 * the derived offsets, is identical
 *
 * Nothing here calls into the firmware, so the whole file builds and runs on
 * a host as well as under UEFI
 */

#ifndef US_PE_H
#define US_PE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define US_PE_DOS_MAGIC 0x5A4DU     /* "MZ" */
#define US_PE_MAGIC 0x00004550U     /* "PE\0\0" */
#define US_PE_MACHINE_ARM64 0xAA64U

#define US_PE_SUBSYSTEM_EFI_APPLICATION 10U
#define US_PE_SUBSYSTEM_NATIVE 1U

#define US_PE_SECTION_EXECUTABLE 0x20000000U
#define US_PE_SECTION_WRITABLE 0x80000000U

#define US_PE_MAX_DIRECTORIES 16U

#define US_PE_MAX_SECTIONS 96

typedef struct UsPESection_t {
    char     name[9];  /* NUL terminated copy, for printing */
    uint32_t nameLen;
    uint32_t virtualSize;
    uint32_t virtualAddress;
    uint32_t rawSize;
    uint32_t rawOffset;
    uint32_t characteristics;
} UsPESection;

typedef enum UsImageView_e {
    /* base points at a mapped file: an RVA is only reachable through the
     * section table, and anything in the header area */
    UsImageViewFile,
    /* base points at a loaded image: an RVA is base plus the RVA */
    UsImageViewMemory,
} UsImageView;

typedef struct UsImage_t {
    const uint8_t *base;
    size_t         size;
    UsImageView    view;
    bool           valid;

    uint64_t preferredBase;
    uint32_t sizeOfImage;
    uint32_t sizeOfHeaders;
    uint32_t entryRVA;
    /* Data directory RVAs and sizes, as many as the header carries */
    uint32_t dataDirectoryRVA[US_PE_MAX_DIRECTORIES];
    uint32_t dataDirectorySize[US_PE_MAX_DIRECTORIES];
    uint32_t dataDirectoryCount;
    uint16_t subsystem;
    uint16_t sectionAlignment;
    uint16_t sectionCount;
    uint32_t sectionTableOffset;  /* into base, not an RVA */
    UsPESection sections[US_PE_MAX_SECTIONS];
} UsImage;

typedef enum UsImageKind_e {
    UsImageUnknown = 0,
    UsImageNtoskrnl,
    UsImageWinload,
    /*
     * Not produced by usImageClassify. bootmgfw is the image the firmware was
     * asked for, so the caller knows it from the protocol; the registry tags
     * it with this kind directly
     */
    UsImageBootmgfw,
} UsImageKind;

/* Builds a view. usImageInitFile fails unless the headers are consistent with
 * the bytes present, so a valid view is safe to walk */
bool usImageInitFile(UsImage *img, const void *data, size_t size);
bool usImageInitMemory(UsImage *img, const void *data, size_t size);

/*
 * Translates an RVA into a pointer, or NULL when it is outside the image. The
 * result is at least one byte long; callers that read more must bound the
 * read themselves, which usImageRVASpan helps with
 */
const uint8_t *usImageRVAToPtr(const UsImage *img, uint32_t rva);

/* Like usImageRVAToPtr, but also reports how many bytes are readable from
 * there, clamped to the end of the image */
const uint8_t *usImageRVASpan(const UsImage *img, uint32_t rva, size_t *available);

/* The section containing an RVA, or NULL */
const UsPESection *usImageSectionOfRVA(const UsImage *img, uint32_t rva);
const UsPESection *usImageFindSection(const UsImage *img, const char *name);

/* Index of the base relocation directory, for a hash that has to hold still
 * across boots: the loader writes relocated addresses into the image, and
 * those bytes differ every time it is loaded somewhere else */
#define US_PE_DIRECTORY_RELOCATIONS 5U

/* Where a data directory's bytes are, or NULL when it is absent */
const uint8_t *usImageDataDirectory(const UsImage *img, uint32_t index,
                                    uint32_t *outSize);
bool usImageHasSection(const UsImage *img, const char *name);

/*
 * Identifies an image from its content alone, for the images that arrive
 * without a protocol to ask: winload and ntoskrnl are read off disk by the
 * loader that preceded them, so there is no LoadedImage to consult
 *
 * Section names separate ntoskrnl, whose linker script emits INITKDBG and the
 * PAGE family, from the boot loaders. The OSLOADER.XSL resource string
 * separates winload: it is a required load option and is absent from every
 * other image. bootmgfw is not distinguished from bootmgr; the caller knows
 * which one it asked the firmware to load
 */
UsImageKind usImageClassify(const UsImage *img);

/* The name the preprocessor knows this kind by, for messages */
const char *usImageKindName(UsImageKind kind);

/*
 * Searching a stretch of memory for images
 *
 * Only the first image of each stage arrives with a protocol attached; winload
 * and the kernel are read off disk by the stage before them, so the only way
 * to find them is to look. They are mapped as whole images, so their headers
 * are present, and a header is the one thing that says where its own parts
 * are
 *
 * The scan steps by page, because images are mapped page aligned, and reports
 * every image it recognises. It is deliberately a plain function over a
 * buffer rather than something that walks the UEFI memory map: that keeps the
 * interesting part testable, and leaves the memory map walking to the caller
 */
typedef enum UsImageVisit_e {
    UsImageVisitContinue,
    UsImageVisitStop,
} UsImageVisit;

typedef UsImageVisit (*UsImageVisitor)(const UsImage *img, UsImageKind kind, void *ctx);

/* Returns how many images were recognised */
int usPEScanRegion(const uint8_t *base, size_t size, UsImageVisitor visit, void *ctx);


#endif
