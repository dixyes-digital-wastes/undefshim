/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Placing the payload, see payload.h
 */

#include <uefi.h>

/* Kept in step with payload/uart.h, which the driver does not include */
#define US_PAYLOAD_UART_PL011 1U
#define US_PAYLOAD_UART_8250 2U

#include <stddef.h>

#include "common/layout.h"
#include "core/stackgen.h"
#include "payload/payload.h"
#include "payload_blob.h"
#include "uefi/cache.h"
#include "uefi/console.h"
#include "uefi/payload_place.h"
#include "uefi/session.h"

/*
 * The payload is a blob, not an object this image is linked against, so it is
 * entered through pointers computed from where it was placed. That is also
 * the honest model: nothing about the payload is known at link time on the
 * driver's side, including whether it will be reachable from where it runs
 */
typedef void (*UsSelfTestFn)(void);

/*
 * Executable, and a class the OS keeps. Runtime services code is the only
 * memory the firmware offers with both properties, and it is the reason the
 * payload does not live in the data pool: that one is mapped non-executable,
 * correctly, because it holds stacks
 */
#define US_PAYLOAD_MEMORY_TYPE EfiRuntimeServicesCode

static void writeConfig(const UsPayloadPlace *place, const UsSession *session) {
    UsPayloadConfig *cfg = (UsPayloadConfig *)(uintptr_t)place->configVA;

    /*
     * Where to report from, as the console ended up rather than as the file
     * put it: the configuration may only say that the firmware knows, and
     * what the firmware said is worked out while the console is being
     * opened. A base of zero is a machine with no port at all, and the
     * payload stays silent for it
     */
    cfg->uartBase = session->uartOpen ? session->uartBase : 0U;
    cfg->uartKind = !session->uartOpen
                        ? 0U
                        : (session->uartKind == UsUARTPL011 ? US_PAYLOAD_UART_PL011
                                                            : US_PAYLOAD_UART_8250);
    cfg->uartWidth = session->uartOpen ? session->uartWidth : 32U;
    for (uint32_t i = 0; i < US_MAX_CPUS; i++) {
        cfg->stackTop[i] = session->pool->stackTop[i];
    }
    cfg->selfVA = place->baseVA;
    cfg->selfPA = place->basePA;
    cfg->selfBytes = place->bytes;
    cfg->poolPA = session->pool->selfPA;
    cfg->entryOffset = US_PAYLOAD_ENTRY_OFFSET;

    /* The trace of what happened goes here, and it has to survive the address
     * space being rebuilt, which the pool does and the payload does not */
    cfg->poolBase = (uint64_t)(uintptr_t)session->pool;
    session->pool->entry = (UsPoolEntry){ 0 };

    /*
     * Which processor is which. Without this the payload would have to use
     * the low byte of MPIDR_EL1, which is not unique across clusters, and two
     * processors would share a landing pad and a stack
     *
     * A machine the firmware did not describe gets one entry, so there is
     * always at least the processor this ran on and the lookup never comes
     * back empty on hardware that works
     */
    {
        uint64_t count = session->cpus.count;

        for (uint64_t i = 0; i < count; i++) {
            cfg->cpus[i] = (UsPayloadCPU){ .mpidr = session->cpus.mpidr[i], .index = i };
        }
        /* The end of the list, for the entry's lookup */
        cfg->cpus[count] = (UsPayloadCPU){ .mpidr = 0, .index = ~(uint64_t)0 };
        cfg->cpuCount = count;
    }

    /*
     * Silence from the moment the kernel is running
     *
     * The serial port is reachable during boot and is not after the kernel
     * builds its own page tables, where a write to it faults. Whether the
     * payload is entered in that state is not something it can find out by
     * trying, so it is told
     */
    cfg->quiet = 1;
    /*
     * Whether the user-mode replacement can happen at all, not just whether it
     * was asked for
     *
     * Making an instruction's page writable needs the base of the image's
     * descriptor mapping, and that comes from the configuration alone - see
     * UsPayloadStub.descriptorBaseRVA, which the arming leaves at zero when
     * there is none. With no base every attempt is refused, so a payload told
     * to try anyway would take the trap path for every RCpc load in user code
     * and spend, on each one, a stack the kernel owns, to reach an answer it
     * could have been given here
     *
     * The exception path still stands: an RCpc load that is not replaced is
     * carried out by the handler, which is correct and slower, and that is
     * exactly what a machine with no base is
     */
    cfg->el0InPlace = session->config != NULL
                          && session->config->el0InplaceRewrite
                          && session->config->hasDescriptorBase
                      ? 1U
                      : 0U;
    cfg->statsEnabled = session->config != NULL && session->config->statsEnabled ? 1U : 0U;
}

