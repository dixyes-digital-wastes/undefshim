#include <uefi.h>

#include "core/patchapply.h"
#include "core/patchlist.h"
#include "uefi/src/config.h"
#include "uefi/src/console.h"
#include "uefi/src/patch_apply.h"

/* A list this large is a mistake, not a list. */
#define US_PATCH_FILE_MAX_BYTES (512U * 1024U)

/*
 * A path that names the volume itself would have us read every file in the
 * root as a list. That is a typo, not an instruction, and it is refused here
 * rather than diagnosed later.
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
    efi_guid_t infoGuid = EFI_FILE_INFO_GUID;
    uintn_t infoSize = sizeof(efi_file_info_t);
    efi_file_info_t info;
    char *buffer;
    uintn_t got;

    if (EFI_ERROR(file->GetInfo(file, &infoGuid, &infoSize, &info))) {
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

/*
 * The bytes a list's addresses refer to: the raw part of the text section, as
 * it is in the file and as it is in memory, which for this image are the same
 * bytes at the same relative place.
 */
static uint8_t *imageText(UsImage *image, uint32_t *outBytes) {
    const UsPeSection *text = usImageFindSection(image, ".text");

    if (text == NULL || text->rawSize == 0) {
        return NULL;
    }
    *outBytes = text->rawSize;
    return (uint8_t *)(uintptr_t)(image->base + text->virtualAddress);
}

static void applyOne(UsImage *image, const char *name, const char *text,
                     size_t length) {
    UsPatchSite *sites;
    UsPatchFile file;
    UsPatchStats stats;
    uint32_t textBytes = 0;
    uint8_t *code = imageText(image, &textBytes);
    uint32_t capacity = (uint32_t)(length / 8U) + 1U;

    memset(&file, 0, sizeof(file));
    memset(&stats, 0, sizeof(stats));
    if (code == NULL) {
        usConsolePuts("patch: no text in ");
        usConsolePuts(name);
        usConsolePuts("\n");
        return;
    }
    sites = NULL;
    if (EFI_ERROR(BS->AllocatePool(LIP ? LIP->ImageDataType : EfiLoaderData,
                                   (uintn_t)capacity * sizeof(UsPatchSite),
                                   (void **)&sites))) {
        return;
    }
    UsPatchStatus status = usPatchParse(text, (uint32_t)length, sites, capacity, &file);

    usConsolePuts("patch: ");
    usConsolePuts(name);
    if (status != UsPatchOk) {
        usConsolePuts(" not usable, status ");
        usConsolePutDec((uint64_t)status);
        usConsolePuts("\n");
        BS->FreePool(sites);
        return;
    }
    stats.files = 1;
    UsPatchApplyResult result = usPatchApplyFile(&file, sites, "ntoskrnl", code,
                                                 textBytes, &stats);
    if (result == UsPatchApplied) {
        usConsolePuts(" applied ");
        usConsolePutDec(stats.applied);
        usConsolePuts(" refused ");
        usConsolePutDec(stats.refused);
        usConsolePuts(" out of range ");
        usConsolePutDec(stats.outOfRange);
    } else if (result == UsPatchWrongBuild) {
        usConsolePuts(" is for another build");
    } else {
        usConsolePuts(" is for another image");
    }
    usConsolePuts("\n");
    BS->FreePool(sites);
}

void usPatchApplyLists(UsSession *session, UsImage *image) {
    efi_handle_t volume;
    efi_guid_t sfsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    efi_simple_file_system_protocol_t *sfs = NULL;
    efi_file_handle_t *root = NULL;
    efi_file_handle_t *dir = NULL;
    const char *path;

    if (session->config == NULL || image == NULL) {
        return;
    }
    path = session->config->patchDir;
    if (path == NULL || path[0] == '\0') {
        return;
    }
    if (namesVolumeRoot(path)) {
        usConsolePuts("patch: refusing the volume root as a list directory\n");
        return;
    }
    volume = usConfigVolume();
    if (volume == NULL) {
        return;
    }
    if (EFI_ERROR(BS->HandleProtocol(volume, &sfsGuid, (void **)&sfs)) || sfs == NULL) {
        return;
    }
    if (EFI_ERROR(sfs->OpenVolume(sfs, &root)) || root == NULL) {
        return;
    }

    wchar_t wide[64];
    size_t i = 0;
    for (; path[i] != '\0' && i + 1U < sizeof(wide) / sizeof(wide[0]); i++) {
        wide[i] = (wchar_t)(unsigned char)path[i];
    }
    wide[i] = 0;

    if (EFI_ERROR(root->Open(root, &dir, wide, EFI_FILE_MODE_READ, 0)) || dir == NULL) {
        usConsolePuts("patch: no list directory ");
        usConsolePuts(path);
        usConsolePuts("\n");
        root->Close(root);
        return;
    }

    uint32_t files = 0;
    for (;;) {
        uintn_t bufSize = sizeof(efi_file_info_t);
        efi_file_info_t info;
        efi_status_t status = dir->Read(dir, &bufSize, &info);

        if (EFI_ERROR(status) || bufSize == 0) {
            break;
        }
        if ((info.Attribute & EFI_FILE_DIRECTORY) != 0) {
            continue;
        }
        efi_file_handle_t *file = NULL;
        char *text = NULL;
        size_t length = 0;

        if (EFI_ERROR(dir->Open(dir, &file, info.FileName, EFI_FILE_MODE_READ, 0))
            || file == NULL) {
            continue;
        }
        if (readFile(file, &text, &length) && text != NULL) {
            char name[64];
            size_t k = 0;

            for (; info.FileName[k] != 0 && k + 1U < sizeof(name); k++) {
                name[k] = (char)info.FileName[k];
            }
            name[k] = '\0';
            files++;
            applyOne(image, name, text, length);
            BS->FreePool(text);
        }
        file->Close(file);
    }
    dir->Close(dir);
    root->Close(root);
    usConsolePuts("patch: ");
    usConsolePutDec(files);
    usConsolePuts(" list file(s) from ");
    usConsolePuts(path);
    usConsolePuts("\n");
}
