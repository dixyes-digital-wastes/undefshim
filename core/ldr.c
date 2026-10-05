/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Walking the loader's module list, see ldr.h
 *
 * The offsets are from the loader's own structures on arm64, where every
 * pointer is eight bytes. They are written out with the field names rather
 * than as a packed struct, for the same reason the ACPI tables are read that
 * way: a struct the compiler is free to pad is how a reader ends up a field
 * out, and the failure would be an address that looks fine
 *
 *   LOADER_PARAMETER_BLOCK   four ULONGs, then the list heads
 *   KLDR_DATA_TABLE_ENTRY    InLoadOrderLinks first, so a list node is the
 *                            structure, and the fields follow in the order the
 *                            kernel's own headers declare them
 *
 * Each field is validated on the way out. A name match is what makes the walk
 * meaningful: without it a wrong offset still yields addresses, just wrong
 * ones, and nothing about them looks wrong
 */

#include "core/ldr.h"

/* --- LOADER_PARAMETER_BLOCK ------------------------------------------------- */

#define LDR_LOAD_ORDER_LIST 0x10U

/* --- KLDR_DATA_TABLE_ENTRY -------------------------------------------------- */

#define KLDR_DLL_BASE 0x30U
#define KLDR_ENTRY_POINT 0x38U
#define KLDR_SIZE_OF_IMAGE 0x40U
#define KLDR_BASE_DLL_NAME 0x58U

/* A UNICODE_STRING: length, maximum length, and a pointer, which the padding
 * before it pushes to the second half of the sixteen bytes */
#define UNICODE_STRING_BUFFER 8U

/* The list is terminated by pointing back at its head, which is what keeps the
 * walk finite on a well formed list. A corrupted one has no such promise, so
 * there is also a count */
#define US_LDR_MAX_ENTRIES 512U

/*
 * An address that could plausibly be a mapped module
 *
 * The floor is well above anything the loader would use for a NULL or an
 * error. The ceiling is the top of the address space rather than anything
 * narrower: the kernel's modules live at the very top of it, so a bound that
 * excluded those would exclude every address this is looking for
 */
static bool plausibleBase(uint64_t v) {
    return v >= 0x10000ULL && v <= 0xFFFFFFFFFFFFF000ULL;
}

static uint64_t read64(const uint8_t *p) {
    uint64_t v = 0;

    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (i * 8);
    }
    return v;
}

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

/*
 * Whether a UTF-16 name is the given ASCII one, case insensitively
 *
 * Lengths first: a name that is a prefix of another would otherwise compare
 * equal, and `ntoskrnl.exe` against a longer name beginning the same way is
 * exactly the mistake worth refusing
 */
static bool nameMatches(const void *utf16, uint32_t chars, const char *ascii) {
    const uint8_t *p = utf16;

    for (uint32_t i = 0; i < chars; i++) {
        uint16_t c = (uint16_t)(p[i * 2] | (p[i * 2 + 1] << 8));
        char want = ascii[i];

        if (want == '\0') {
            /* The name in the list is longer than the one asked for */
            return false;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (uint16_t)(c - 'A' + 'a');
        }
        if ((char)c != want) {
            return false;
        }
    }
    return ascii[chars] == '\0';
}

bool usLDRFindModule(const void *loaderBlock, const char *asciiName,
                     UsLDRModule *out) {
    const uint8_t *ldr = loaderBlock;
    uint64_t head;
    uint64_t node;

    if (ldr == NULL || asciiName == NULL || out == NULL) {
        return false;
    }

    head = (uint64_t)(uintptr_t)loaderBlock + LDR_LOAD_ORDER_LIST;
    node = read64(ldr + LDR_LOAD_ORDER_LIST);

    for (uint32_t i = 0; i < US_LDR_MAX_ENTRIES; i++) {
        const uint8_t *entry;
        uint16_t nameChars;
        uint64_t nameAt;
        uint64_t base;
        uint64_t size;

        /* Back at the head: the list is a ring and this is its end */
        if (node == head || !plausibleBase(node)) {
            return false;
        }

        entry = (const uint8_t *)(uintptr_t)node;
        base = read64(entry + KLDR_DLL_BASE);
        size = read32(entry + KLDR_SIZE_OF_IMAGE);
        nameChars = (uint16_t)(read64(entry + KLDR_BASE_DLL_NAME) & 0xFFFFU);
        nameAt = read64(entry + KLDR_BASE_DLL_NAME + UNICODE_STRING_BUFFER);

        /* The list is a ring of nodes that each point at the next, so a node
         * that does not is not one */
        if (!plausibleBase(base) || size == 0 || size > 0x40000000ULL
            || nameChars == 0 || nameChars > 512 || !plausibleBase(nameAt)) {
            return false;
        }

        if (nameMatches((const void *)(uintptr_t)nameAt, nameChars / 2U,
                        asciiName)) {
            out->base = base;
            out->entry = read64(entry + KLDR_ENTRY_POINT);
            out->size = size;
            out->nameChars = nameChars / 2U;
            return true;
        }

        node = read64(entry);
    }
    return false;
}
