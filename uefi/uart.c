/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Finding the console, see uart.h
 */

#include <uefi.h>

#include "core/cfg.h"
#include "uefi/acpi.h"
#include "uefi/console.h"
#include "uefi/session.h"
#include "uefi/stack.h"
#include "uefi/uart.h"

/*
 * What the search found, filled in on our own stack and read back here
 */
typedef struct ConsoleOpen_t {
    const UsConfig *cfg;
    UsUARTKind      kind;
    uint64_t        base;
    uint32_t        width;
    bool            found;
} ConsoleOpen;

static UsACPIUARTTable tableOf(int which) {
    switch (which) {
    case UsCfgUARTTableDBG2:
        return UsACPIUARTTableDBG2;
    case UsCfgUARTTableDSDT:
        return UsACPIUARTTableDSDT;
    default:
        return UsACPIUARTTableSPCR;
    }
}

static UsUARTKind kindOf(UsACPIUARTKind kind) {
    switch (kind) {
    case UsACPIUARTPl011:
        return UsUARTPL011;
    case UsACPIUART16550:
        return UsUART8250;
    default:
        return UsUARTOff;
    }
}

/*
 * The search itself, which is what runs on our own stack: looking a table up
 * by signature is small, but the DSDT is a whole namespace and this is
 * reached from the firmware's own entry point
 */
static void searchOnOwnStack(void *arg) {
    ConsoleOpen *open = arg;
    UsACPIUART found;

    found = usACPIProbeUART(tableOf(open->cfg->uartTable), open->cfg->uartPath);
    open->kind = kindOf(found.kind);
    open->base = found.base;
    /* The table's own access size is what its author wrote, and the width in
     * the configuration is what the board wants: the file has the last word,
     * because it is the one that was written for this machine */
    open->width = open->cfg->uartWidth != 0 ? open->cfg->uartWidth : found.width;
    open->found = open->kind != UsUARTOff && open->base != 0;
}

void usConsoleOpen(const UsConfig *cfg, UsSession *session) {
    ConsoleOpen open = {
        .cfg = cfg,
        .kind = UsUARTOff,
        .base = 0,
        .width = 32,
        .found = false,
    };

    if (cfg->uartKind == UsCfgUARTOff) {
        return;
    }
    if (cfg->hasUART) {
        /* Named outright: the address is what the file says and the kind was
         * settled when the file was read */
        open.kind = cfg->uartKind == UsCfgUARTUart8250 ? UsUART8250 : UsUARTPL011;
        open.base = cfg->uartBase;
        open.width = cfg->uartWidth;
        open.found = true;
    } else {
        usStackRunOn(session->bootStackTop, searchOnOwnStack, &open);
    }
    if (!open.found) {
        return;
    }
    usConsoleUse(open.kind, open.base, open.width, cfg->uartClock, cfg->uartBaud);
    /* Recorded for the payload, which reports through the same port and needs
     * where it turned out to be rather than what the file said about it */
    session->uartOpen = true;
    session->uartKind = open.kind;
    session->uartBase = open.base;
    session->uartWidth = open.width;
    usLogV("uart", "found " US_VALUE("%s") " at " US_VALUE("%#llx")
           " width=" US_VALUE("%u") "\n",
           open.kind == UsUARTPL011 ? "pl011" : "uart8250",
           (unsigned long long)open.base, (unsigned)open.width);
}
