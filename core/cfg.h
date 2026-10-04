/*
 * Runtime configuration.
 *
 * The driver takes all of its tunables from a TOML document, so a build never
 * needs a flag embedded in it: change the file on the boot volume and the next
 * boot behaves differently. See clean-room/design.md for the format.
 *
 * Parsing is pure: this file only ever sees a buffer. Finding and reading that
 * buffer is the bootPhase's job, in uefi/src/config.c.
 *
 * Ownership: a parsed config owns the document buffer and the parsed table.
 * The strings reachable through UsPatch and the debug accessors point into
 * that storage and stay valid until usConfigFree.
 */

#ifndef US_CFG_H
#define US_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "toml.h"

typedef enum UsLogLevel_e {
    UsLogOff,
    UsLogError,
    UsLogInfo,
    UsLogVerbose,
    UsLogDebug,
} UsLogLevel;

/*
 * One entry of the debug patch table. The bootPhase applies these directly,
 * without going through the scanner, which makes them useful for planting a
 * breakpoint in a known place while bringing the driver up.
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
    int           version;
    UsLogLevel    logLevel;
    bool          ldaprRewrite;
    /* Whether user-mode instructions may be replaced while the kernel runs. */
    bool          el0InPlace;
    /*
     * Directory of patch list files, relative to the volume the configuration
     * was read from. Points into the document storage. An empty string turns
     * the feature off; the volume root is refused where it is used.
     */
    const char   *patchDir;
    bool          debugEnabled;
    UsPatch      *patches;    /* owned array */
    uint32_t      patchCount;
} UsConfig;

/*
 * Parses a document. Any problem is reported into err and yields NULL: the
 * caller must not press on with a half applied configuration.
 *
 * An empty document is valid and yields the built in defaults, which is how
 * "no configuration file present" is expressed.
 */
UsConfig *usConfigParse(const char *text, size_t len, char *err, size_t errLen);
void usConfigFree(UsConfig *c);

/*
 * The [debug] table is a free form bag of bools, ints and strings. These look
 * a key up and fall back to def when it is absent. US_CFG_DEBUG_* is the
 * convention used in the shipped file.
 */
bool usConfigDebugBool(const UsConfig *c, const char *key, bool def);
int64_t usConfigDebugInt(const UsConfig *c, const char *key, int64_t def);
const char *usConfigDebugStr(const UsConfig *c, const char *key, const char *def);

#endif
