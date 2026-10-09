/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * blobcheck - catch a leaked address in the payload blob
 *
 * The blob is copied to an address the linker never knew about, so any word
 * in it that holds a link time address is a bug: it will still be that
 * address after the copy, and the payload will jump or read somewhere that
 * has nothing to do with it
 *
 * This is not caught by looking for relocations. A GOT entry, or a constant
 * pool filled in by the linker, is resolved at link time and leaves no
 * relocation behind; the address simply appears in the bytes. So the bytes are
 * what get checked
 *
 * The test is a heuristic and is used as one: a word that points inside the
 * blob is reported. A legitimate constant could in principle land there by
 * accident, and if that ever happens the fix is to say so here rather than to
 * weaken the check
 *
 * Usage: blobcheck <payload.bin> <payload.elf>
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct App_t {
    FILE *out;
    int   failures;
} App;

static void fail(App *app, const char *what) {
    fprintf(stderr, "FAIL %s\n", what);
    app->failures++;
}

static uint8_t *readWhole(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *data;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = malloc((size_t)size);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return data;
}

/*
 * Symbols that hold bytes and never addresses
 *
 * The test looks at every aligned word, and a run of byte data will hold a
 * small one sooner or later: the glyph table is eight bytes a row and three of
 * its rows are values below the blob's size. Those are bit patterns, not
 * addresses, and no amount of care in the code would make them anything else
 *
 * Naming the region here is the honest answer rather than weakening the test:
 * what is claimed is narrow and checkable -- nothing inside these symbols is
 * ever an address -- and everything outside them is still looked at. Adding a
 * symbol to this list is a claim, and it should be read as one
 */
static const char *const kDataOnly[] = { "kFont" };
#define DATA_ONLY_COUNT (sizeof(kDataOnly) / sizeof(kDataOnly[0]))

/* The range a symbol covers in the blob, or false when it is not there */
static int symbolRange(const char *elfPath, const char *name, uint64_t *start,
                       uint64_t *end) {
    char cmd[1024];
    FILE *p;
    long s = -1;
    long size = -1;

    /*
     * -S is the symbol's size, which is what makes the range exact: taking
     * the next symbol's address as the end is a guess that is wrong for the
     * last one, and a range that runs off the end of the blob would exempt
     * everything after it
     */
    snprintf(cmd, sizeof(cmd),
             "llvm-nm -S %s | awk -v n=\"%s\" '$4==n"
             "{print strtonum(\"0x\"$1), strtonum(\"0x\"$2); exit}'",
             elfPath, name);
    p = popen(cmd, "r");
    if (p == NULL) {
        return 0;
    }
    if (fscanf(p, "%ld %ld", &s, &size) == 2 && s >= 0 && size > 0) {
        *start = (uint64_t)s;
        *end = (uint64_t)s + (uint64_t)size;
        pclose(p);
        return 1;
    }
    pclose(p);
    return 0;
}

/* Whether an offset is inside one of the regions that hold no addresses */
static int insideDataOnly(const char *elfPath, size_t at) {
    for (size_t i = 0; i < DATA_ONLY_COUNT; i++) {
        uint64_t start = 0;
        uint64_t end = 0;

        if (symbolRange(elfPath, kDataOnly[i], &start, &end) && at >= start
            && at < end) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, const char **argv) {
    App app = { .out = stdout, .failures = 0 };
    uint8_t *blob;
    size_t len = 0;
    size_t leaked = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <payload.bin> <payload.elf>\n", argv[0]);
        return 2;
    }

    blob = readWhole(argv[1], &len);
    if (blob == NULL) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    if (len == 0) {
        fail(&app, "the blob is empty");
        free(blob);
        return app.failures != 0;
    }

    /*
     * The range the payload believes it occupies has to be exactly the file,
     * or a section was left outside it and the copy misses that section
     */
    {
        char cmd[1024];
        FILE *p;
        long claimed = -1;

        snprintf(cmd, sizeof(cmd),
                 "llvm-nm %s | awk '/ usPayloadEnd$/{e=strtonum(\"0x\"$1)} "
                 "/ usPayloadStart$/{s=strtonum(\"0x\"$1)} END{print e-s}'",
                 argv[2]);
        p = popen(cmd, "r");
        if (p == NULL || fscanf(p, "%ld", &claimed) != 1) {
            fail(&app, "cannot read the payload's extent");
        } else if (claimed >= 0 && (size_t)claimed != len) {
            fprintf(stderr, "FAIL the blob is %zu bytes, the symbols claim %ld\n", len, claimed);
            app.failures++;
        }
        if (p != NULL) {
            pclose(p);
        }
    }

    /*
     * Every aligned word, against the blob's own range. An address into the
     * blob that is not relative to anything is exactly the shape of the bug
     * this exists to catch
     */
    for (size_t off = 0; off + 8 <= len; off += 8) {
        uint64_t word;

        memcpy(&word, blob + off, sizeof(word));
        if (word != 0 && word < (uint64_t)len
            && !insideDataOnly(argv[2], off)) {
            if (leaked < 8) {
                fprintf(stderr, "FAIL word at 0x%zx holds 0x%llx, inside the blob\n",
                        off, (unsigned long long)word);
            }
            leaked++;
        }
    }
    if (leaked != 0) {
        fprintf(stderr, "FAIL %zu words hold an address into the blob\n", leaked);
        app.failures++;
    }

    if (app.failures == 0) {
        printf("blob: %zu bytes, no addresses into itself\n", len);
    }
    free(blob);
    return app.failures != 0;
}
