/*
 * Placing the payload, see payload.h.
 */

#include <uefi.h>

#include "common/layout.h"
#include "payload/payload.h"
#include "payload_blob.h"
#include "uefi/src/cache.h"
#include "uefi/src/console.h"
#include "uefi/src/payload_place.h"
#include "uefi/src/session.h"

/*
 * The payload is a blob, not an object this image is linked against, so it is
 * entered through pointers computed from where it was placed. That is also
 * the honest model: nothing about the payload is known at link time on the
 * driver's side, including whether it will be reachable from where it runs.
 */
typedef void (*UsSelfTestFn)(void);

/*
 * Executable, and a class the OS keeps. Runtime services code is the only
 * memory the firmware offers with both properties, and it is the reason the
 * payload does not live in the data pool: that one is mapped non-executable,
 * correctly, because it holds stacks.
 */
#define US_PAYLOAD_MEMORY_TYPE EfiRuntimeServicesCode

static void writeConfig(const UsPayloadPlace *place, const UsSession *session) {
    UsPayloadConfig *cfg = (UsPayloadConfig *)(uintptr_t)place->configVa;

    cfg->uartBase = US_UART_BASE;
    for (uint32_t i = 0; i < US_MAX_CPUS; i++) {
        cfg->stackTop[i] = session->pool->stackTop[i];
    }
    cfg->selfVa = place->baseVa;
    /*
     * Nothing to forward to yet. A zero here means an exception the payload
     * will not claim stops rather than being handed on, which is the right
     * answer until there is somewhere to hand it to.
     */
    cfg->forwardTarget = 0;
}

bool usPayloadPlace(UsSession *session, UsPayloadPlace *out) {
    efi_physical_address_t pa = 0;
    uint8_t *dst;
    efi_status_t status;

    /* A second call leaves the first placement alone: the addresses in it may
     * already have been handed out. */
    if (session->payloadPlaced) {
        *out = session->payloadPlace;
        return true;
    }

    status = BS->AllocatePages(AllocateAnyPages, US_PAYLOAD_MEMORY_TYPE,
                               (uintn_t)((US_PAYLOAD_BYTES + US_PAGE_SIZE - 1) / US_PAGE_SIZE),
                               &pa);
    if (EFI_ERROR(status) || pa == 0) {
        return false;
    }

    dst = (uint8_t *)(uintptr_t)pa;
    memcpy(dst, kPayloadBlob, US_PAYLOAD_BYTES);

    /*
     * The copy just became instructions, and on AArch64 a store does not
     * reach the instruction fetcher by itself.
     */
    usCacheFlushRange(dst, US_PAYLOAD_BYTES);

    out->basePa = (uint64_t)pa;
    out->baseVa = (uint64_t)pa;
    out->bytes = US_PAYLOAD_BYTES;
    out->entryVa = out->baseVa + US_PAYLOAD_ENTRY_OFFSET;
    out->configVa = out->baseVa + US_PAYLOAD_CONFIG_OFFSET;

    writeConfig(out, session);

    session->payloadPlace = *out;
    session->payloadPlaced = true;
    return true;
}

void usPayloadReport(const UsSession *session) {
    const UsPayloadPlace *place = &session->payloadPlace;
    UsSelfTestFn selfTest;

    if (!session->payloadPlaced) {
        usConsolePuts("payload: not placed\n");
        return;
    }

    usConsolePuts("payload: at ");
    usConsolePutHex(place->baseVa);
    usConsolePuts(" bytes=");
    usConsolePutDec(place->bytes);
    usConsolePuts(" entry=");
    usConsolePutHex(place->entryVa);
    usConsolePuts(" config=");
    usConsolePutHex(place->configVa);
    usConsolePuts("\n");

    /*
     * Called rather than branched to. The entry is reached through a vector
     * slot and cannot be entered from here, but the C part can, and it reports
     * through the configuration block the boot wrote: a silent answer means
     * the block was not written where the payload looks for it, which is the
     * one thing about placement that is easy to get wrong.
     */
    selfTest = (UsSelfTestFn)(uintptr_t)(place->baseVa + US_PAYLOAD_SELFTEST_OFFSET);
    selfTest();

    /* Printed last, on its own line: the deployment checks stop the machine
     * the moment they see a marker, and stopping on the payload's own output
     * would cut it in half. */
    usConsolePuts("US-M6-PAYLOAD\n");
}
