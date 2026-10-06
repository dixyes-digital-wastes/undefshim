/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Runtime configuration, see cfg.h for the contract
 */

#include <stdlib.h>
#include <string.h>

#include "core/cfg.h"

#define US_CFG_MAX_TEXT (64 * 1024)

static void setErr(char *err, size_t errLen, const char *msg) {
    if (err == NULL || errLen == 0) {
        return;
    }
    size_t i = 0;
    while (msg[i] != '\0' && i + 1 < errLen) {
        err[i] = msg[i];
        i++;
    }
    err[i] = '\0';
}

static void setErrKey(char *err, size_t errLen, const char *what, const char *key) {
    char buf[192];
    size_t i = 0;

    for (const char *p = what; *p != '\0' && i + 1 < sizeof(buf); p++) {
        buf[i++] = *p;
    }
    for (const char *p = key; *p != '\0' && i + 1 < sizeof(buf); p++) {
        buf[i++] = *p;
    }
    buf[i] = '\0';
    setErr(err, errLen, buf);
}

/* Presence test: non-NULL for every value kind, including ones the typed
 * accessors cannot represent */
static bool sameStr(const char *text, int length, const char *wanted) {
    int i = 0;

    for (; i < length && wanted[i] != '\0'; i++) {
        if (text[i] != wanted[i]) {
            return false;
        }
    }
    return i == length && wanted[i] == '\0';
}

static bool cfgHas(const toml_table_t *t, const char *key) {
    return t != NULL && toml_table_unparsed(t, key) != NULL;
}

/* Each returns true when the key is absent, so callers can keep their default
 * with a plain if and still get a hard failure on a present but unusable
 * value */
static bool cfgBool(const toml_table_t *t, const char *key, bool *out, char *err, size_t errLen) {
    if (!cfgHas(t, key)) {
        return true;
    }
    toml_value_t v = toml_table_bool(t, key);
    if (!v.ok) {
        setErrKey(err, errLen, "not a boolean: ", key);
        return false;
    }
    *out = v.u.b;
    return true;
}

static bool cfgInt(const toml_table_t *t, const char *key, int64_t *out, char *err, size_t errLen) {
    if (!cfgHas(t, key)) {
        return true;
    }
    toml_value_t v = toml_table_int(t, key);
    if (!v.ok) {
        setErrKey(err, errLen, "not an integer: ", key);
        return false;
    }
    *out = v.u.i;
    return true;
}

static bool cfgStr(const toml_table_t *t, const char *key, const char **out, int *outLen, char *err, size_t errLen) {
    int len = 0;
    if (!cfgHas(t, key)) {
        return true;
    }
    const char *v = toml_table_string_ref(t, key, &len);
    if (v == NULL) {
        setErrKey(err, errLen, "not a simple quoted string: ", key);
        return false;
    }
    *out = v;
    *outLen = len;
    return true;
}

/* The string accessors hand back a pointer into the document plus a length, so
 * comparisons have to respect the length: the byte after the value is the
 * closing quote, not a terminator */
static bool refEq(const char *ref, int len, const char *literal) {
    int i = 0;
    for (; i < len && literal[i] != '\0'; i++) {
        if (ref[i] != literal[i]) {
            return false;
        }
    }
    return i == len && literal[i] == '\0';
}

/*
 * The levels by name, which is how the file spells them and how a dump of
 * the file prints them back. One table, so the two cannot drift apart
 */
static const struct {
    const char *name;
    UsLogLevel  level;
} gLevelNames[] = {
    { "off", UsLogOff },
    { "error", UsLogError },
    { "warn", UsLogWarn },
    { "info", UsLogInfo },
    { "verbose", UsLogVerbose },
    { "debug", UsLogDebug },
};

static bool parseLogLevel(const char *s, int len, UsLogLevel *out) {
    for (size_t i = 0; i < sizeof(gLevelNames) / sizeof(gLevelNames[0]); i++) {
        if (refEq(s, len, gLevelNames[i].name)) {
            *out = gLevelNames[i].level;
            return true;
        }
    }
    return false;
}

const char *usConfigLogLevelName(UsLogLevel level) {
    for (size_t i = 0; i < sizeof(gLevelNames) / sizeof(gLevelNames[0]); i++) {
        if (gLevelNames[i].level == level) {
            return gLevelNames[i].name;
        }
    }
    return "?";
}

