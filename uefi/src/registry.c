/*
 * The image registry, see registry.h for the contract
 */

#include <string.h>

#include "uefi/src/registry.h"

void usRegistryInit(UsRegistry *reg) {
    memset(reg, 0, sizeof(*reg));
}

bool usRegistryAddBootmgfw(UsRegistry *reg, const void *base, size_t size) {
    /* The firmware may load the same image twice; the second registration
     * would overwrite a view that is already being referred to, so keep the
     * first */
    if (reg->bootmgfwPresent) {
        return false;
    }
    if (!usImageInitMemory(&reg->bootmgfw, base, size)) {
        return false;
    }
    reg->bootmgfwPresent = true;
    return true;
}

/* The scan itself lives in core/pe.c, where it can be tested without a
 * firmware; this only decides what to do with what it finds */
static UsImageVisit recordImage(const UsImage *img, UsImageKind kind, void *ctx) {
    UsRegistry *reg = ctx;

    if (kind == UsImageNtoskrnl && !reg->ntoskrnlPresent) {
        reg->ntoskrnl = *img;
        reg->ntoskrnlPresent = true;
    } else if (kind == UsImageWinload && !reg->winloadPresent) {
        reg->winload = *img;
        reg->winloadPresent = true;
    }

    /* Both are wanted, so the scan only stops once it cannot help */
    return UsImageVisitContinue;
}

int usRegistryScanRegion(UsRegistry *reg, const void *base, size_t size) {
    int before = usRegistryCount(reg);

    usPeScanRegion(base, size, recordImage, reg);
    return usRegistryCount(reg) - before;
}

UsImage *usRegistryGet(UsRegistry *reg, UsImageKind kind) {
    switch (kind) {
    case UsImageBootmgfw:
        return reg->bootmgfwPresent ? &reg->bootmgfw : NULL;
    case UsImageWinload:
        return reg->winloadPresent ? &reg->winload : NULL;
    case UsImageNtoskrnl:
        return reg->ntoskrnlPresent ? &reg->ntoskrnl : NULL;
    default:
        return NULL;
    }
}

UsImage *usRegistryByName(UsRegistry *reg, const char *name) {
    if (name == NULL) {
        return NULL;
    }
    /* A table written by hand will use the spellings from the documentation,
     * which are the short ones */
    if (strcmp(name, "bootmgfw") == 0) {
        return usRegistryGet(reg, UsImageBootmgfw);
    }
    if (strcmp(name, "winload") == 0) {
        return usRegistryGet(reg, UsImageWinload);
    }
    if (strcmp(name, "ntoskrnl") == 0) {
        return usRegistryGet(reg, UsImageNtoskrnl);
    }
    return NULL;
}

int usRegistryCount(const UsRegistry *reg) {
    return (reg->bootmgfwPresent ? 1 : 0) + (reg->winloadPresent ? 1 : 0)
           + (reg->ntoskrnlPresent ? 1 : 0);
}
