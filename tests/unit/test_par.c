/*
 * Checks for the translation result decoder
 *
 * The register packs a status, an address and attributes, and a decoder that
 * takes the whole register for an address is the mistake worth guarding
 * against: it never matches, and it fails silently, because the value it
 * produces looks like an address
 *
 * The encodings here are written from the architecture rather than captured
 * from a run, so the test says what each field means rather than what one
 * machine happened to produce
 */

#include <stdio.h>

#include "core/par.h"

#define US_CHECK_NAME "test_par"
#include "check.h"

static int failures;
static int checks;

static void ok(const char *name, int cond) {
    checks++;
    if (!cond) {
        failures++;
        usCheckFail("%s\n", name);
    }
}

static void eqU64(const char *name, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        failures++;
        usCheckFail("%-42s want 0x%llx got 0x%llx\n", name,
               (unsigned long long)want, (unsigned long long)got);
    }
}

static void testASuccess(void) {
    /* A translation that worked, of a page at 0x13bc00000, on long
     * descriptor tables, ordinary memory */
    uint64_t par = 0x13bc00000ULL | (1ULL << 11) | (0xFFULL << 1);
    UsPar p = usParDecode(par);

    ok("a successful translation is recognised", p.valid);
    eqU64("and reports the address", p.pa, 0x13bc00000ULL);
    ok("and says the tables were long descriptors", p.lpae);
    eqU64("no fault is reported", p.fault, 0);

    /* The attributes sit above the address and must not leak into it */
    par = (0xF0ULL << 56) | 0x20000000ULL | (1ULL << 11);
    p = usParDecode(par);
    ok("attributes do not confuse the address", p.valid);
    eqU64("the address is still just the address", p.pa, 0x20000000ULL);
    eqU64("and the attributes are reported separately", p.attributes, 0xF0);
}

static void testAFailure(void) {
    /* The failure bit set, with a fault status in the low bits */
    uint64_t par = 1ULL | (0x07ULL << 1);
    UsPar p = usParDecode(par);

    ok("a failed translation is not valid", !p.valid);
    eqU64("with the reason it gives", p.fault, 0x07);
    eqU64("and no address", p.pa, 0);

    /* A fault status must not be mistaken for an address, which is what a
     * decoder that used the whole register would do */
    par = 1ULL | (0x3FULL << 1);
    p = usParDecode(par);
    ok("the largest fault status is still a failure", !p.valid);
    ok("and produces no address", p.pa == 0);
}

/*
 * The address field is 36 bits, which is what a 48 bit physical address needs
 * at a 4KB granule. Anything wider is picking up attributes; anything
 * narrower would truncate a real address
 */
static void testAddressWidth(void) {
    uint64_t par;
    UsPar p;

    /* The widest address the field can hold */
    par = 0xFFFFFFFFFULL << 12;
    p = usParDecode(par);
    ok("a full width address is kept whole", p.valid);
    eqU64("with nothing above it lost", p.pa, 0xFFFFFFFFF000ULL);

    /* A bit above the field belongs to the attributes, not the address */
    par = 0x1000000000ULL << 12;
    p = usParDecode(par);
    eqU64("a bit above the field is not part of the address", p.pa, 0);
}

int main(void) {
    testASuccess();
    testAFailure();
    testAddressWidth();

    return usCheckSummary(checks, failures);
}
