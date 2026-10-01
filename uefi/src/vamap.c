/*
 * Catching the address change, see vamap.h.
 */

#include <uefi.h>

#include "uefi/src/console.h"
#include "uefi/src/vamap.h"

static UsSession *gSession;
static efi_event_t gEvent;

/*
 * Translates one pointer, in place, while the firmware still can.
 *
 * ConvertPointer is only usable inside this notification: it is what the
 * firmware offers for exactly this purpose, and it is gone once the switch is
 * complete. A pointer that is not in runtime memory is left alone by it, so
 * calling it on everything we hold is the safe thing to do.
 */
static void convert(void **address) {
    efi_status_t status;
    void *before = *address;

    if (*address == NULL) {
        return;
    }
    status = RT->ConvertPointer(0, address);
    if (EFI_ERROR(status)) {
        usConsolePuts("vamap: convert refused ");
        usConsolePutHex((uint64_t)(uintptr_t)before);
        usConsolePuts("\n");
    }
}

static void EFIAPI onVirtualAddressChange(efi_event_t event, void *context) {
    (void)event;
    (void)context;

    usConsolePuts("\nUS-VAMAP-ENTER\n");

    if (gSession != NULL) {
        usConsolePuts("vamap: pool ");
        usConsolePutHex((uint64_t)(uintptr_t)gSession->pool);
        usConsolePuts(" payload ");
        usConsolePutHex(gSession->payloadPlace.baseVa);

        /*
         * The pool and the payload are both runtime memory, and both are
         * reached through addresses the driver stores. The payload's own
         * configuration block holds addresses of its own, and it cannot be
         * converted from here: whatever translates it has to be the payload
         * itself, when it next runs. What this does is make sure the driver
         * can still find the payload to do that.
         */
        convert((void **)&gSession->pool);
        convert((void **)&gSession->payloadPlace.baseVa);
        convert((void **)&gSession->payloadPlace.basePa);
        convert((void **)&gSession->payloadPlace.entryVa);
        convert((void **)&gSession->payloadPlace.configVa);

        usConsolePuts(" -> ");
        usConsolePutHex((uint64_t)(uintptr_t)gSession->pool);
        usConsolePuts(" ");
        usConsolePutHex(gSession->payloadPlace.baseVa);
        usConsolePuts("\n");
    }

    usConsolePuts("US-VAMAP-DONE\n");
}

bool usVaMapArm(UsSession *session) {
    gSession = session;

    if (EFI_ERROR(BS->CreateEvent(EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE, TPL_NOTIFY,
                                  onVirtualAddressChange, NULL, &gEvent))) {
        gSession = NULL;
        return false;
    }
    return true;
}