static bool parseOnePatch(const toml_table_t *t, uint32_t index, UsPatch *out, char *err, size_t errLen) {
    char key[32];
    const char *s;
    int64_t n;
    int len = 0;
    int tagLen = 0;

    snprintf(key, sizeof(key), "patch[%u].target", (unsigned)index);
    if (cfgHas(t, "target")) {
        s = toml_table_string_ref(t, "target", &len);
        if (s == NULL) {
            setErrKey(err, errLen, "not a simple quoted string: ", key);
            return false;
        }
    } else {
        setErrKey(err, errLen, "missing key: ", key);
        return false;
    }
    out->target = s;

    snprintf(key, sizeof(key), "patch[%u].rva", (unsigned)index);
    if (!cfgInt(t, "rva", &n, err, errLen)) {
        return false;
    }
    if (!cfgHas(t, "rva")) {
        setErrKey(err, errLen, "missing key: ", key);
        return false;
    }
    if (n < 0 || n > (int64_t)UINT32_MAX) {
        setErrKey(err, errLen, "out of range: ", key);
        return false;
    }
    out->rva = (uint32_t)n;

    snprintf(key, sizeof(key), "patch[%u].value", (unsigned)index);
    if (!cfgInt(t, "value", &n, err, errLen)) {
        return false;
    }
    if (!cfgHas(t, "value")) {
        setErrKey(err, errLen, "missing key: ", key);
        return false;
    }
    /* Taken as a bit pattern, so a negative literal is the readable way to ask
     * for high bits set */
    out->value = (uint64_t)n;

    snprintf(key, sizeof(key), "patch[%u].width", (unsigned)index);
    if (!cfgInt(t, "width", &n, err, errLen)) {
        return false;
    }
    if (!cfgHas(t, "width")) {
        setErrKey(err, errLen, "missing key: ", key);
        return false;
    }
    if (n != 1 && n != 2 && n != 4 && n != 8) {
        setErrKey(err, errLen, "must be 1, 2, 4 or 8: ", key);
        return false;
    }
    out->width = (uint8_t)n;

    /* The value has to fit the space it is written into, otherwise the write
     * would silently drop the high bits */
    if (out->width < 8 && (int64_t)out->value < 0) {
        snprintf(key, sizeof(key), "patch[%u].value", (unsigned)index);
        setErrKey(err, errLen, "negative value needs width 8: ", key);
        return false;
    }
    if (out->width < 8 && out->value >= (1ULL << (out->width * 8))) {
        snprintf(key, sizeof(key), "patch[%u].value", (unsigned)index);
        setErrKey(err, errLen, "does not fit the given width: ", key);
        return false;
    }

    out->tag = NULL;
    if (!cfgStr(t, "tag", &out->tag, &tagLen, err, errLen)) {
        return false;
    }

    return true;
}

static bool parsePatches(const toml_table_t *dbg, UsConfig *cfg, char *err, size_t errLen) {
    toml_array_t *arr;

    if (dbg == NULL) {
        return true;
    }
    arr = toml_table_array(dbg, "patch");
    if (arr == NULL) {
        return true;
    }

    int n = toml_array_len(arr);
    if (n == 0) {
        return true;
    }

    cfg->patches = calloc((size_t)n, sizeof(UsPatch));
    if (cfg->patches == NULL) {
        setErr(err, errLen, "out of memory for the patch table");
        return false;
    }
    cfg->patchCount = (uint32_t)n;

    for (int i = 0; i < n; i++) {
        toml_table_t *entry = toml_array_table(arr, i);
        if (entry == NULL) {
            char key[32];
            snprintf(key, sizeof(key), "patch[%d]", i);
            setErrKey(err, errLen, "not a table: ", key);
            return false;
        }
        if (!parseOnePatch(entry, (uint32_t)i, &cfg->patches[i], err, errLen)) {
            return false;
        }
    }
    return true;
}

