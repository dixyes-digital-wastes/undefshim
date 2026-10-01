/*
 * peprobe - run the locators over a directory of Windows images.
 *
 * This is a host tool, not part of the driver. It answers the question the
 * locators have to answer before anything is built on them: given a real
 * image, does each of them find exactly one place?
 *
 * A locator that finds two is worse than one that finds none, because it will
 * pick a place and the failure surfaces much later, so ambiguity is reported
 * as loudly as absence.
 *
 * Usage: peprobe <corpus-root> [file ...]
 *
 * The corpus is laid out as <root>/<version>/<name>. The default file list is
 * the set the driver cares about; a version without those files is skipped.
 *
 * Known gaps are declared below rather than papered over, so a new gap shows
 * up as a failure and an accepted one does not.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/pe.h"
#include "core/scan.h"

#define MAX_VERSIONS 64

static int gFailures;
static int gChecks;

typedef struct Tally_t {
    int unique;
    int none;
    int ambiguous;
} Tally;

static Tally gLeaf;
static Tally gHandoff;
static int   gClassOk;
static int   gClassBad;

/*
 * Versions whose content predates the signatures. Each entry says what is
 * expected to be missing so that the report stays meaningful instead of
 * accumulating unexplained blanks.
 */
typedef struct Gap_t {
    const char *version;
    const char *file;
    bool        noLeaf;
} Gap;

static const Gap kGaps[] = {
    /* Pre-release image that the analysis notes as reference only: it does
     * not carry the transfer leaf at all, and must not be patched. */
    { "14877", "winload.efi", true },
    { "14877", "bootmgfw.efi", true },
};

static const Gap *findGap(const char *version, const char *file) {
    for (size_t i = 0; i < sizeof(kGaps) / sizeof(kGaps[0]); i++) {
        if (strcmp(kGaps[i].version, version) == 0 && strcmp(kGaps[i].file, file) == 0) {
            return &kGaps[i];
        }
    }
    return NULL;
}

/*
 * Which kernels are expected to use LDAPR at all. 21h2, 22h2 and 23h2 each
 * carry exactly one, so a machine without the extension traps there too; from
 * 24h2 on it is used thousands of times.
 */
static bool versionUsesLdapr(const char *version) {
    static const char *withLdapr[] = { "21h2", "22h2", "23h2", "24h2", "26100pe", "26h1" };

    for (size_t i = 0; i < sizeof(withLdapr) / sizeof(withLdapr[0]); i++) {
        if (strcmp(version, withLdapr[i]) == 0) {
            return true;
        }
    }
    return false;
}

static char *readFile(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    len = ftell(f);
    if (len <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    buf = malloc((size_t)len);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)len;
    return buf;
}

static void tally(Tally *t, size_t matches) {
    if (matches == 1) {
        t->unique++;
    } else if (matches == 0) {
        t->none++;
    } else {
        t->ambiguous++;
    }
}

/* A failed expectation is reported with enough context to act on. */
static void expect(const char *version, const char *file, const char *what, bool got, bool want) {
    gChecks++;
    if (got == want) {
        return;
    }
    gFailures++;
    printf("FAIL %-9s %-13s %s: expected %s, got %s\n", version, file, what,
           want ? "present" : "absent", got ? "present" : "absent");
}

