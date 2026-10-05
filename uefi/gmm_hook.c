/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The memory map hook, see gmm_hook.h for why this is the moment to act
 */

#include <uefi.h>

#include "common/layout.h"
#include "core/pe.h"
#include "core/scan.h"
#include "uefi/console.h"
#include "uefi/gmm_hook.h"
#include "uefi/patch.h"
#include "uefi/service_hook.h"
#include "uefi/stack.h"
#include "uefi/work.h"

static UsSession *gSession;
static UsServiceHook gHook;
static efi_get_memory_map_t gOriginal;
static bool gDone;
static uint32_t gScans;
static bool gReportedWinload;
static bool gReportedKernel;

/*
 * The kernel is loaded by the loader, which runs after the first maps the
 * boot manager asks for, so the hook cannot leave on the first success. This
 * cap is what keeps a boot that never loads one from scanning for ever
 */
#define US_GMM_MAX_SCANS 64

/*
 * Only loader memory is searched. Images are loaded into these two types and
 * nowhere else, and restricting the search is what keeps it from walking
 * hundreds of megabytes of conventional memory on every call
 */
static bool isImageMemory(uint32_t type) {
    return type == EfiLoaderCode || type == EfiLoaderData;
}

/*
 * Walks the map and hands every loader region to the scanner. The map is a
 * packed array of records whose stride the firmware chooses, so the only way
 * to move through it is by DescriptorSize
 */
static int scanMap(const efi_memory_descriptor_t *map, uintn_t mapSize, uintn_t descSize) {
    const uint8_t *p = (const uint8_t *)map;
    const uint8_t *end = p + mapSize;
    int added = 0;

    if (descSize == 0 || descSize > mapSize) {
        return 0;
    }

    while (p + descSize <= end) {
        const efi_memory_descriptor_t *d = (const efi_memory_descriptor_t *)p;

        if (isImageMemory(d->Type) && d->NumberOfPages != 0) {
            added += usRegistryScanRegion(&gSession->registry,
                                          (const void *)(uintptr_t)d->PhysicalStart,
                                          (size_t)(d->NumberOfPages * US_PAGE_SIZE));
        }
        p += descSize;
    }

    return added;
}

/*
 * What runs on our own stack: the scan, and everything it drags in
 *
 * The image scanner keeps an image header on the stack, and that header holds
 * the section table, so it is a few kilobytes. That is fine on a stack we
 * allocated and fatal on one we did not: bootmgfw calls GetMemoryMap deep in
 * its own work, and the caller's frame here is not ours to spend. Running the
 * scan through this indirection is what makes the hook safe
 */
typedef struct ScanRequest_t {
    const efi_memory_descriptor_t *map;
    uintn_t mapSize;
    uintn_t descSize;
    int added;
} ScanRequest;

static void scanOnOwnStack(void *arg) {
    ScanRequest *req = arg;

    req->added = scanMap(req->map, req->mapSize, req->descSize);
}

/*
 * Applying the patch table on our own stack as well. It walks the table,
 * resolves image names and writes into the images, and none of that belongs
 * on the caller's stack: the caller here is the boot manager, deep in a
 * memory map of its own
 */
static void applyPatchesOnOwnStack(void *arg) {
    (void)arg;
    usPatchApplyPending(gSession);
}

static efi_status_t EFIAPI gmmHook(uintn_t *memoryMapSize, efi_memory_descriptor_t *memoryMap,
                                   uintn_t *mapKey, uintn_t *descriptorSize,
                                   uint32_t *descriptorVersion) {
    efi_status_t status;
    ScanRequest req;

    /*
     * Ours does nothing but observe, so the firmware's is called first and its
     * answer is returned unchanged. Reporting anything else would break the
     * caller, and the caller at this point is the boot manager
     */
    status = gOriginal(memoryMapSize, memoryMap, mapKey, descriptorSize, descriptorVersion);

    /*
     * The first call of a pair is usually a size probe that fails with
     * EFI_BUFFER_TOO_SMALL and no usable map. Scanning a map that was not
     * filled in would read whatever the caller's buffer happened to hold
     */
    if (EFI_ERROR(status) || memoryMap == NULL || descriptorSize == NULL) {
        return status;
    }

    if (usRegistryGet(&gSession->registry, UsImageNtoskrnl) != NULL) {
        /* Both stages are in hand; there is nothing left to watch for */
        if (!gDone) {
            usServiceHookRemove(&gHook);
            gDone = true;
        }
        return status;
    }

    usConsoleProgress('g');

    req.map = memoryMap;
    req.mapSize = *memoryMapSize;
    req.descSize = *descriptorSize;
    req.added = 0;
    usConsoleProgress('1');
    usStackRunOn(gSession->bootStackTop, scanOnOwnStack, &req);
    usConsoleProgress('2');

    {
        UsImage *winload = usRegistryGet(&gSession->registry, UsImageWinload);
        UsImage *kernel = usRegistryGet(&gSession->registry, UsImageNtoskrnl);

        if (winload != NULL && !gReportedWinload) {
            gReportedWinload = true;
            usLogV("gmm", "winload found at " US_VALUE("%#llx") "\n",
                   (unsigned long long)(uintptr_t)winload->base);

            UsLeafSite leaf = usLocateTransferLeaf(winload);
            if (leaf.found) {
                usLogI("gmm", "winload leaf found\n");
            } else if (leaf.matches == 0) {
                usLogW("gmm", "no winload leaf\n");
            } else {
                usLogW("gmm", "winload leaf ambiguous\n");
            }
        }

        if (kernel != NULL && !gReportedKernel) {
            gReportedKernel = true;
            usLogV("gmm", "ntoskrnl found at " US_VALUE("%#llx") "\n",
                   (unsigned long long)(uintptr_t)kernel->base);
        }
    }

    /*
     * Whatever the scan has just brought in may be what a patch was waiting
     * for, so the table is run again here rather than only once at the end.
     * It comes after the report, because a patch is usually what a check
     * stops on and everything that describes the run has to be out by then
     *
     * On our own stack, like the scan: this hook is reached from inside the
     * boot manager, and applying patches is not a small frame
     */
    usStackRunOn(gSession->bootStackTop, applyPatchesOnOwnStack, NULL);

    usConsoleProgress('3');

    /*
     * The kernel is what this is really waiting for; the loader is reported on
     * the way so that a boot which never produces one still says how far it
     * got
     */
    if (usRegistryGet(&gSession->registry, UsImageNtoskrnl) != NULL
        || ++gScans >= US_GMM_MAX_SCANS) {
        usServiceHookRemove(&gHook);
        gDone = true;
        usLogI("gmm", "done\n");
        usPatchReportPending(gSession);

        /* Everything the plan is built from is in memory now */
        if (usRegistryGet(&gSession->registry, UsImageNtoskrnl) != NULL) {
            usWorkCollect(gSession);
        }
    }
    usConsoleProgress('4');

    return status;
}

bool usGmmHookInstall(UsSession *session) {
    gSession = session;
    gOriginal = BS->GetMemoryMap;
    gDone = false;
    gScans = 0;
    gReportedWinload = false;
    gReportedKernel = false;

    if (!usServiceHookInstall(&gHook, (void *const *)&BS->GetMemoryMap, (void *)gmmHook)) {
        gOriginal = NULL;
        return false;
    }
    return true;
}
