/*
 * SHA-256, for checking that a patch list was written for the binary it is
 * about to be applied to. The structure follows the implementation musl
 * carries: no allocation, no libc, usable from the driver and from the
 * payload
 */
#ifndef US_SHA256_H
#define US_SHA256_H

#include <stdint.h>

#define US_SHA256_DIGEST 32U

typedef struct {
    uint32_t state[8];
    uint64_t bytes;                 /* total length, for the padding */
    uint8_t  block[64];
    uint32_t used;                  /* bytes buffered in block */
} UsSHA256;

void usSHA256Init(UsSHA256 *ctx);
void usSHA256Update(UsSHA256 *ctx, const void *data, uint32_t length);
void usSHA256Final(UsSHA256 *ctx, uint8_t out[US_SHA256_DIGEST]);

/* The whole thing in one call, for short inputs */
void usSHA256(const void *data, uint32_t length, uint8_t out[US_SHA256_DIGEST]);

#endif
