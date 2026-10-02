/*
 * Decoding a translation result, see par.h.
 */

#include "core/par.h"

/*
 * Where the fields sit.
 *
 * The register packs three things: a status below the address, the address
 * itself, and the attributes above it. The address field is 36 bits wide,
 * which is what a 4KB granule translation of a 48 bit physical address
 * produces; the wider field a 52 bit address would use is not read, because
 * nothing here is placed that high.
 */
#define US_PAR_FAIL (1ULL << 0)
#define US_PAR_ADDR_MASK 0x0000FFFFFFFFF000ULL
#define US_PAR_FAULT_MASK 0x3FU
#define US_PAR_FAULT_SHIFT 1
#define US_PAR_LPAE (1ULL << 11)
#define US_PAR_ATTR_MASK 0xFFU
#define US_PAR_ATTR_SHIFT 56

UsPar usParDecode(uint64_t par) {
    UsPar out = { 0 };

    if ((par & US_PAR_FAIL) != 0) {
        out.valid = false;
        out.fault = (uint32_t)((par >> US_PAR_FAULT_SHIFT) & US_PAR_FAULT_MASK);
        return out;
    }

    out.valid = true;
    out.pa = par & US_PAR_ADDR_MASK;
    out.lpae = (par & US_PAR_LPAE) != 0;
    out.attributes = (uint32_t)((par >> US_PAR_ATTR_SHIFT) & US_PAR_ATTR_MASK);
    return out;
}

uint64_t usParPa(uint64_t par) {
    UsPar decoded = usParDecode(par);

    return decoded.valid ? decoded.pa : 0;
}
