/*
 * Checks for the walk over the loader's module list.
 *
 * The list is built here rather than read from a machine, which is the point:
 * the offsets are the part that can be wrong, and a test that used a real
 * capture would agree with whatever the implementation already did. Building
 * the structure from the field names means a field read at the wrong place
 * lands on something else and fails.
 *
 * The cases that matter are the ones where a wrong answer would look right: a
 * name that is a prefix of another, a list that is a ring, and a list whose
 * shape stops making sense part way through.
 */

#include <stdio.h>
#include <string.h>

#include "core/ldr.h"

static int failures;
static int checks;

static void ok(const char *name, int cond) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

static void eqU64(const char *name, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %-46s want 0x%llx got 0x%llx\n", name,
               (unsigned long long)want, (unsigned long long)got);
    }
}

/* --- building the structures the walk expects ---------------------------- */

#define LIST_HEAD 0x10U
#define ENTRY_DLL_BASE 0x30U
#define ENTRY_ENTRY 0x38U
#define ENTRY_SIZE 0x40U
#define ENTRY_NAME 0x58U
#define ENTRY_STRIDE 0x80U

#define MAX_ENTRIES 8
#define NAME_CHARS 64

typedef struct Fixture_t {
    uint8_t block[0x1000];
    uint8_t entries[MAX_ENTRIES][ENTRY_STRIDE];
    uint8_t names[MAX_ENTRIES][NAME_CHARS * 2];
    uint32_t count;
} Fixture;

static void wr64(void *at, uint64_t v) {
    memcpy(at, &v, 8);
}

static void wr32(void *at, uint32_t v) {
    memcpy(at, &v, 4);
}

static void wr16(void *at, uint16_t v) {
    memcpy(at, &v, 2);
}

/* The name is UTF-16 in the list, so an ASCII name has to be widened. */
static void setName(Fixture *fx, uint32_t i, const char *name) {
    size_t n = strlen(name);
    uint16_t *dst = (uint16_t *)fx->names[i];

    for (size_t k = 0; k < n; k++) {
        dst[k] = (uint16_t)name[k];
    }
    dst[n] = 0;
    wr16(fx->entries[i] + ENTRY_NAME, (uint16_t)(n * 2));
    wr16(fx->entries[i] + ENTRY_NAME + 2, (uint16_t)(n * 2 + 2));
    wr64(fx->entries[i] + ENTRY_NAME + 8, (uint64_t)(uintptr_t)fx->names[i]);
}

static void addEntry(Fixture *fx, const char *name, uint64_t base, uint64_t size) {
    uint32_t i = fx->count++;

    memset(fx->entries[i], 0, ENTRY_STRIDE);
    wr64(fx->entries[i] + ENTRY_DLL_BASE, base);
    wr64(fx->entries[i] + ENTRY_ENTRY, base + 0x1000);
    wr32(fx->entries[i] + ENTRY_SIZE, (uint32_t)size);
    setName(fx, i, name);
}

/*
 * Links the entries in order and points the head at the first. The last points
 * back at the head, which is what ends the walk.
 */
static void link(Fixture *fx) {
    uint64_t head = (uint64_t)(uintptr_t)fx->block + LIST_HEAD;

    for (uint32_t i = 0; i < fx->count; i++) {
        uint64_t next = (i + 1 < fx->count)
                            ? (uint64_t)(uintptr_t)fx->entries[i + 1]
                            : head;

        wr64(fx->entries[i], next);
    }
    wr64(fx->block + LIST_HEAD, fx->count ? (uint64_t)(uintptr_t)fx->entries[0] : head);
}

/* --- the checks ----------------------------------------------------------- */

static void testFindsTheNamedModule(void) {
    Fixture fx;
    UsLdrModule m = { 0 };

    memset(&fx, 0, sizeof(fx));
    addEntry(&fx, "ntoskrnl.exe", 0xFFFFF80053C00000ULL, 0x1249000);
    addEntry(&fx, "hal.dll", 0xFFFFF80055200000ULL, 0x6000);
    link(&fx);

    ok("the first entry is found",
       usLdrFindModule(fx.block, "ntoskrnl.exe", &m));
    eqU64("with its base", m.base, 0xFFFFF80053C00000ULL);
    eqU64("and its size", m.size, 0x1249000);
    eqU64("and its entry point", m.entry, 0xFFFFF80053C00000ULL + 0x1000);

    ok("a later entry is found too",
       usLdrFindModule(fx.block, "hal.dll", &m));
    eqU64("with its own base", m.base, 0xFFFFF80055200000ULL);

    ok("a name that is not there is not found",
       !usLdrFindModule(fx.block, "win32k.sys", &m));
}

