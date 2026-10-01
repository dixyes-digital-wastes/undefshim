/*
 * The memory map hook, see gmm_hook.h for why this is the moment to act.
 */

#include <uefi.h>

#include "common/layout.h"
#include "core/pe.h"
#include "core/scan.h"
#include "uefi/src/console.h"
#include "uefi/src/gmm_hook.h"
#include "uefi/src/service_hook.h"
#include "uefi/src/stack.h"

static UsSession *gSession;
static UsServiceHook gHook;
static efi_get_memory_map_t gOriginal;
static bool gDone;

/*
 * Only loader memory is searched. Images are loaded into these two types and
 * nowhere else, and restricting the search is what keeps it from walking
 * hundreds of megabytes of conventional memory on every call.
 */
static bool isImageMemory(uint32_t type) {
    return type == EfiLoaderCode || type == EfiLoaderData;
}

/*
 * Walks the map and hands every loader region to the scanner. The map is a
 * packed array of records whose stride the firmware chooses, so the only way
 * to move through it is by DescriptorSize.
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
 * What runs on our own stack: the scan, and everything it drags in.
 *
 * The image scanner keeps an image header on the stack, and that header holds
 * the section table, so it is a few kilobytes. That is fine on a stack we
 * allocated and fatal on one we did not: bootmgfw calls GetMemoryMap deep in
 * its own work, and the caller's frame here is not ours to spend. Running the
 * scan through this indirection is what makes the hook safe.
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

static efi_status_t EFIAPI gmmHook(uintn_t *memoryMapSize, efi_memory_descriptor_t *memoryMap,
                                   uintn_t *mapKey, uintn_t *descriptorSize,
                                   uint32_t *descriptorVersion) {
    efi_status_t status;
    ScanRequest req;

    /*
     * Ours does nothing but observe, so the firmware's is called first and its
     * answer is returned unchanged. Reporting anything else would break the
     * caller, and the caller at this point is the boot manager.
     */
    status = gOriginal(memoryMapSize, memoryMap, mapKey, descriptorSize, descriptorVersion);

    /*
     * The first call of a pair is usually a size probe that fails with
     * EFI_BUFFER_TOO_SMALL and no usable map. Scanning a map that was not
     * filled in would read whatever the caller's buffer happened to hold.
     */
    if (EFI_ERROR(status) || memoryMap == NULL || descriptorSize == NULL) {
        return status;
    }

    if (usRegistryGet(&gSession->registry, UsImageNtoskrnl) != NULL
        && usRegistryGet(&gSession->registry, UsImageWinload) != NULL) {
        return status;
    }

    usConsolePutc('g');

    req.map = memoryMap;
    req.mapSize = *memoryMapSize;
    req.descSize = *descriptorSize;
    req.added = 0;
    usConsolePuts("1");
    usStackRunOn(gSession->bootStackTop, scanOnOwnStack, &req);
    usConsolePuts("2");

    {
        UsImage *winload = usRegistryGet(&gSession->registry, UsImageWinload);

        if (winload != NULL) {
            usConsolePuts("\ngmm: winload found\n");

            UsLeafSite leaf = usLocateTransferLeaf(winload);
            if (leaf.found) {
                usConsolePuts("gmm: winload leaf found\n");
            } else if (leaf.matches == 0) {
                usConsolePuts("gmm: no winload leaf\n");
            } else {
                usConsolePuts("gmm: winload leaf ambiguous\n");
            }
        }
        if (usRegistryGet(&gSession->registry, UsImageNtoskrnl) != NULL) {
            usConsolePuts("gmm: ntoskrnl found\n");
        }
    }
    usConsolePuts("3");

    /*
     * The kernel has not been loaded yet when this first fires, so the hook
     * stays until the loader is in memory, which is as far as this stage can
     * get. Later calls, after the loader has run, bring the kernel too.
     */
    if (usRegistryGet(&gSession->registry, UsImageWinload) != NULL) {
        usServiceHookRemove(&gHook);
        gDone = true;
        usConsolePuts("\ngmm: done\n");
    }
    usConsolePuts("4");

    return status;
}

bool usGmmHookInstall(UsSession *session) {
    gSession = session;
    gOriginal = BS->GetMemoryMap;
    gDone = false;

    if (!usServiceHookInstall(&gHook, (void *const *)&BS->GetMemoryMap, (void *)gmmHook)) {
        gOriginal = NULL;
        return false;
    }
    return true;
}

bool usGmmHookDone(void) {
    return gDone;
}
