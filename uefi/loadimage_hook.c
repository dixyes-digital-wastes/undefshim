/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The image loader hook, see loadimage_hook.h for why it exists
 */

#include <uefi.h>

#include "core/cfg.h"
#include "core/pe.h"
#include "uefi/console.h"
#include "uefi/loadimage_hook.h"
#include "uefi/patch.h"
#include "uefi/rewrite.h"
#include "uefi/service_hook.h"
#include "uefi/stack.h"

static UsSession *gSession;
static UsServiceHook gHook;
static efi_image_load_t gOriginal;

/*
 * What runs on our own stack: taking the image apart and recording it
 *
 * Parsing an image header costs a few kilobytes, because the header holds the
 * section table. That is fine where we are and fatal where we are not: this is
 * called from inside the firmware's loader, with bootmgfw above us on the same
 * stack. Getting this wrong does not fail here, it fails later, in the
 * firmware, with a fault at an address that has nothing to do with us
 */
typedef struct RegisterRequest_t {
    efi_loaded_image_protocol_t *image;
    bool recorded;
} RegisterRequest;

static void registerOnOwnStack(void *arg) {
    RegisterRequest *req = arg;

    req->recorded = usRegistryAddBootmgfw(&gSession->registry, req->image->ImageBase,
                                          req->image->ImageSize);
}

/*
 * Replacing the RCpc loads in whatever was just loaded
 *
 * The image is in memory and has not been started: LoadImage has returned and
 * the caller has yet to call StartImage. That is the same window the kernel is
 * rewritten in, and it is the one place where an image can be changed without
 * anything running out of it
 *
 * This is what covers the drivers and libraries that arrive long after the
 * boot, ci.dll among them. Without it they are only covered by the exception
 * path, which needs the vector slot they happen to use and the registers that
 * slot's handler happens to preserve
 */
static void rewriteOnOwnStack(void *arg) {
    RegisterRequest *req = arg;
    UsImage img;

    /* The smallest thing that could hold a header: the parse refuses the
     * rest, and this keeps a firmware structure from being read as one */
    if (req->image->ImageBase == NULL || req->image->ImageSize < 0x1000) {
        return;
    }
    if (!usImageInitMemory(&img, req->image->ImageBase, req->image->ImageSize)) {
        return;
    }
    usRewriteOne(&img);
}

/*
 * Applying the patch table also runs on our own stack, for the same reason the
 * registration does and with less margin: it walks the table, looks images up
 * by name and writes into them, which together is a frame this hook has no
 * room for. Running it here is what keeps the overflow out of the boot
 * manager's stack, where it would surface much later as something unrelated
 */
typedef struct PatchRequest_t {
    int applied;
} PatchRequest;

static void applyPatchesOnOwnStack(void *arg) {
    PatchRequest *req = arg;

    req->applied = usPatchApplyPending(gSession);
}

static efi_status_t EFIAPI loadImageHook(boolean_t bootPolicy, efi_handle_t parent,
                                         efi_device_path_t *path, void *sourceBuffer,
                                         uintn_t sourceSize, efi_handle_t *image) {
    efi_status_t status;
    efi_guid_t lipGUID = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    efi_loaded_image_protocol_t *lip = NULL;

    /*
     * One character in, fixed strings out, and the real work on our own stack:
     * this runs on the caller's, and the caller may be anything
     */
    usConsoleProgress('h');

    status = gOriginal(bootPolicy, parent, path, sourceBuffer, sourceSize, image);
    if (EFI_ERROR(status) || image == NULL || *image == NULL) {
        usLogE("loadimage", "original failed\n");
        return status;
    }

    /*
     * bootmgfw is whatever the firmware was asked to start; the registry
     * records it without recognising it by content, because content
     * identification is only needed for the stages that arrive without a
     * protocol attached
     */
    if (EFI_ERROR(BS->HandleProtocol(*image, &lipGUID, (void **)&lip)) || lip == NULL
        || lip->ImageBase == NULL || lip->ImageSize == 0) {
        usLogE("loadimage", "no loaded image\n");
        return status;
    }

    {
        RegisterRequest req = { .image = lip, .recorded = false };

        usStackRunOn(gSession->bootStackTop, registerOnOwnStack, &req);
        /*
         * And its instructions replaced, before anything runs out of it -
         * unless the configuration says not to. That key reads "replace the
         * RCpc loads in these images before either runs", and this is the
         * moment it is talking about: leaving it out of here made the key a
         * thing that only had an effect on a machine that also reached the
         * memory map scan, and a machine that did not went on replacing
         * everything while the file said it did not
         */
        if (gSession->imageInplaceRewrite) {
            usStackRunOn(gSession->bootStackTop, rewriteOnOwnStack, &req);
        }
        usLog(req.recorded ? UsLogInfo : UsLogVerbose, "loadimage",
              "%s at " US_VALUE("%#llx") "\n",
              req.recorded ? "registered" : "seen",
              (unsigned long long)(uintptr_t)lip->ImageBase);
    }
    /* A patch aimed at this image can go in now, while the loader is still
     * holding it and before anything runs it. On our own stack: see
     * applyPatchesOnOwnStack */
    {
        PatchRequest preq = { .applied = 0 };

        usStackRunOn(gSession->bootStackTop, applyPatchesOnOwnStack, &preq);
    }

    return status;
}

bool usLoadImageHookInstall(UsSession *session) {
    gSession = session;
    gOriginal = BS->LoadImage;

    if (!usServiceHookInstall(&gHook, (void *const *)&BS->LoadImage, (void *)loadImageHook)) {
        gOriginal = NULL;
        return false;
    }
    return true;
}
