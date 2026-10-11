/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The volume the patch lists are on, and the files in the configured directory
 * of it
 *
 * A path in the configuration is a byte string, and the firmware takes UTF-16,
 * so the conversion happens here and nowhere else. Everything the filesystem
 * can refuse is reported here as well: what a caller gets back is a list it can
 * read, and the ones it cannot were said out loud on the way
 */

#ifndef US_UEFI_PATCH_LISTS_H
#define US_UEFI_PATCH_LISTS_H

#include <stdbool.h>
#include <stddef.h>

#include <uefi.h>

/*
 * A name longer than this is truncated: it is only ever printed, and the file
 * it names is already open by the handle that came with it
 */
#define US_PATCH_LIST_NAME_BYTES 64U

typedef struct UsPatchListDir_t {
    efi_file_handle_t *root;
    efi_file_handle_t *dir;
} UsPatchListDir;

/*
 * Opens the directory a configuration path names on the configuration's own
 * volume. A path that names the volume itself is refused: it would have every
 * file in the root read as a list, which is a typo and not an instruction
 *
 * Returns false when there is no such directory, and leaves nothing to close
 */
bool usPatchListDirOpen(UsPatchListDir *list, const char *path);

/*
 * The next readable list in it: the name is copied into name, and the bytes are
 * allocated for the caller to free. False when the directory is exhausted
 *
 * Entries that cannot be opened or read are reported here and skipped, which is
 * what makes a bad file a message rather than the end of the run
 */
bool usPatchListDirNext(UsPatchListDir *list, char *name, size_t nameBytes,
                        char **text, size_t *textBytes);

void usPatchListDirClose(UsPatchListDir *list);

#endif
