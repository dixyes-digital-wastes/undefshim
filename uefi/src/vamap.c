/*
 * Catching the address change, see vamap.h.
 *
 * The firmware's SetVirtualAddressMap is what carries the change, and calling
 * ConvertPointer from inside that call is the only way to be told where the
 * runtime memory went. Everything here exists to get at that one moment.
 */

#include <uefi.h>

#include "payload/vamap.h"
#include "payload_blob.h"
#include "uefi/src/console.h"
#include "uefi/src/session.h"
#include "uefi/src/vamap.h"

/*
 * Both the function that is entered and the record it writes live in the
 * payload, and for the same reason: the change happens after the boot
 * services are gone, and by then the driver's own memory has been given back
 * and may be in use for something else. The blob is memory the OS keeps, and
 * the driver knows where it was placed, so the offset is all it needs.
 */
static UsVaMapRecord *record(UsSession *session) {
    if (!session->payloadPlaced) {
        return NULL;
    }
    return (UsVaMapRecord *)(uintptr_t)
        (session->payloadPlace.baseVa + US_PAYLOAD_VAMAP_OFFSET);
}

bool usVaMapArm(UsSession *session) {
    UsVaMapRecord *r = record(session);
    efi_event_t change;
    void *hook;
    void *notify;

    if (r == NULL) {
        usConsolePuts("vamap: no payload to write the answer to\n");
        return false;
    }

    r->poolBefore = (uint64_t)(uintptr_t)session->pool;
    r->payloadBefore = session->payloadPlace.baseVa;
    r->rt = (uint64_t)(uintptr_t)RT;
    r->convertPointer = (uint64_t)(uintptr_t)RT->ConvertPointer;

    /*
     * The notification has to be entered in the middle of the switch and is
     * therefore in the blob as well, for the same reason as the hook above,
     * and it is registered here because the services that create it are gone
     * by the time the switch happens.
     */
    notify = (void *)(uintptr_t)(session->payloadPlace.baseVa + US_PAYLOAD_VAMAPNOTIFY_OFFSET);
    if (EFI_ERROR(BS->CreateEvent(EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE, TPL_NOTIFY,
                                  (efi_event_notify_t)notify, NULL, &change))) {
        usConsolePuts("vamap: no notification\n");
        return false;
    }

    /*
     * A function of the driver's own would be the obvious thing to put here,
     * and is what the first attempt did. It is also the mistake: this is
     * entered once the loader owns the machine, and the driver's pages are
     * free by then. The stub is in the blob for that reason alone.
     */
    hook = (void *)(uintptr_t)(session->payloadPlace.baseVa + US_PAYLOAD_VAMAPHOOK_OFFSET);
    r->svmOriginal = (uint64_t)(uintptr_t)RT->SetVirtualAddressMap;
    RT->SetVirtualAddressMap = (efi_set_virtual_address_map_t)hook;

    usConsolePuts("vamap: record=");
    usConsolePutHex((uint64_t)(uintptr_t)r);
    usConsolePuts(" original=");
    usConsolePutHex(r->svmOriginal);
    usConsolePuts(" hook=");
    usConsolePutHex((uint64_t)(uintptr_t)hook);
    usConsolePuts("\n");
    return true;
}
