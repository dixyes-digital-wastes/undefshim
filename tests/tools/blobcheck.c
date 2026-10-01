/*
 * blobcheck - catch a leaked address in the payload blob.
 *
 * The blob is copied to an address the linker never knew about, so any word
 * in it that holds a link time address is a bug: it will still be that
 * address after the copy, and the payload will jump or read somewhere that
 * has nothing to do with it.
 *
 * This is not caught by looking for relocations. A GOT entry, or a constant
 * pool filled in by the linker, is resolved at link time and leaves no
 * relocation behind; the address simply appears in the bytes. So the bytes are
 * what get checked.
 *
 * The test is a heuristic and is used as one: a word that points inside the
 * blob is reported. A legitimate constant could in principle land there by
 * accident, and if that ever happens the fix is to say so here rather than to
 * weaken the check.
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

int main(int argc, char **argv) {
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
     * or a section was left outside it and the copy misses that section.
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
     * this exists to catch.
     */
    for (size_t off = 0; off + 8 <= len; off += 8) {
        uint64_t word;

        memcpy(&word, blob + off, sizeof(word));
        if (word != 0 && word < (uint64_t)len) {
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
