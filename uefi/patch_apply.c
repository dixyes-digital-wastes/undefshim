#include <uefi.h>

#include "core/patchapply.h"
#include "core/patchlist.h"

/* Little endian fields, as they are in the image */
static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

static uint32_t rd16le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
#include "core/sha256.h"
#include "uefi/config.h"
#include "uefi/console.h"
#include "uefi/patch_apply.h"

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

/*
 * The bytes a list's addresses refer to: the raw part of the text section, as
 * it is in the file and as it is in memory, which for this image are the same
 * bytes at the same relative place
 */
static uint8_t *imageText(UsImage *image, uint32_t *outBytes) {
    const UsPESection *text = usImageFindSection(image, ".text");

    if (text == NULL || text->rawSize == 0) {
        return NULL;
    }
    *outBytes = text->rawSize;
    return (uint8_t *)(uintptr_t)(image->base + text->virtualAddress);
}

/* Defined below, used by the application above it */
static void digestText(UsImage *image, uint8_t *code, uint32_t bytes, uint8_t out[32]);

/*
 * The build's identity, as the image itself carries it: the debug directory's
 * CodeView entry holds the program database's GUID and age. Those bytes are
 * the same in the file and in memory - nothing relocates them and the loader
 * does not patch them - so a list written from the file can be checked
 * against the running image without ever having run it
 */
static bool imageIdentity(UsImage *image, UsPatchIdentity *out) {
    uint32_t size = 0;
    const uint8_t *table = usImageDataDirectory(image, 6U, &size);

    if (table == NULL) {
        return false;
    }
    for (uint32_t at = 0; at + 28U <= size; at += 28U) {
        uint32_t type = rd32le(table + at + 12);
        uint32_t bytes = rd32le(table + at + 16);
        uint32_t rva = rd32le(table + at + 20);
        const uint8_t *record;

        if (type != 2U || bytes < 24U) {
            continue;
        }
        record = usImageRVAToPtr(image, rva);
        if (record == NULL || rd32le(record) != 0x53445352U) {   /* RSDS */
            continue;
        }
        for (uint32_t i = 0; i < 16U; i++) {
            out->guid[i] = record[4U + i];
        }
        out->age = rd32le(record + 20U);
        return true;
    }
    return false;
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
        usLogE("patch", "no text in %s\n", name);
        return;
    }
    sites = NULL;
    if (EFI_ERROR(BS->AllocatePool(LIP ? LIP->ImageDataType : EfiLoaderData,
                                   (uintn_t)capacity * sizeof(UsPatchSite),
                                   (void **)&sites))) {
        return;
    }
    UsPatchStatus status = usPatchParse(text, (uint32_t)length, sites, capacity, &file);

    if (status != UsPatchOk) {
        usLogE("patch", "%s not usable, status " US_VALUE("%u") "\n", name,
               (unsigned)status);
        BS->FreePool(sites);
        return;
    }
    uint8_t digest[32];
    UsPatchIdentity identity;
    UsPatchMatchers matchers;

    digestText(image, code, textBytes, digest);
    matchers.imageName = "ntoskrnl";
    matchers.digest = digest;
    matchers.identity = imageIdentity(image, &identity) ? &identity : NULL;
    stats.files = 1;
    /*
     * Sites are image RVAs and they are not all in .text: the kernel keeps
     * plenty of instructions in paged sections that are executed just the
     * same, and a list written from the file names those too. So the range a
     * site is checked against is the whole image, and what keeps a write from
     * landing somewhere it should not is the match against the bytes already
     * there, which every site carries
     */
    UsPatchApplyResult result = usPatchApplyMatched(&file, sites, &matchers, 0U,
                                                    (uint8_t *)(uintptr_t)image->base,
                                                    image->sizeOfImage, &stats);
    if (result != UsPatchApplied) {
        usLogW("patch", "%s is for another %s\n", name,
               result == UsPatchWrongBuild ? "build" : "image");
        BS->FreePool(sites);
        return;
    }
    usLogI("patch", "%s applied " US_VALUE("%u") " refused " US_VALUE("%u")
           " out of range " US_VALUE("%u") "\n",
           name, (unsigned)stats.applied, (unsigned)stats.refused,
           (unsigned)stats.outOfRange);
    BS->FreePool(sites);
}

/* Most a kernel's relocation table is going to hold. Anything past this is
 * not counted, which can only make the digest stricter, never wrong */
#define US_PATCH_MAX_RELOCATIONS 65536U

