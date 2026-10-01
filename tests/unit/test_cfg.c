/*
 * Checks for the configuration loader.
 *
 * The loader is where unsupported values are rejected, so most of what is
 * tested here is that a bad document fails loudly instead of yielding a half
 * applied configuration.
 */

#include <stdio.h>
#include <string.h>

#include "core/cfg.h"

static int failures;
static int checks;

static void ok(const char *name, int cond) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

static void eqInt(const char *name, int64_t got, int64_t want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %-30s want %lld got %lld\n", name, (long long)want, (long long)got);
    }
}

static void eqStr(const char *name, const char *got, const char *want) {
    checks++;
    if (got == NULL || strcmp(got, want) != 0) {
        failures++;
        printf("FAIL %-30s want \"%s\" got \"%s\"\n", name, want, got ? got : "(null)");
    }
}

/* Parses and asserts the document is accepted. */
static UsConfig *accept(const char *name, const char *doc) {
    char err[256] = { 0 };
    UsConfig *c = usConfigParse(doc, strlen(doc), err, sizeof(err));
    checks++;
    if (c == NULL) {
        failures++;
        printf("FAIL %-30s rejected: %s\n", name, err);
    }
    return c;
}

/* Parses and asserts the document is rejected, returning the message. */
static void reject(const char *name, const char *doc) {
    char err[256] = { 0 };
    UsConfig *c = usConfigParse(doc, strlen(doc), err, sizeof(err));
    checks++;
    if (c != NULL) {
        failures++;
        printf("FAIL %-30s accepted but should not be\n", name);
        usConfigFree(c);
        return;
    }
    checks++;
    if (err[0] == '\0') {
        failures++;
        printf("FAIL %-30s rejected without a message\n", name);
    }
}

static void testDefaults(void) {
    /* An empty document is how "no configuration file" is expressed. */
    UsConfig *c = accept("empty", "");
    if (c == NULL) {
        return;
    }
    eqInt("default version", c->version, 1);
    eqInt("default log level", c->logLevel, UsLogInfo);
    eqInt("default ldapr_rewrite", c->ldaprRewrite, 1);
    eqInt("default debug.enabled", c->debugEnabled, 0);
    eqInt("default patch count", c->patchCount, 0);
    eqInt("debug bag fallback", usConfigDebugBool(c, "nope", true), 1);
    eqInt("debug bag int fallback", usConfigDebugInt(c, "nope", 42), 42);
    eqStr("debug bag str fallback", usConfigDebugStr(c, "nope", "def"), "def");
    usConfigFree(c);
}

static void testShippedShape(void) {
    static const char doc[] =
        "version = 1\n"
        "\n"
        "[log]\n"
        "level = \"verbose\"\n"
        "\n"
        "[scan]\n"
        "ldapr_rewrite = false\n"
        "\n"
        "[debug]\n"
        "enabled = true\n"
        "flag1 = true\n"
        "answer = 42\n"
        "label = \"hello\"\n"
        "\n"
        "[[debug.patch]]\n"
        "target = \"ntoskrnl\"\n"
        "rva    = 0x203bd4\n"
        "value  = 0x14000000\n"
        "width  = 4\n"
        "tag    = \"vbar write site\"\n"
        "\n"
        "[[debug.patch]]\n"
        "target = \"winload\"\n"
        "rva    = 0x1a2b0\n"
        "value  = 0xd503201f\n"
        "width  = 4\n";

    UsConfig *c = accept("shipped shape", doc);
    if (c == NULL) {
        return;
    }

    eqInt("version", c->version, 1);
    eqInt("log level", c->logLevel, UsLogVerbose);
    eqInt("ldapr_rewrite", c->ldaprRewrite, 0);
    eqInt("debug.enabled", c->debugEnabled, 1);

    eqInt("debug.flag1", usConfigDebugBool(c, "flag1", false), 1);
    eqInt("debug.answer", usConfigDebugInt(c, "answer", 0), 42);
    eqStr("debug.label", usConfigDebugStr(c, "label", ""), "hello");

    eqInt("patch count", c->patchCount, 2);
    if (c->patchCount == 2) {
        eqStr("patch[0].target", c->patches[0].target, "ntoskrnl");
        eqInt("patch[0].rva", c->patches[0].rva, 0x203bd4);
        eqInt("patch[0].value", c->patches[0].value, 0x14000000);
        eqInt("patch[0].width", c->patches[0].width, 4);
        eqStr("patch[0].tag", c->patches[0].tag, "vbar write site");

        eqStr("patch[1].target", c->patches[1].target, "winload");
        eqInt("patch[1].rva", c->patches[1].rva, 0x1a2b0);
        eqInt("patch[1].value", c->patches[1].value, 0xd503201f);
        eqInt("patch[1].width", c->patches[1].width, 4);
        ok("patch[1].tag absent", c->patches[1].tag == NULL);
    }

    usConfigFree(c);
}

