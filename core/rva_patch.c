/*
 * Writing bytes into a loaded image, see rva_patch.h for the contract.
 */

#include "core/rva_patch.h"

const char *usPatchResultName(UsPatchResult r) {
    switch (r) {
    case UsPatchOk:
        return "ok";
    case UsPatchOutOfRange:
        return "address outside the image";
    case UsPatchBadWidth:
        return "unsupported width";
    }
    return "unknown";
}

/*
 * The bytes a patch is made of, little endian.
 *
 * Order matters here in a way that is easy to get wrong: a patch written the
 * wrong way round still writes successfully, it just writes something else,
 * and the mistake only shows up as a crash much later.
 */
static void encodeLittleEndian(uint64_t value, uint8_t width, uint8_t *out) {
    for (uint8_t i = 0; i < width; i++) {
        out[i] = (uint8_t)(value >> (8 * i));
    }
}

UsPatchResult usPatchApply(UsImage *img, const UsPatchSpec *spec, UsPatchRange *out) {
    uint8_t bytes[8];
    size_t available = 0;
    uint8_t *at;

    if (spec->width != 1 && spec->width != 2 && spec->width != 4 && spec->width != 8) {
        return UsPatchBadWidth;
    }

    at = (uint8_t *)usImageRvaSpan(img, spec->rva, &available);
    if (at == NULL || available < spec->width) {
        return UsPatchOutOfRange;
    }

    encodeLittleEndian(spec->value, spec->width, bytes);
    for (uint8_t i = 0; i < spec->width; i++) {
        at[i] = bytes[i];
    }

    if (out != NULL) {
        out->addr = at;
        out->bytes = spec->width;
    }
    return UsPatchOk;
}
