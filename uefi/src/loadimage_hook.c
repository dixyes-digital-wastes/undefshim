/*
 * The image loader hook, see loadimage_hook.h for why it exists.
 */

#include <uefi.h>

#include "core/pe.h"
#include "uefi/src/console.h"
#include "uefi/src/loadimage_hook.h"
#include "uefi/src/service_hook.h"
#include "uefi/src/stack.h"

static UsSession *gSession;
static UsServiceHook gHook;
static efi_image_load_t gOriginal;

/*
 * What runs on our own stack: taking the image apart and recording it.
 *
 * Parsing an image header costs a few kilobytes, because the header holds the
 * section table. That is fine where we are and fatal where we are not: this is
 * called from inside the firmware's loader, with bootmgfw above us on the same
 * stack. Getting this wrong does not fail here, it fails later, in the
 * firmware, with a fault at an address that has nothing to do with us.
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

static efi_status_t EFIAPI loadImageHook(boolean_t bootPolicy, efi_handle_t parent,
                                         efi_device_path_t *path, void *sourceBuffer,
                                         uintn_t sourceSize, efi_handle_t *image) {
    efi_status_t status;
    efi_guid_t lipGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    efi_loaded_image_protocol_t *lip = NULL;

    /*
     * One character in, fixed strings out, and the real work on our own stack:
     * this runs on the caller's, and the caller may be anything.
     */
    usConsolePutc('h');

    status = gOriginal(bootPolicy, parent, path, sourceBuffer, sourceSize, image);
    if (EFI_ERROR(status) || image == NULL || *image == NULL) {
        usConsolePuts("\nloadimage: original failed\n");
        return status;
    }

    /*
     * bootmgfw is whatever the firmware was asked to start; the registry
     * records it without recognising it by content, because content
     * identification is only needed for the stages that arrive without a
     * protocol attached.
     */
    if (EFI_ERROR(BS->HandleProtocol(*image, &lipGuid, (void **)&lip)) || lip == NULL
        || lip->ImageBase == NULL || lip->ImageSize == 0) {
        usConsolePuts("\nloadimage: no loaded image\n");
        return status;
    }

    {
        RegisterRequest req = { .image = lip, .recorded = false };

        usStackRunOn(gSession->bootStackTop, registerOnOwnStack, &req);
        usConsolePuts(req.recorded ? "\nloadimage: registered at " : "\nloadimage: seen at ");
        usConsolePutHex((uint64_t)(uintptr_t)lip->ImageBase);
        usConsolePuts("\n");
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

void usLoadImageHookRemove(void) {
    usServiceHookRemove(&gHook);
}
