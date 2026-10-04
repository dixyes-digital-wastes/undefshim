/*
 * Replacing an entry in the boot services table, see service_hook.h
 */

#include <uefi.h>

#include "uefi/src/cache.h"
#include "uefi/src/service_hook.h"

/*
 * The boot services table carries a CRC32 over its header, which the firmware
 * may verify. Changing an entry means recomputing it, or the next component
 * that checks will reject the table
 */
static void refreshBootServicesCrc(void) {
    efi_boot_services_t *bs = BS;
    const uint8_t *p = (const uint8_t *)bs;
    size_t len = bs->Hdr.HeaderSize;
    uint32_t sum;

    if (len < 12) {
        return;
    }
    bs->Hdr.CRC32 = 0;

    /* CRC32 as the firmware computes it: reflected, polynomial 0xEDB88320,
     * over the header with the checksum field zeroed */
    sum = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        sum ^= p[i];
        for (int b = 0; b < 8; b++) {
            sum = (sum >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(sum & 1)));
        }
    }
    bs->Hdr.CRC32 = sum ^ 0xFFFFFFFFU;
}

bool usServiceHookInstall(UsServiceHook *hook, void *const *slot, void *replacement) {
    efi_boot_services_t *bs = BS;

    if (hook == NULL || slot == NULL || !bs->Hdr.HeaderSize) {
        return false;
    }

    hook->slot = (void **)(uintptr_t)slot;
    hook->original = *hook->slot;
    hook->replacement = replacement;
    hook->installed = false;

    *hook->slot = replacement;
    refreshBootServicesCrc();

    /* The entry is code the firmware will fetch, so the write has to be made
     * visible beyond this core before anything calls it */
    usCacheFlushRange(hook->slot, sizeof(*hook->slot));
    usCacheSync();

    /* Read back: a write that did not take is otherwise indistinguishable from
     * a hook that is never called, and finding that out later is much worse */
    hook->installed = (*hook->slot == replacement);
    return hook->installed;
}

void usServiceHookRemove(UsServiceHook *hook) {
    if (hook == NULL || !hook->installed) {
        return;
    }
    *hook->slot = hook->original;
    refreshBootServicesCrc();
    usCacheFlushRange(hook->slot, sizeof(*hook->slot));
    usCacheSync();
    hook->installed = false;
}