static void probe(const char *version, const char *name, const char *path) {
    size_t size = 0;
    char *data = readFile(path, &size);
    UsImage img;
    const Gap *gap;
    bool isKernel = strcmp(name, "ntoskrnl.exe") == 0;
    bool isWinload = strcmp(name, "winload.efi") == 0;

    if (data == NULL) {
        printf("%-9s %-13s not readable\n", version, name);
        return;
    }
    if (!usImageInitFile(&img, data, size)) {
        printf("%-9s %-13s not a valid arm64 image\n", version, name);
        free(data);
        return;
    }

    gap = findGap(version, name);

    UsImageKind kind = usImageClassify(&img);
    UsLeafSite leaf = usLocateTransferLeaf(&img);
    UsHandoffSite handoff = usLocateTtbrHandoff(&img);
    UsLdaprCounts ldapr = usCountLdapr(&img);

    printf("%-9s %-13s class=%-9s leaf=%-4zu handoff=%-10s ldapr=%-6zu (w%zu x%zu b%zu h%zu)\n",
           version, name, usImageKindName(kind), leaf.matches,
           handoff.found
               ? (handoff.form == UsHandoffInTransSection ? "trans" : "sctlr")
               : (handoff.candidates == 0 ? "none" : "unresolved"),
           ldapr.total, ldapr.word, ldapr.xword, ldapr.byte, ldapr.half);

    /* Classification: ntoskrnl and winload are identified from content.
     * bootmgfw is not, and must not be mistaken for either. */
    if (isKernel) {
        gChecks++;
        if (kind == UsImageNtoskrnl) {
            gClassOk++;
        } else {
            gClassBad++;
            gFailures++;
            printf("FAIL %-9s %-13s class: expected ntoskrnl, got %s\n",
                   version, name, usImageKindName(kind));
        }
    } else if (isWinload) {
        gChecks++;
        if (kind == UsImageWinload) {
            gClassOk++;
        } else {
            gClassBad++;
            gFailures++;
            printf("FAIL %-9s %-13s class: expected winload, got %s\n",
                   version, name, usImageKindName(kind));
        }
    } else {
        expect(version, name, "not classified as the kernel", kind != UsImageNtoskrnl, true);
        expect(version, name, "not classified as winload", kind != UsImageWinload, true);
    }

    /* The leaf is what the driver hooks, so it has to be unique wherever it
     * is expected to exist, and the exception directory has to agree that it
     * begins a function. */
    if (!isKernel) {
        tally(&gLeaf, leaf.matches);
        expect(version, name, "transfer leaf", leaf.matches == 1,
               gap == NULL || !gap->noLeaf);
        if (leaf.found) {
            expect(version, name, "leaf at a function start", leaf.isFunctionStart, true);
        }
    }
    if (isWinload || strcmp(name, "bootmgfw.efi") == 0) {
        tally(&gHandoff, handoff.found ? 1 : (handoff.candidates == 0 ? 0 : 2));
        expect(version, name, "page table handoff", handoff.found, true);
    }

    /* The whole point of the shim: on hardware without the extension every
     * one of these traps, so a kernel that has any is a kernel that needs
     * the shim. Neither loader contains one. */
    if (isKernel) {
        expect(version, name, "kernel uses LDAPR", ldapr.total > 0, versionUsesLdapr(version));
    } else {
        expect(version, name, "loader free of LDAPR", ldapr.total == 0, true);
    }

    free(data);
}

static void walkVersion(const char *root, const char *version, char **names, int nameCount) {
    for (int i = 0; i < nameCount; i++) {
        char path[512];
        struct stat st;

        snprintf(path, sizeof(path), "%s/%s/%s", root, version, names[i]);
        if (stat(path, &st) != 0) {
            continue;
        }
        probe(version, names[i], path);
    }
}

int main(int argc, char **argv) {
    static char *defaultNames[] = { "ntoskrnl.exe", "winload.efi", "bootmgfw.efi" };
    char *versions[MAX_VERSIONS];
    int versionCount = 0;
    char **names = defaultNames;
    int nameCount = 3;
    const char *root;
    DIR *dir;
    struct dirent *ent;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <corpus-root> [file ...]\n", argv[0]);
        return 2;
    }
    root = argv[1];
    if (argc > 2) {
        names = &argv[2];
        nameCount = argc - 2;
    }

    dir = opendir(root);
    if (dir == NULL) {
        fprintf(stderr, "cannot open %s\n", root);
        return 2;
    }
    while ((ent = readdir(dir)) != NULL && versionCount < MAX_VERSIONS) {
        char sub[512];
        struct stat st;

        if (ent->d_name[0] == '.') {
            continue;
        }
        snprintf(sub, sizeof(sub), "%s/%s", root, ent->d_name);
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        versions[versionCount++] = strdup(ent->d_name);
    }
    closedir(dir);

    /* Sorted so the report reads the same way every run. */
    for (int i = 0; i < versionCount; i++) {
        for (int k = i + 1; k < versionCount; k++) {
            if (strcmp(versions[k], versions[i]) < 0) {
                char *t = versions[i];
                versions[i] = versions[k];
                versions[k] = t;
            }
        }
    }
    for (int i = 0; i < versionCount; i++) {
        walkVersion(root, versions[i], names, nameCount);
        free(versions[i]);
    }

    printf("\n");
    printf("classification : %d ok, %d wrong\n", gClassOk, gClassBad);
    printf("transfer leaf  : %d unique, %d absent, %d ambiguous\n",
           gLeaf.unique, gLeaf.none, gLeaf.ambiguous);
    printf("handoff        : %d unique, %d absent, %d ambiguous\n",
           gHandoff.unique, gHandoff.none, gHandoff.ambiguous);
    printf("%d checks, %d failures\n", gChecks, gFailures);

    return gFailures != 0;
}
