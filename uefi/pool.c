/*
 * Getting the runtime pool from firmware memory, see pool.h
 */

#include <uefi.h>

#include "core/pool.h"
#include "uefi/pool.h"

/* Runtime services memory, so the OS keeps it instead of reclaiming it */
#define US_POOL_MEMORY_TYPE EfiRuntimeServicesData

bool usPoolAllocate(UsPoolAlloc *out) {
    efi_physical_address_t pa = 0;
    efi_status_t status;

    out->pool = NULL;
    out->basePA = 0;
    out->baseVA = 0;
    out->bytes = 0;

    status = BS->AllocatePages(AllocateAnyPages, US_POOL_MEMORY_TYPE,
                               (uintn_t)US_POOL_PAGES, &pa);
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
    out->bytes = US_POOL_BYTES;

    if (!usPoolInitLayout((UsPool *)(uintptr_t)out->baseVA, out->baseVA, out->basePA)) {
        BS->FreePages(pa, (uintn_t)US_POOL_PAGES);
        out->basePA = 0;
        out->baseVA = 0;
        return false;
    }

    out->pool = (UsPool *)(uintptr_t)out->baseVA;
    return usPoolIsValid(out->pool);
}