UsConfig *usConfigParse(const char *text, size_t len, char *err, size_t errLen) {
    char errbuf[160];
    UsConfig *cfg;
    const char *s = NULL;

    setErr(err, errLen, "");

    if (len > US_CFG_MAX_TEXT) {
        setErr(err, errLen, "config file is larger than the 64KB limit");
        return NULL;
    }

    cfg = calloc(1, sizeof(*cfg));
    if (cfg == NULL) {
        setErr(err, errLen, "out of memory for the config");
        return NULL;
    }

    cfg->logLevel = UsLogInfo;
    /*
     * The two that are on. A document that says nothing gets the whole
     * mechanism, and the [debug] table is only ever a way of taking a piece
     * of it out; every other key defaults to zero, which is what calloc has
     * already done
     */
    cfg->imageInplaceRewrite = true;
    cfg->el0InplaceRewrite = true;
    cfg->showLicenses = true;

    /* toml_parse edits the buffer and keeps pointers into it, so it has to
     * outlive the table */
    cfg->text = malloc(len + 1);
    if (cfg->text == NULL) {
        setErr(err, errLen, "out of memory for the config text");
        usConfigFree(cfg);
        return NULL;
    }
    if (len != 0) {
        memcpy(cfg->text, text, len);
    }
    cfg->text[len] = '\0';

    errbuf[0] = '\0';
    cfg->root = toml_parse(cfg->text, errbuf, sizeof(errbuf));
    if (cfg->root == NULL) {
        char msg[224];
        snprintf(msg, sizeof(msg), "config: %s", errbuf[0] != '\0' ? errbuf : "parse failed");
        setErr(err, errLen, msg);
        usConfigFree(cfg);
        return NULL;
    }


    toml_table_t *log = toml_table_table(cfg->root, "log");
    int levelLen = 0;
    if (!cfgStr(log, "level", &s, &levelLen, err, errLen)) {
        usConfigFree(cfg);
        return NULL;
    }
    if (s != NULL) {
        if (!parseLogLevel(s, levelLen, &cfg->logLevel)) {
            setErrKey(err, errLen, "unknown log level: ", "log.level");
            usConfigFree(cfg);
            return NULL;
        }
    }
    if (!cfgBool(log, "showLicenses", &cfg->showLicenses, err, errLen)) {
        usConfigFree(cfg);
        return NULL;
    }

    /*
     * What to do about the loads this hardware cannot carry out. Both are on
     * unless turned off, and neither can make the machine wrong by being off:
     * what it costs is exceptions, which the handler is there to answer
     */
    toml_table_t *ldapr = toml_table_table(cfg->root, "ldapr");
    if (!cfgBool(ldapr, "imageInplaceRewrite", &cfg->imageInplaceRewrite, err, errLen)
        || !cfgBool(ldapr, "el0InplaceRewrite", &cfg->el0InplaceRewrite, err, errLen)) {
        usConfigFree(cfg);
        return NULL;
    }

    /* Where the handler's own count of trapping addresses is kept: off
     * unless asked for, because it costs a lookup on every exception */
    cfg->statsEnabled = false;
    toml_table_t *stats = toml_table_table(cfg->root, "stats");
    if (!cfgBool(stats, "enabled", &cfg->statsEnabled, err, errLen)) {
        usConfigFree(cfg);
        return NULL;
    }

    cfg->patchDir = "usPatch";
    cfg->hasDescriptorBase = false;
    cfg->descriptorBaseRVA = 0;
    toml_table_t *kern = toml_table_table(cfg->root, "kernel");
    if (kern != NULL && cfgHas(kern, "descriptorBaseRVA")) {
        int64_t rva = 0;

        if (!cfgInt(kern, "descriptorBaseRVA", &rva, err, errLen) || rva <= 0
            || rva > 0xFFFFFFFFLL) {
            setErrKey(err, errLen, "not an address: ", "descriptorBaseRVA");
            usConfigFree(cfg);
            return NULL;
        }
        cfg->descriptorBaseRVA = (uint32_t)rva;
        cfg->hasDescriptorBase = true;
    }
    toml_table_t *patch = toml_table_table(cfg->root, "patch");
    if (patch != NULL) {
        int length = 0;
        const char *dir = toml_table_string_ref(patch, "dir", &length);

        if (dir != NULL) {
            cfg->patchDir = dir;
        }
    }

    /*
     * Serial output, under a table of its own rather than under the kernel's:
     * the driver, the payload and anything reporting later all need it, and
     * none of them is the kernel
     *
     * What the table names is the port, and it may name nothing at all: a
     * machine whose own tables describe a console needs no address here, and
     * looking for it is the default because the description travels with the
     * machine while a copy of it in a file does not
     */
    cfg->hasUART = false;
    cfg->uartKind = UsCfgUARTACPIFind;
    cfg->uartType = "acpi";
    cfg->uartBase = 0;
    cfg->uartWidth = 32;
    cfg->uartTable = UsCfgUARTTableSPCR;
    cfg->uartPath = NULL;
    cfg->uartClock = 0;
    cfg->uartBaud = 0;
    cfg->uartColour = true;
    {
        toml_table_t *uart = toml_table_table(cfg->root, "uart");
        const char *type = NULL;
        int typeLen = 0;

        if (uart != NULL && cfgHas(uart, "type")) {
            if (!cfgStr(uart, "type", &type, &typeLen, err, errLen)) {
                usConfigFree(cfg);
                return NULL;
            }
            cfg->uartType = type;
            if (sameStr(type, typeLen, "acpi")) {
                cfg->uartKind = UsCfgUARTACPIFind;
            } else if (sameStr(type, typeLen, "pl011")) {
                cfg->uartKind = UsCfgUARTPl011;
            } else if (sameStr(type, typeLen, "uart8250")) {
                cfg->uartKind = UsCfgUARTUart8250;
            } else if (sameStr(type, typeLen, "off")) {
                cfg->uartKind = UsCfgUARTOff;
            } else {
                setErrKey(err, errLen, "unknown uart type: ", "type");
                usConfigFree(cfg);
                return NULL;
            }
        }
        if (uart != NULL && cfgHas(uart, "table")) {
            const char *which = NULL;
            int whichLen = 0;

            if (!cfgStr(uart, "table", &which, &whichLen, err, errLen)) {
                usConfigFree(cfg);
                return NULL;
            }
            if (sameStr(which, whichLen, "SPCR")) {
                cfg->uartTable = UsCfgUARTTableSPCR;
            } else if (sameStr(which, whichLen, "DBG2")) {
                cfg->uartTable = UsCfgUARTTableDBG2;
            } else if (sameStr(which, whichLen, "DSDT")) {
                cfg->uartTable = UsCfgUARTTableDSDT;
            } else {
                setErrKey(err, errLen, "unknown uart table: ", "table");
                usConfigFree(cfg);
                return NULL;
            }
        }
        if (uart != NULL && cfgHas(uart, "path")) {
            const char *path = NULL;
            int pathLen = 0;

            if (!cfgStr(uart, "path", &path, &pathLen, err, errLen)) {
                usConfigFree(cfg);
                return NULL;
            }
            cfg->uartPath = path;
        }
        if (uart != NULL && cfgHas(uart, "width")) {
            int64_t width = 0;

            if (!cfgInt(uart, "width", &width, err, errLen)
                || (width != 8 && width != 32)) {
                setErrKey(err, errLen, "must be 8 or 32: ", "width");
                usConfigFree(cfg);
                return NULL;
            }
            cfg->uartWidth = (uint32_t)width;
        }
        if (uart != NULL && cfgHas(uart, "clock")) {
            int64_t clock = 0;

            /* Zero is the value that means "leave the port alone", so it is
             * one of the answers rather than a mistake */
            if (!cfgInt(uart, "clock", &clock, err, errLen) || clock < 0) {
                setErrKey(err, errLen, "not a frequency: ", "clock");
                usConfigFree(cfg);
                return NULL;
            }
            cfg->uartClock = (uint64_t)clock;
        }
        if (uart != NULL && cfgHas(uart, "baud")) {
            int64_t baud = 0;

            /* And the same here: a line rate of zero is "as the firmware set
             * it", which is what a machine whose tables describe a console
             * wants and what every configuration in this tree says */
            if (!cfgInt(uart, "baud", &baud, err, errLen) || baud < 0
                || baud > 0xFFFFFFFFLL) {
                setErrKey(err, errLen, "not a line rate: ", "baud");
                usConfigFree(cfg);
                return NULL;
            }
            cfg->uartBaud = (uint32_t)baud;
        }
        /*
         * A base address is only meaningful for a port the configuration
         * names: with a type of its own the driver has somewhere to write,
         * and with none it has to find one. Naming one is also the way a
         * machine with no usable table is spoken to at all
         */
        if (uart != NULL && cfgHas(uart, "baseAddr")) {
            int64_t base = 0;

            if (!cfgInt(uart, "baseAddr", &base, err, errLen) || base <= 0) {
                setErrKey(err, errLen, "not an address: ", "baseAddr");
                usConfigFree(cfg);
                return NULL;
            }
            cfg->uartBase = (uint64_t)base;
            cfg->hasUART = true;
            if (cfg->uartKind == UsCfgUARTACPIFind) {
                cfg->uartKind = UsCfgUARTPl011;
                cfg->uartType = "pl011";
            }
        }
        if (uart != NULL && !cfgBool(uart, "color", &cfg->uartColour, err, errLen)) {
            usConfigFree(cfg);
            return NULL;
        }
    }

    /*
     * The switches that take a piece of the mechanism out of the way. Each is
     * named for what it stops, and each is off unless set, so the whole of
     * the mechanism is what a file that does not mention this table gets
     */
    toml_table_t *dbg = toml_table_table(cfg->root, "debug");
    if (!cfgBool(dbg, "notArmVectors", &cfg->notArmVectors, err, errLen)
        || !cfgBool(dbg, "notArmVectorsEl1t", &cfg->notArmVectorsEl1t, err, errLen)
        || !cfgBool(dbg, "notArmHandover", &cfg->notArmHandover, err, errLen)
        || !cfgBool(dbg, "notVamap", &cfg->notVamap, err, errLen)
        || !cfgBool(dbg, "spxStack", &cfg->spxStack, err, errLen)) {
        usConfigFree(cfg);
        return NULL;
    }
    if (!parsePatches(dbg, cfg, err, errLen)) {
        usConfigFree(cfg);
        return NULL;
    }

    return cfg;
}

void usConfigFree(UsConfig *c) {
    if (c == NULL) {
        return;
    }
    if (c->root != NULL) {
        toml_free(c->root);
    }
    free(c->patches);
    free(c->text);
    free(c);
}
