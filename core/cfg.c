/*
 * Runtime configuration, see cfg.h for the contract.
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
 * accessors cannot represent. */
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
 * value. */
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
 * closing quote, not a terminator. */
static bool refEq(const char *ref, int len, const char *literal) {
    int i = 0;
    for (; i < len && literal[i] != '\0'; i++) {
        if (ref[i] != literal[i]) {
            return false;
        }
    }
    return i == len && literal[i] == '\0';
}

static bool parseLogLevel(const char *s, int len, UsLogLevel *out) {
    static const struct {
        const char *name;
        UsLogLevel  level;
    } table[] = {
        { "off", UsLogOff },
        { "error", UsLogError },
        { "info", UsLogInfo },
        { "verbose", UsLogVerbose },
        { "debug", UsLogDebug },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (refEq(s, len, table[i].name)) {
            *out = table[i].level;
            return true;
        }
    }
    return false;
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
     * for high bits set. */
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
     * would silently drop the high bits. */
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
    int64_t n;

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
    cfg->ldaprRewrite = true;
    cfg->debugEnabled = false;

    /* toml_parse edits the buffer and keeps pointers into it, so it has to
     * outlive the table. */
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

    toml_table_t *scan = toml_table_table(cfg->root, "scan");
    if (!cfgBool(scan, "ldaprRewrite", &cfg->ldaprRewrite, err, errLen)) {
        usConfigFree(cfg);
        return NULL;
    }
    /* Replacing user-mode instructions where they stand is the default: it is
     * what makes a user-mode RCpc load stop taking an exception, and it is
     * not something the kernel's integrity check has an opinion about. */
    cfg->el0InPlace = true;
    if (!cfgBool(scan, "el0InPlace", &cfg->el0InPlace, err, errLen)) {
        usConfigFree(cfg);
        return NULL;
    }

    /* Where the handler's own count of trapping addresses is kept: off
     * unless asked for, because it costs a lookup on every exception. */
    cfg->statsEnabled = false;
    toml_table_t *stats = toml_table_table(cfg->root, "stats");
    if (!cfgBool(stats, "enabled", &cfg->statsEnabled, err, errLen)) {
        usConfigFree(cfg);
        return NULL;
    }

    cfg->patchDir = "usPatch";
    cfg->hasDescriptorBase = false;
    cfg->descriptorBaseRva = 0;
    toml_table_t *kern = toml_table_table(cfg->root, "kernel");
    if (kern != NULL && cfgHas(kern, "descriptorBaseRva")) {
        int64_t rva = 0;

        if (!cfgInt(kern, "descriptorBaseRva", &rva, err, errLen) || rva <= 0
            || rva > 0xFFFFFFFFLL) {
            setErrKey(err, errLen, "not an address: ", "descriptorBaseRva");
            usConfigFree(cfg);
            return NULL;
        }
        cfg->descriptorBaseRva = (uint32_t)rva;
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
     * Serial output, stated at the root: the driver, the payload and anything
     * reporting later all need it, so it does not belong under a table of its
     * own. No base means no serial output at all.
     */
    cfg->hasUart = false;
    cfg->uartType = "pl011";
    cfg->uartBase = 0;
    cfg->uartWidth = 32;
    if (cfgHas(cfg->root, "uartBase")) {
        int64_t base = 0;
        int64_t width = 0;
        const char *type = NULL;
        int typeLen = 0;

        if (!cfgInt(cfg->root, "uartBase", &base, err, errLen) || base <= 0) {
            setErrKey(err, errLen, "not an address: ", "uartBase");
            usConfigFree(cfg);
            return NULL;
        }
        cfg->uartBase = (uint64_t)base;
        cfg->hasUart = true;
        if (cfgHas(cfg->root, "uartType")) {
            if (!cfgStr(cfg->root, "uartType", &type, &typeLen, err, errLen)) {
                usConfigFree(cfg);
                return NULL;
            }
            if (sameStr(type, typeLen, "pl011")) {
                cfg->uartWidth = 32;
            } else if (sameStr(type, typeLen, "uart8250")) {
                cfg->uartWidth = 8;
            } else {
                setErrKey(err, errLen, "unknown uart type: ", "uartType");
                usConfigFree(cfg);
                return NULL;
            }
            cfg->uartType = type;
        }
        if (cfgHas(cfg->root, "uartWidth")) {
            if (!cfgInt(cfg->root, "uartWidth", &width, err, errLen)
                || (width != 8 && width != 32)) {
                setErrKey(err, errLen, "must be 8 or 32: ", "uartWidth");
                usConfigFree(cfg);
                return NULL;
            }
            cfg->uartWidth = (uint32_t)width;
        }
    }

    toml_table_t *dbg = toml_table_table(cfg->root, "debug");
    if (!cfgBool(dbg, "enabled", &cfg->debugEnabled, err, errLen)) {
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

static const toml_table_t *debugTable(const UsConfig *c) {
    if (c == NULL || c->root == NULL) {
        return NULL;
    }
    return toml_table_table(c->root, "debug");
}

bool usConfigDebugBool(const UsConfig *c, const char *key, bool def) {
    const toml_table_t *t = debugTable(c);
    if (!cfgHas(t, key)) {
        return def;
    }
    toml_value_t v = toml_table_bool(t, key);
    return v.ok ? v.u.b : def;
}

int64_t usConfigDebugInt(const UsConfig *c, const char *key, int64_t def) {
    const toml_table_t *t = debugTable(c);
    if (!cfgHas(t, key)) {
        return def;
    }
    toml_value_t v = toml_table_int(t, key);
    return v.ok ? v.u.i : def;
}

const char *usConfigDebugStr(const UsConfig *c, const char *key, const char *def) {
    const toml_table_t *t = debugTable(c);
    int len = 0;
    const char *v;

    if (!cfgHas(t, key)) {
        return def;
    }
    v = toml_table_string_ref(t, key, &len);
    return v != NULL ? v : def;
}