bool usPayloadPlace(UsSession *session, UsPayloadPlace *out) {
    efi_physical_address_t pa = 0;
    uint8_t *dst;
    efi_status_t status;
    uint32_t lookup[US_STACK_LOOKUP_WORDS];

    /* A second call leaves the first placement alone: the addresses in it may
     * already have been handed out */
    if (session->payloadPlaced) {
        *out = session->payloadPlace;
        return true;
    }

    /* Validate the same CPU table used by writeConfig before allocating */
    if (usGenerateStackLookup(session->cpus.mpidr,
                              (uint32_t)session->cpus.count,
                              US_PAYLOAD_STACKLOOKUP_OFFSET,
                              US_PAYLOAD_CONFIG_OFFSET
                                  + offsetof(UsPayloadConfig, stackTop),
                              US_PAYLOAD_READY_OFFSET,
                              US_PAYLOAD_NOSTACK_OFFSET,
                              lookup)
        == 0) {
        return false;
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
     * The entry's bootstrap finds its own stack through generated code: one
     * match branch per processor the firmware described, keyed on the full
     * normalized affinity. Writing it here means the cpu list only has to be
     * right once, at boot, instead of the blob carrying a fixed mapping
     */
    memcpy(dst + US_PAYLOAD_STACKLOOKUP_OFFSET, lookup, sizeof(lookup));

    /*
     * The copy just became instructions, and on AArch64 a store does not
     * reach the instruction fetcher by itself
     */
    usCacheFlushRange(dst, US_PAYLOAD_BYTES);

    out->basePA = (uint64_t)pa;
    out->baseVA = (uint64_t)pa;
    out->bytes = US_PAYLOAD_BYTES;
    out->entryVA = out->baseVA + US_PAYLOAD_ENTRY_OFFSET;
    out->configVA = out->baseVA + US_PAYLOAD_CONFIG_OFFSET;

    writeConfig(out, session);

    session->payloadPlace = *out;
    session->payloadPlaced = true;
    return true;
}

void usPayloadReport(const UsSession *session) {
    const UsPayloadPlace *place = &session->payloadPlace;
    UsSelfTestFn selfTest;

    if (!session->payloadPlaced) {
        usLogE("payload", "not placed\n");
        return;
    }

    usLogV("payload", "at " US_VALUE("%#llx") " bytes=" US_VALUE("%u")
           " entry=" US_VALUE("%#llx") " config=" US_VALUE("%#llx") "\n",
           (unsigned long long)place->baseVA, (unsigned)place->bytes,
           (unsigned long long)place->entryVA,
           (unsigned long long)place->configVA);

    /*
     * Called rather than branched to. The entry is reached through a vector
     * slot and cannot be entered from here, but the C part can, and it reports
     * through the configuration block the boot wrote: a silent answer means
     * the block was not written where the payload looks for it, which is the
     * one thing about placement that is easy to get wrong
     */
    selfTest = (UsSelfTestFn)(uintptr_t)(place->baseVA + US_PAYLOAD_SELFTEST_OFFSET);
    selfTest();

    /* Printed last, on its own line: the deployment checks stop the machine
     * the moment they see a marker, and stopping on the payload's own output
     * would cut it in half */
    usConsoleMilestone("M6 payload placed");
}
