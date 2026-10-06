/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Runtime configuration
 *
 * The driver takes all of its tunables from a TOML document, so a build never
 * needs a flag embedded in it: change the file on the boot volume and the next
 * boot behaves differently. See clean-room/design.md for the format
 *
 * Parsing is pure: this file only ever sees a buffer. Finding and reading that
 * buffer is the bootPhase's job, in uefi/config.c
 *
 * Ownership: a parsed config owns the document buffer and the parsed table.
 * The strings reachable through UsPatch and the debug accessors point into
 * that storage and stay valid until usConfigFree
 */

#ifndef US_CFG_H
#define US_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/log.h"
#include "toml.h"

/*
 * What the [uart] table said the port is
 *
 * UsCfgUARTACPIFind is the default: the firmware described its own console
 * somewhere, and looking for it is better than being told, because the
 * description travels with the machine and a copy of it in a file does not
 */
typedef enum UsCfgUARTKind_e {
    UsCfgUARTACPIFind = 0,
    UsCfgUARTPl011,
    UsCfgUARTUart8250,
    UsCfgUARTOff,
} UsCfgUARTKind;

/* And which table it described it in */
typedef enum UsCfgUARTTable_e {
    UsCfgUARTTableSPCR = 0,
    UsCfgUARTTableDBG2,
    UsCfgUARTTableDSDT,
} UsCfgUARTTable;

/*
 * One entry of the debug patch table. The bootPhase applies these directly,
 * without going through the scanner, which makes them useful for planting a
 * breakpoint in a known place while bringing the driver up
 */
typedef struct UsPatch_t {
    const char *target;  /* image name, e.g. "ntoskrnl" */
    uint32_t    rva;     /* offset from the image base */
    uint64_t    value;   /* written little endian, truncated to width */
    uint8_t     width;   /* 1, 2, 4 or 8 */
    const char *tag;     /* optional, NULL when absent */
} UsPatch;

typedef struct UsConfig_t {
    char         *text;   /* owned copy, the document's storage */
    toml_table_t *root;   /* owned, NULL when the config is empty */
    UsLogLevel    logLevel;

    /*
     * Whether the licences this binary carries are printed at boot, from
     * [log]. On unless turned off: a driver is distributed as a binary and
     * nothing else, so the notices that go with it have nowhere to live but
     * inside it, and a machine that cannot be asked for them is a machine
     * that does not carry them
     */
    bool          showLicenses;

    /*
     * What to do about RCpc loads, from [ldapr]. Both are on unless turned
     * off, and turning either off costs exceptions rather than correctness:
     * the exception path carries out what the replacement did not
     */
    bool          imageInplaceRewrite;
    bool          el0InplaceRewrite;

    /*
     * Whether the handler keeps a count of where traps are taken. It costs a
     * lookup on every entry, so it is off unless asked for
     */
    bool          statsEnabled;

    /*
     * Directory of patch list files, relative to the volume the configuration
     * was read from. Points into the document storage. An empty string turns
     * the feature off; the volume root is refused where it is used
     */
    const char   *patchDir;

    /*
     * Where the kernel's own page table base lives, as an RVA, for the builds
     * that state it. It is a property of the build rather than of any list,
     * and it is never trusted: the handler checks it against the hardware's
     * own translation before using it, and carries on without it if it does
     * not hold
     */
    bool          hasDescriptorBase;
    uint32_t      descriptorBaseRVA;

    /*
     * Where to report from, from the [uart] table: the driver, the payload
     * and anything reporting later all need it. Leaving it out is how a
     * machine says it wants no serial output at all
     */
    bool          hasUART;
    /*
     * A type, and not a pointer: the driver can be asked to find the port
     * rather than be told where it is. A machine whose tables describe a
     * console needs no address in its configuration at all
     */
    int           uartKind;    /* UsCfgUART* */
    const char   *uartType;    /* the word the file used, for the report */
    uint64_t      uartBase;    /* only when the configuration names one */
    uint32_t      uartWidth;   /* bits per access: 8 or 32 */
    /*
     * Which table to find it in, and for the DSDT the device to look for.
     * Both point into the document storage
     */
    int           uartTable;   /* UsCfgUARTTable* */
    const char   *uartPath;    /* NULL when the file named none */
    /*
     * The line rate and the clock that drives it. Both have to be given
     * before the port is programmed: a boot that says nothing about either
     * leaves the dividers as the firmware set them, which is the answer for
     * every machine whose own tables describe a console, and for every port
     * whose clock nothing in ACPI states
     */
    uint64_t      uartClock;
    uint32_t      uartBaud;
    /*
     * Whether the output carries the colour escapes the screen and a terminal
     * understand. On unless turned off: a log read back on a terminal is
     * where the colours are worth anything, and a reader that does not
     * understand the escapes ignores them
     */
    bool          uartColour;

    /*
     * The switches that take parts of the mechanism out of the way, from
     * [debug]. Each is named for what it stops rather than for what it does,
     * and every one is false unless set: the mechanism is whole in a file
     * that does not mention them, and a key here can only ever be a remark
     * about a machine being brought up
     */
    bool          notArmVectors;
    bool          notArmVectorsEl1t;
    bool          notArmHandover;
    bool          notVamap;

    /*
     * Whether to treat the SPx vector's stack as one the stub may push on.
     * This one is not a "not": the stub pushing there is the extra thing, so
     * false - the ordinary answer - is the default and the name reads the
     * same way round as the rest of this table
     */
    bool          spxStack;

    /*
     * Whether to take the vectors over as soon as a kernel is loaded, rather
     * than when the plan that says which image the kernel is has been built
     *
     * The one key here that is not a "not", and the one that turns something
     * on rather than off. Where a kernel's vector table is, and what room in
     * its image the stubs may use, is normally answered by the plan - which
     * is built when a kernel is found in the memory map, and only a boot
     * manager asking for that map produces one. The stand-in kernel the fast
     * test runs is loaded by the shell and never produces one, so with this
     * off the test only ever exercises the replacement, and carrying out an
     * instruction that was not replaced is not covered at all
     *
     * Off by default, and it must stay that way: arming on a name alone would
     * take over the vectors of whatever image happened to be called that
     */
    bool          armOnLoad;

    UsPatch      *patches;    /* owned array */
    uint32_t      patchCount;
} UsConfig;

/*
 * Parses a document. Any problem is reported into err and yields NULL: the
 * caller must not press on with a half applied configuration
 *
 * An empty document is valid and yields the built in defaults, which is how
 * "no configuration file present" is expressed
 */
UsConfig *usConfigParse(const char *text, size_t len, char *err, size_t errLen);
void usConfigFree(UsConfig *c);

/*
 * The name a level is spelled with, so a dump of the configuration can print
 * what it read the way the file would have said it
 */
const char *usConfigLogLevelName(UsLogLevel level);

#endif
