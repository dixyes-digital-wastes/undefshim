/*
 * The patch list parser, and the hash it checks files with
 *
 * The rule this test exists for: odd input may be refused, and may leave a
 * site out, but it must never walk off the buffer or claim a site it did not
 * read. So besides the format's corners the parser is fed garbage, at the
 * end of the buffer as well, where an off-by-one shows up as a crash under
 * the sanitizers this is also built with
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "core/patchapply.h"
#include "core/patchlist.h"
#include "core/sha256.h"

static unsigned checks;
static unsigned failures;

static void ok(const char *what, bool value) {
    checks++;
    if (!value) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static void eq32(const char *what, uint32_t got, uint32_t want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: want %u, got %u\n", what, want, got);
    }
}

static void eq64(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: want %#llx, got %#llx\n", what,
               (unsigned long long)want, (unsigned long long)got);
    }
}

static void hexOf(const char *hex, uint8_t *out, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++) {
        unsigned hi = 0, lo = 0;

        sscanf(hex + i * 2, "%1x", &hi);
        sscanf(hex + i * 2 + 1, "%1x", &lo);
        out[i] = (uint8_t)((hi << 4) | lo);
    }
}

static void testSha256(void) {
    static const struct {
        const char *text;
        uint32_t    length;
        const char *digest;
    } vectors[] = {
        { "", 0,
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", 3,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        /* One byte past a block, so the padding needs a block of its own */
        { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 64,
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
    };

    for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint8_t want[32];
        uint8_t got[32];
        UsSha256 ctx;

        hexOf(vectors[i].digest, want, 32);
        usSha256(vectors[i].text, vectors[i].length, got);
        checks++;
        if (memcmp(want, got, 32) != 0) {
            failures++;
            printf("FAIL sha256 vector %u\n", i);
        }
        /* Fed in pieces, the answer has to be the same */
        usSha256Init(&ctx);
        for (uint32_t at = 0; at < vectors[i].length; at++) {
            usSha256Update(&ctx, vectors[i].text + at, 1);
        }
        usSha256Final(&ctx, got);
        checks++;
        if (memcmp(want, got, 32) != 0) {
            failures++;
            printf("FAIL sha256 vector %u, byte at a time\n", i);
        }
    }
}

/* A file that uses everything the format has */
static const char *good =
    "# a list for one kernel\n"
    "\n"
    "USPATCHV1\n"
    "peFile ntoskrnl    # the only target there is\n"
    "\n"
    "textSHA256Hash 000102030405060708090a0b0c0d0e0f"
    "101112131415161718191a1b1c1d1e1f\n"
    "0x458b18 f8bfc3ea c8dfffea          # match and replace\n"
    "0x458b24 38bfc3ea 28dfffea FFFFFFFF # with an explicit mask\n"
    "0x458b30 c8dfffea                   # replace only\n";

static void testGood(void) {
    UsPatchSite sites[16];
    UsPatchFile file;
    /* f8bfc3ea, as the four bytes at the address */
    uint8_t memory[4] = { 0xea, 0xc3, 0xbf, 0xf8 };
    uint8_t digest[32];

    memset(&file, 0, sizeof(file));
    memset(sites, 0, sizeof(sites));
    eq32("a good file parses", usPatchParse(good, (uint32_t)strlen(good), sites,
                                          16U, &file), UsPatchOk);
    eq32("three sites", file.sites, 3U);
    eq32("nothing skipped", file.skipped, 0U);
    eq64("the target is named", file.targetLength, 8U);
    ok("and it is ntoskrnl", memcmp(file.target, "ntoskrnl", 8) == 0);
    eq64("the hash is read", file.hash[0], 0U);
    eq64("byte for byte", file.hash[31], 0x1fU);
    eq64("the first site's address", sites[0].rva, 0x458b18U);
    eq64("its width", sites[0].width, 4U);
    eq64("its match", sites[0].match[0], 0xeaU);
    eq64("its replacement", sites[0].replace[0], 0xeaU);
    eq64("a missing mask is all ones", sites[0].mask[0], 0xffU);
    eq64("an explicit mask is kept", sites[2 - 1].mask[3], 0xffU);
    eq64("the third site is replace only", sites[2].match[0], 0xffU);
    eq64("and its replacement", sites[2].replace[0], 0xeaU);

    ok("the memory matches", usPatchMatches(&sites[0], memory));
    memory[3] = 0x00;
    ok("and stops matching when it does not", !usPatchMatches(&sites[0], memory));
    memory[3] = 0xf8;
    usPatchWrite(&sites[0], memory);
    eq64("a write lands", memory[0], 0xeaU);
    eq64("all four bytes of it", memory[3], 0xc8U);
    (void)digest;
}

