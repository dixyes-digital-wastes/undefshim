/*
 * The answer a translation attempt gives back.
 *
 * Asking the hardware to translate an address does not read the translation
 * tables: the answer arrives in a register, describing what that address
 * resolves to at this moment. That is the only way to learn a mapping when
 * the tables themselves are not reachable, which is the situation at the
 * handover.
 *
 * The register is a pack of fields, and the one that matters is where the
 * result begins: bit 0 says whether the attempt succeeded, and the failure
 * case has a status in the low bits where the address would otherwise be. So
 * a decoder is worth having on its own, and is worth testing without any
 * hardware: the encodings are fixed by the architecture and can be written
 * down.
 */

#ifndef US_PAR_H
#define US_PAR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct UsPar_t {
    /* Whether the translation succeeded. */
    bool     valid;
    /*
     * The physical address it resolved to, page aligned. The low bits of the
     * page are the same as the virtual address's, so a caller that needs the
     * exact address adds them back.
     */
    uint64_t pa;
    /* When it failed, the reason, as the architecture encodes it. */
    uint32_t fault;
    /* Whether the answer came from long descriptor tables. */
    bool     lpae;
    /* The memory attributes, of interest only for what they say about
     * whether the page can be executed. */
    uint32_t attributes;
} UsPar;

/*
 * Decodes a translation result.
 *
 * The address field is the one place the encoding is worth stating: it is
 * bits 12 to 47, not the whole register. Comparing the raw register against a
 * physical address never matches, because the attributes sit above the
 * address and the status below it. The wider field a 52 bit physical address
 * would use is not read: nothing here is placed that high.
 */
UsPar usParDecode(uint64_t par);

/*
 * The physical address a translation attempt reported, or 0 when it failed.
 * Provided because most callers want only this.
 */
uint64_t usParPa(uint64_t par);

#endif
