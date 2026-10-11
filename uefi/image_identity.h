/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Which build an image is, read out of the image
 *
 * An image is a PE view, the record it carries is bytes, and the digest is
 * arithmetic over them: nothing here knows about firmware, so a host test can
 * compile this file on its own
 */

#ifndef US_UEFI_IMAGE_IDENTITY_H
#define US_UEFI_IMAGE_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#include "core/patchapply.h"
#include "core/pe.h"

/* A uuid as text: thirty-two digits, four hyphens and a terminator */
#define US_IMAGE_GUID_CHARS (16U * 2U + 4U + 1U)

/* A sha256 as text: sixty-four digits and a terminator */
#define US_IMAGE_HASH_CHARS (32U * 2U + 1U)

/*
 * The bytes a list's addresses refer to: the raw part of the text section, as
 * it is in the file and as it is in memory, which for these images are the same
 * bytes at the same relative place
 *
 * NULL when the image has no text, which is a refusal rather than an empty
 * answer
 */
uint8_t *usImageTextBytes(UsImage *image, uint32_t *outBytes);

/*
 * The digest of that text, with every byte the loader rewrites for a relocation
 * left out: those bytes are a function of where the image was loaded and differ
 * from boot to boot, so a digest that names the build has to leave them out
 */
void usImageTextDigest(UsImage *image, const uint8_t *code, uint32_t bytes,
                       uint8_t out[32]);

/*
 * The build's identity as the image itself carries it: the debug directory's
 * CodeView entry holds the program database's GUID and age
 */
bool usImageIdentity(UsImage *image, UsPatchIdentity *out);

/*
 * A uuid as a debugger prints it, which is how a list and a symbol path both
 * write it: the sixteen bytes with hyphens after the fourth, sixth, eighth and
 * tenth, in the order the caller hands them over
 */
void usImageFormatGUID(const uint8_t *bytes, char out[US_IMAGE_GUID_CHARS]);

/*
 * The first three fields of a uuid are numbers: the record holds them least
 * significant byte first, and everything that prints one writes them the other
 * way. The eight bytes after them are a byte string and do not move
 */
void usImageGUIDTurnedRound(const uint8_t *record, uint8_t out[16]);

void usImageFormatHash(const uint8_t *bytes, char out[US_IMAGE_HASH_CHARS]);

#endif