static void testRefusals(void) {
    UsPatchSite sites[8];
    UsPatchFile file;
    struct {
        const char *what;
        const char *text;
        UsPatchStatus want;
    } cases[] = {
        { "an empty file has no version", "", UsPatchUnsupported },
        { "and neither does a comment", "# nothing here\n", UsPatchUnsupported },
        { "a later version is refused", "USPATCHV2\n", UsPatchUnsupported },
        /* No matcher at all is refused: there would be nothing to check */
        { "a version and nothing else", "USPATCHV1\n", UsPatchNoMatchers },
        /* A matcher twice is a bad file, not a coincidence to resolve */
        { "two names",
          "USPATCHV1\npeFile ntoskrnl\npeFile winload\n", UsPatchDuplicate },
        { "two hashes",
          "USPATCHV1\ntextSHA256Hash "
          "0000000000000000000000000000000000000000000000000000000000000000\n"
          "textSHA256Hash "
          "1111111111111111111111111111111111111111111111111111111111111111\n",
          UsPatchDuplicate },
        { "two uuids",
          "USPATCHV1\npdbUUID 00010203-0405-0607-0809-0a0b0c0d0e0f-1\n"
          "pdbUUID 00010203-0405-0607-0809-0a0b0c0d0e0f-2\n", UsPatchDuplicate },
        /* One matcher on its own is enough to be checkable */
        { "a uuid alone is enough",
          "USPATCHV1\npdbUUID 00010203-0405-0607-0809-0a0b0c0d0e0f-1\n", UsPatchOk },
        { "a name alone is enough", "USPATCHV1\npeFile ntoskrnl\n", UsPatchOk },
        { "a short hash", "USPATCHV1\npeFile ntoskrnl\ntextSHA256Hash cafebabe\n",
          UsPatchNoHash },
        { "an unknown instruction",
          "USPATCHV1\npeFile ntoskrnl\ntextSHA256Hash "
          "0000000000000000000000000000000000000000000000000000000000000000\n"
          "patchEverything\n", UsPatchSyntax },
        { "a lone 0x",
          "USPATCHV1\npeFile ntoskrnl\ntextSHA256Hash "
          "0000000000000000000000000000000000000000000000000000000000000000\n"
          "0x\n", UsPatchSyntax },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(&file, 0, sizeof(file));
        UsPatchStatus got = usPatchParse(cases[i].text, (uint32_t)strlen(cases[i].text),
                                         sites, 8U, &file);

        eq32(cases[i].what, got, cases[i].want);
    }
}

/* Sites whose shape is wrong are left out; the file still applies */
static void testSkippedSites(void) {
    static const char *head =
        "USPATCHV1\npeFile ntoskrnl\ntextSHA256Hash "
        "0000000000000000000000000000000000000000000000000000000000000000\n";
    struct {
        const char *what;
        const char *body;
        uint32_t    kept;
        uint32_t    skipped;
    } cases[] = {
        { "an odd number of digits", "0x458b18 f8bfc3e c8dfffea\n", 0U, 1U },
        { "fields of different widths", "0x458b18 f8bfc3ea c8dfffea11\n", 0U, 1U },
        /* Ten bytes is inside the limit, whatever the address */
        { "a ten byte field",
          "0x458b18 f8bfc3eaf8bfc3eaf8 c8dfffeac8dfffeac8\n", 1U, 0U },
        { "an odd width", "0x458b19 f8bfc3 c8dfff\n", 1U, 0U },
        { "an odd address is no reason to refuse", "0x458b19 f8bfc3ea c8dfffea\n", 1U, 0U },
        { "so is an odd width at an odd address", "0x458b1b abcd12 1234ab\n", 1U, 0U },
        { "an address with no replacement", "0x458b18\n", 0U, 1U },
        { "a second site over the first",
          "0x458b18 f8bfc3ea c8dfffea\n0x458b1a f8bfc3ea c8dfffea\n", 1U, 1U },
        { "a good one after a bad one",
          "0x458b18 f8bfc3e c8dfffe\n0x458b18 f8bfc3ea c8dfffea\n", 1U, 1U },
        { "a two byte patch", "0x458b18 f8bf c8df\n", 1U, 0U },
        /* The second address has no fields at all, so it is left out */
        { "an address with nothing after it",
          "0x458b18 cafebabe 0x1234\n", 1U, 1U },
        { "sixteen bytes are allowed",
          "0x458b40 000102030405060708090a0b0c0d0e0f\n"
          "1112131415161718191a1b1c1d1e1f20\n", 1U, 0U },
        { "seventeen are not",
          "0x458b40 000102030405060708090a0b0c0d0e0f10\n"
          "1112131415161718191a1b1c1d1e1f2021\n", 0U, 1U },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char text[512];
        UsPatchSite sites[8];
        UsPatchFile file;

        snprintf(text, sizeof(text), "%s%s", head, cases[i].body);
        memset(&file, 0, sizeof(file));
        eq32(cases[i].what, usPatchParse(text, (uint32_t)strlen(text), sites, 8U,
                                         &file), UsPatchOk);
        eq32("  sites kept", file.sites, cases[i].kept);
        eq32("  sites left out", file.skipped, cases[i].skipped);
    }
}