/*
 * A prefix must not match. This is the mistake that a length-blind comparison
 * makes, and it is the one that matters: `ntoskrnl` against `ntoskrnl.exe`
 * would silently pick the wrong module and every address after it would be
 * wrong in a way that still looks like an address.
 */
static void testPrefixesDoNotMatch(void) {
    Fixture fx;
    UsLdrModule m = { 0 };

    memset(&fx, 0, sizeof(fx));
    addEntry(&fx, "ntoskrnl.exe", 0xFFFFF80053C00000ULL, 0x1249000);
    link(&fx);

    ok("a shorter name does not match a longer one",
       !usLdrFindModule(fx.block, "ntoskrnl", &m));
    ok("nor does a longer one match a shorter",
       !usLdrFindModule(fx.block, "ntoskrnl.exe.backup", &m));
    ok("but the exact name does",
       usLdrFindModule(fx.block, "ntoskrnl.exe", &m));
}

static void testCaseDoesNotMatter(void) {
    Fixture fx;
    UsLdrModule m = { 0 };

    memset(&fx, 0, sizeof(fx));
    addEntry(&fx, "CLFS.SYS", 0xFFFFF8004D460000ULL, 0x7e000);
    link(&fx);

    ok("a lower case query finds an upper case name",
       usLdrFindModule(fx.block, "clfs.sys", &m));
    eqU64("with the right base", m.base, 0xFFFFF8004D460000ULL);
}

static void testStopsAtTheEnd(void) {
    Fixture fx;
    UsLdrModule m = { 0 };

    memset(&fx, 0, sizeof(fx));
    addEntry(&fx, "kdcom.dll", 0xFFFFF8004D400000ULL, 0xb000);
    link(&fx);

    /* The ring terminates the walk, so a name that is not in it costs one
     * entry and not a lap of memory. */
    ok("the walk ends at the head", !usLdrFindModule(fx.block, "missing.dll", &m));
}

/*
 * A list whose node does not point at another list node is not a list. Reading
 * on from there would produce addresses drawn from whatever is at that place,
 * so the walk stops and answers no.
 */
static void testRefusesABrokenList(void) {
    Fixture fx;
    UsLdrModule m = { 0 };

    memset(&fx, 0, sizeof(fx));
    addEntry(&fx, "ntoskrnl.exe", 0xFFFFF80053C00000ULL, 0x1249000);
    addEntry(&fx, "hal.dll", 0xFFFFF80055200000ULL, 0x6000);
    link(&fx);

    /* The first node points somewhere that is not the second entry. */
    wr64(fx.entries[0], 0x0000000000000010ULL);
    ok("a node pointing outside the list is refused",
       !usLdrFindModule(fx.block, "hal.dll", &m));

    /* A base that cannot be one. */
    memset(&fx, 0, sizeof(fx));
    addEntry(&fx, "ntoskrnl.exe", 0x10, 0x1249000);
    link(&fx);
    ok("an implausible base is refused",
       !usLdrFindModule(fx.block, "ntoskrnl.exe", &m));

    /* A size of zero is how an uninitialised entry reads. */
    memset(&fx, 0, sizeof(fx));
    addEntry(&fx, "ntoskrnl.exe", 0xFFFFF80053C00000ULL, 0);
    link(&fx);
    ok("a zero size is refused",
       !usLdrFindModule(fx.block, "ntoskrnl.exe", &m));
}

static void testRefusesNothing(void) {
    UsLdrModule m = { 0 };
    uint8_t block[0x100] = { 0 };

    ok("a null block is refused", !usLdrFindModule(NULL, "ntoskrnl.exe", &m));
    ok("a null name is refused", !usLdrFindModule(block, NULL, &m));
    ok("a null output is refused", !usLdrFindModule(block, "ntoskrnl.exe", NULL));
    /* A head that points at itself is an empty list. */
    ok("an empty list has no matches",
       !usLdrFindModule(block, "ntoskrnl.exe", &m));
}

int main(void) {
    testFindsTheNamedModule();
    testPrefixesDoNotMatch();
    testCaseDoesNotMatter();
    testStopsAtTheEnd();
    testRefusesABrokenList();
    testRefusesNothing();

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