static uint32_t relocations[US_PATCH_MAX_RELOCATIONS];
static uint32_t relocationCount;

/*
 * The loader writes each relocated address into the image at the offsets the
 * relocation table names, so those bytes are a function of where the image
 * was loaded and differ from boot to boot. Everything else in the text is the
 * build. A digest that has to be the same every time therefore has to leave
 * them out, and it has to leave out exactly what the list's author did
 */
static void collectRelocations(UsImage *image, uint32_t textRVA, uint32_t textBytes) {
    uint32_t size = 0;
    const uint8_t *table;

    relocationCount = 0;
    table = usImageDataDirectory(image, US_PE_DIRECTORY_RELOCATIONS, &size);
    if (table == NULL) {
        return;
    }
    for (uint32_t at = 0; at + 8U <= size;) {
        uint32_t page = rd32le(table + at);
        uint32_t blockSize = rd32le(table + at + 4);

        if (blockSize < 8U || at + blockSize > size) {
            break;
        }
        for (uint32_t entry = at + 8U; entry + 2U <= at + blockSize; entry += 2U) {
            uint32_t value = (uint32_t)rd16le(table + entry);
            uint32_t type = value >> 12;
            uint32_t rva = page + (value & 0xfffU);

            /* Every target is skipped by the same width, whatever the entry
             * says it is: the two sides of the comparison only have to agree
             * with each other, and a wider skip is the safer agreement */
            (void)type;
            if (rva < textRVA || rva >= textRVA + textBytes) {
                continue;
            }
            if (relocationCount < US_PATCH_MAX_RELOCATIONS) {
                relocations[relocationCount++] = rva - textRVA;
            }
        }
        at += blockSize;
    }
    /* Sorted, so the digest can be fed the gaps in one pass */
    for (uint32_t i = 1; i < relocationCount; i++) {
        uint32_t value = relocations[i];
        uint32_t j = i;

        while (j > 0 && relocations[j - 1] > value) {
            relocations[j] = relocations[j - 1];
            j--;
        }
        relocations[j] = value;
    }
}

/* The digest the driver prints and the lists carry */
static void digestText(UsImage *image, uint8_t *code, uint32_t bytes, uint8_t out[32]) {
    UsSHA256 ctx;
    uint32_t at = 0;

    collectRelocations(image, (uint32_t)((uintptr_t)code - (uintptr_t)image->base), bytes);
    usSHA256Init(&ctx);
    for (uint32_t i = 0; i < relocationCount; i++) {
        uint32_t start = relocations[i];
        uint32_t end = start + 8U;

        if (start > bytes) {
            break;
        }
        if (end > bytes) {
            end = bytes;
        }
        if (start > at) {
            usSHA256Update(&ctx, code + at, start - at);
        }
        if (end > at) {
            at = end;
        }
    }
    if (bytes > at) {
        usSHA256Update(&ctx, code + at, bytes - at);
    }
    usSHA256Final(&ctx, out);
}

/*
 * Printing the digest of the image's text is how a list gets written for this
 * build rather than for a file that looks like it: the two are not always the
 * same binary, and a list that says "another build" is the check working
 */
static void reportTextHash(UsImage *image) {
    static const char digits[] = "0123456789abcdef";
    uint32_t bytes = 0;
    uint8_t *code = imageText(image, &bytes);
    uint8_t digest[32];
    char hex[sizeof(digest) * 2U + 1U];

    if (code == NULL) {
        return;
    }
    digestText(image, code, bytes, digest);
    for (uint32_t i = 0; i < sizeof(digest); i++) {
        hex[i * 2U] = digits[digest[i] >> 4];
        hex[i * 2U + 1U] = digits[digest[i] & 0xfU];
    }
    hex[sizeof(hex) - 1U] = '\0';
    usLogD("patch", "text sha256 %s\n", hex);
}

void usPatchApplyLists(UsSession *session, UsImage *image) {
    efi_handle_t volume;
    efi_guid_t sfsGUID = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
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
        usLogE("patch", "refusing the volume root as a list directory\n");
        return;
    }
    volume = usConfigVolume();
    if (volume == NULL) {
        return;
    }
    reportTextHash(image);
    if (EFI_ERROR(BS->HandleProtocol(volume, &sfsGUID, (void **)&sfs)) || sfs == NULL) {
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
        usLogE("patch", "no list directory %s\n", path);
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
    usLogV("patch", US_VALUE("%u") " list file(s) from %s\n", (unsigned)files, path);
}