static void testCapacity(void) {
    char text[2048];
    UsPatchSite sites[2];
    UsPatchFile file;
    uint32_t at = 0;

    at += (uint32_t)snprintf(text + at, sizeof(text) - at,
                             "USPATCHV1\npeFile ntoskrnl\ntextSHA256Hash "
                             "0000000000000000000000000000000000000000000000000000000000000000\n");
    for (unsigned i = 0; i < 4; i++) {
        at += (uint32_t)snprintf(text + at, sizeof(text) - at,
                                 "0x45%04x f8bfc3ea c8dfffea\n", 0x8b00U + i * 4U);
    }
    eq32("a file that does not fit is refused",
         usPatchParse(text, at, sites, 2U, &file), UsPatchTooManySites);
}

/* Garbage in, a status out: never a crash, never a site nobody read */
static void testGarbage(void) {
    UsPatchSite sites[4];
    UsPatchFile file;
    uint32_t seed = 12345U;
    char text[96];

    for (unsigned round = 0; round < 3000U; round++) {
        uint32_t length = 0;

        seed = seed * 1103515245U + 12345U;
        length = (seed >> 16) % (sizeof(text) - 1U);
        for (uint32_t i = 0; i < length; i++) {
            seed = seed * 1103515245U + 12345U;
            text[i] = (char)((seed >> 16) & 0xffU);
        }
        text[length] = '\0';
        memset(&file, 0, sizeof(file));
        UsPatchStatus status = usPatchParse(text, length, sites, 4U, &file);

        checks++;
        if (status != UsPatchOk) {
            continue;
        }
        for (uint32_t i = 0; i < file.sites; i++) {
            if (sites[i].width == 0U || sites[i].width > US_PATCH_MAX_WIDTH) {
                failures++;
                printf("FAIL garbage produced a site of width %u\n", sites[i].width);
            }
        }
    }

    /* Pieces of a valid file cut short at every length: the same rule */
    for (uint32_t cut = 0; cut <= strlen(good); cut++) {
        memset(&file, 0, sizeof(file));
        (void)usPatchParse(good, cut, sites, 4U, &file);
    }
    ok("truncations and garbage all returned", true);
}

/*
 * Applying a list: which image it is for, which build of it, and whether the
 * bytes are still the ones the list was written against
 */
static void testApply(void) {
    static const char *file =
        "USPATCHV1\n"
        "peFile ntoskrnl\n"
        "textSHA256Hash %s\n"
        "0x4 f8bfc3ea c8dfffea\n"        /* will match and be written */
        "0x8 00000000 deadbeef\n"        /* bytes are not what it says */
        "0x100 f8bfc3ea c8dfffea\n";     /* past the end of the text */
    uint8_t text[16] = { 1, 2, 3, 4, 0xea, 0xc3, 0xbf, 0xf8, 9, 9, 9, 9, 9, 9, 9, 9 };
    uint8_t digest[32];
    char document[512];
    char hex[65];
    UsPatchSite sites[8];
    UsPatchFile parsed;
    UsPatchStats stats;

    usSha256(text, sizeof(text), digest);
    for (unsigned i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    hex[64] = '\0';
    snprintf(document, sizeof(document), file, hex);
    memset(&parsed, 0, sizeof(parsed));
    eq32("the file parses", usPatchParse(document, (uint32_t)strlen(document), sites,
                                        8U, &parsed), UsPatchOk);
    memset(&stats, 0, sizeof(stats));
    eq32("it applies", usPatchApplyFile(&parsed, sites, "ntoskrnl.exe", 0U, text,
                                        sizeof(text), &stats), UsPatchApplied);
    eq32("one site written", stats.applied, 1U);
    eq32("one site refused for its bytes", stats.refused, 1U);
    eq32("one site past the end", stats.outOfRange, 1U);
    eq64("the instruction is replaced", text[4], 0xeaU);
    eq64("all four bytes of it", text[7], 0xc8U);
    eq64("and nothing else was touched", text[8], 9U);

    /* The same list against another build: the hash says no */
    text[0] = 0xff;
    memset(&stats, 0, sizeof(stats));
    eq32("another build is refused",
         usPatchApplyFile(&parsed, sites, "ntoskrnl", 0U, text, sizeof(text), &stats),
         UsPatchWrongBuild);
    eq32("and nothing is written for it", stats.applied, 0U);
    text[0] = 1;

    /* Another image entirely */
    memset(&stats, 0, sizeof(stats));
    eq32("another image is not this list's",
         usPatchApplyFile(&parsed, sites, "winload.efi", 0U, text, sizeof(text), &stats),
         UsPatchNotThisImage);
    ok("the stem decides", usPatchTargetMatches(&parsed, "NTOSKRNL.EFI"));
    ok("and a different stem does not", !usPatchTargetMatches(&parsed, "ntoskrnl2"));
}

int main(void) {
    testSha256();
    testApply();
    testGood();
    testRefusals();
    testSkippedSites();
    testCapacity();
    testGarbage();

    printf("patchlist: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