static void testRejections(void) {
    /* A value the parser cannot type is exactly what the loader must catch,
     * which is how the floating point and date cases are stopped. */
    reject("float level", "[log]\nlevel = 1.5\n");
    reject("date level", "[log]\nlevel = 2024-01-02\n");
    reject("int level", "[log]\nlevel = 3\n");
    reject("unknown level", "[log]\nlevel = \"chatty\"\n");

    reject("bool as string", "[scan]\nldapr_rewrite = \"yes\"\n");
    reject("version 2", "version = 2\n");

    reject("patch missing target", "[[debug.patch]]\nrva = 1\nvalue = 1\nwidth = 4\n");
    reject("patch missing rva", "[[debug.patch]]\ntarget = \"ntoskrnl\"\nvalue = 1\nwidth = 4\n");
    reject("patch missing value", "[[debug.patch]]\ntarget = \"ntoskrnl\"\nrva = 1\nwidth = 4\n");
    reject("patch missing width", "[[debug.patch]]\ntarget = \"ntoskrnl\"\nrva = 1\nvalue = 1\n");
    reject("patch bad width", "[[debug.patch]]\ntarget = \"ntoskrnl\"\nrva = 1\nvalue = 1\nwidth = 3\n");
    reject("patch value too wide", "[[debug.patch]]\ntarget = \"ntoskrnl\"\nrva = 1\nvalue = 0x1ff\nwidth = 1\n");
    reject("patch rva not an int", "[[debug.patch]]\ntarget = \"ntoskrnl\"\nrva = \"x\"\nvalue = 1\nwidth = 4\n");
    reject("patch rva out of range", "[[debug.patch]]\ntarget = \"ntoskrnl\"\nrva = 0x1ffffffff\nvalue = 1\nwidth = 4\n");
    reject("patch entry not a table", "debug = { patch = [1, 2] }\n");
    reject("unterminated string", "[log]\nlevel = \"info\n");
}

static void testWidths(void) {
    /* Every supported width, including the boundary value that still fits and
     * a negative literal for an all ones 64 bit pattern. */
    UsConfig *c = accept("widths",
        "[[debug.patch]]\ntarget = \"a\"\nrva = 0\nvalue = 0xff\nwidth = 1\n"
        "[[debug.patch]]\ntarget = \"b\"\nrva = 1\nvalue = 0xffff\nwidth = 2\n"
        "[[debug.patch]]\ntarget = \"c\"\nrva = 2\nvalue = 0xffffffff\nwidth = 4\n"
        "[[debug.patch]]\ntarget = \"d\"\nrva = 3\nvalue = -1\nwidth = 8\n");
    if (c == NULL) {
        return;
    }
    eqInt("four patches", c->patchCount, 4);
    if (c->patchCount == 4) {
        eqInt("width 1", c->patches[0].width, 1);
        eqInt("width 2", c->patches[1].width, 2);
        eqInt("width 4", c->patches[2].width, 4);
        eqInt("width 8", c->patches[3].width, 8);
        eqInt("value 8 byte", (int64_t)c->patches[3].value, -1);
    }
    usConfigFree(c);
}

int main(void) {
    testDefaults();
    testShippedShape();
    testRejections();
    testWidths();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
