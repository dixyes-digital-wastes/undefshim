/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Getting the runtime pool from firmware memory, see pool.h
 */

#include <uefi.h>

#include "core/pool.h"
#include "uefi/pool.h"

/* Runtime services memory, so the OS keeps it instead of reclaiming it */
#define US_POOL_MEMORY_TYPE EfiRuntimeServicesData

bool usPoolAllocate(UsPoolAlloc *out, uint32_t stackSlots) {
    efi_physical_address_t pa = 0;
    efi_status_t status;
    uint64_t bytes;
    uint64_t pages;

    out->pool = NULL;
    out->basePA = 0;
    out->baseVA = 0;
    out->bytes = 0;

    /*
     * One stack per processor the machine has, not per processor it could
     * have: what is asked for is what will be used, and the callers have
     * already refused a count this cannot describe
     */
    if (stackSlots == 0 || stackSlots > US_MAX_CPUS) {
        return false;
    }
    bytes = usPoolBytes(stackSlots);
    pages = usPoolPages(stackSlots);

    status = BS->AllocatePages(AllocateAnyPages, US_POOL_MEMORY_TYPE,
                               (uintn_t)pages, &pa);
    if (EFI_ERROR(status) || pa == 0) {
        return false;
    }

    /*
     * The firmware mapped memory identity, so the physical address is also the
     * address to reach it by. Both are recorded because they stop being equal
     * once the address space is rebuilt, and the injector needs the physical
     * one to re-establish the mapping
     */
    out->basePA = (uint64_t)pa;
    out->baseVA = (uint64_t)pa;
    out->bytes = bytes;

    if (!usPoolInitLayout((UsPool *)(uintptr_t)out->baseVA, out->baseVA, out->basePA,
                          stackSlots)) {
        BS->FreePages(pa, (uintn_t)pages);
        out->basePA = 0;
        out->baseVA = 0;
        out->bytes = 0;
        return false;
    }

    out->pool = (UsPool *)(uintptr_t)out->baseVA;
    return usPoolIsValid(out->pool);
}
