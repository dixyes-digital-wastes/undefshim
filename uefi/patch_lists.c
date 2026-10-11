/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * The volume the patch lists are on, and the files in the configured directory
 * of it
 */

#include <uefi.h>

#include "uefi/config.h"
#include "uefi/console.h"
#include "uefi/patch_lists.h"

/* A list this large is a mistake, not a list */
#define US_PATCH_FILE_MAX_BYTES (512U * 1024U)

/*
 * A path that names the volume itself would have us read every file in the
 * root as a list. That is a typo, not an instruction, and it is refused here
 * rather than diagnosed later
 */
static bool namesVolumeRoot(const char *path) {
    if (path[0] == '\0') {
        return true;
    }
    for (size_t i = 0; path[i] != '\0'; i++) {
        if (path[i] != '/' && path[i] != '\\' && path[i] != '.' && path[i] != ' ') {
            return false;
        }
    }
    return true;
}

static bool readFile(efi_file_handle_t *file, char **out, size_t *outLength) {
    efi_guid_t infoGUID = EFI_FILE_INFO_GUID;
    uintn_t infoSize = sizeof(efi_file_info_t);
    efi_file_info_t info;
    char *buffer;
    uintn_t got;

    if (EFI_ERROR(file->GetInfo(file, &infoGUID, &infoSize, &info))) {
        return false;
    }
    if (info.FileSize > US_PATCH_FILE_MAX_BYTES) {
        return false;
    }
    buffer = NULL;
    if (EFI_ERROR(BS->AllocatePool(LIP ? LIP->ImageDataType : EfiLoaderData,
                                   (uintn_t)info.FileSize + 1U, (void **)&buffer))) {
        return false;
    }
    got = (uintn_t)info.FileSize;
    if (got != 0 && EFI_ERROR(file->Read(file, &got, buffer))) {
        BS->FreePool(buffer);
        return false;
    }
    buffer[got] = '\0';
    *out = buffer;
    *outLength = (size_t)got;
    return true;
}

bool usPatchListDirOpen(UsPatchListDir *list, const char *path) {
    efi_guid_t sfsGUID = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    efi_simple_file_system_protocol_t *sfs = NULL;
    efi_handle_t volume;
    wchar_t wide[64];
    size_t i = 0;

    list->root = NULL;
    list->dir = NULL;
    if (namesVolumeRoot(path)) {
        usLogE("patch", "refusing the volume root as a list directory\n");
        return false;
    }
    volume = usConfigVolume();
    if (volume == NULL) {
        return false;
    }
    if (EFI_ERROR(BS->HandleProtocol(volume, &sfsGUID, (void **)&sfs)) || sfs == NULL) {
        return false;
    }
    if (EFI_ERROR(sfs->OpenVolume(sfs, &list->root)) || list->root == NULL) {
        return false;
    }

    for (; path[i] != '\0' && i + 1U < sizeof(wide) / sizeof(wide[0]); i++) {
        wide[i] = (wchar_t)(unsigned char)path[i];
    }
    wide[i] = 0;
    if (EFI_ERROR(list->root->Open(list->root, &list->dir, wide, EFI_FILE_MODE_READ, 0))
        || list->dir == NULL) {
        usLogE("patch", "no list directory %s\n", path);
        list->root->Close(list->root);
        list->root = NULL;
        return false;
    }
    return true;
}

bool usPatchListDirNext(UsPatchListDir *list, char *name, size_t nameBytes,
                        char **text, size_t *textBytes) {
    *text = NULL;
    *textBytes = 0;
    for (;;) {
        uintn_t bufSize = sizeof(efi_file_info_t);
        efi_file_info_t info;
        efi_status_t status = list->dir->Read(list->dir, &bufSize, &info);

        if (EFI_ERROR(status) || bufSize == 0) {
            return false;
        }
        if ((info.Attribute & EFI_FILE_DIRECTORY) != 0) {
            continue;
        }
        efi_file_handle_t *file = NULL;
        size_t k = 0;

        for (; info.FileName[k] != 0 && k + 1U < nameBytes; k++) {
            name[k] = (char)info.FileName[k];
        }
        name[k] = '\0';
        if (EFI_ERROR(list->dir->Open(list->dir, &file, info.FileName,
                                     EFI_FILE_MODE_READ, 0))
            || file == NULL) {
            usLogW("patch", "cannot open %s\n", name);
            continue;
        }
        /* A file that cannot be read at all is a machine to look at, not a
         * list to apply: say which one it was */
        if (readFile(file, text, textBytes) && *text != NULL) {
            file->Close(file);
            return true;
        }
        usLogW("patch", "cannot read %s, or it is larger than "
               US_VALUE("%u") " bytes\n", name, (unsigned)US_PATCH_FILE_MAX_BYTES);
        file->Close(file);
    }
}

void usPatchListDirClose(UsPatchListDir *list) {
    if (list->dir != NULL) {
        list->dir->Close(list->dir);
        list->dir = NULL;
    }
    if (list->root != NULL) {
        list->root->Close(list->root);
        list->root = NULL;
    }
}
