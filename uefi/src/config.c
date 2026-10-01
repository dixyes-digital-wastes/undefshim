/*
 * Locates, reads and parses the configuration file.
 *
 * See config.h for the policy. Every step here can fail, and the failure
 * modes are deliberately distinct: no file is a normal outcome, a file that
 * will not parse is not.
 */

#include <uefi.h>

#include "uefi/src/config.h"

/* A configuration this large is a mistake, not a configuration. */
#define US_CONFIG_MAX_BYTES (64 * 1024)

static char toLowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static char toUpperAscii(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

static size_t nameLen(const wchar_t *name) {
    size_t n = 0;
    while (name[n] != 0) {
        n++;
    }
    return n;
}

/* Case insensitive comparison of a firmware path against an ASCII literal. */
static bool nameEq(const wchar_t *name, const char *literal) {
    size_t i = 0;
    for (; literal[i] != '\0'; i++) {
        if (name[i] == 0 || toLowerAscii((char)name[i]) != toLowerAscii(literal[i])) {
            return false;
        }
    }
    return name[i] == 0;
}

static bool nameStartsWith(const wchar_t *name, const char *prefix) {
    for (size_t i = 0; prefix[i] != '\0'; i++) {
        if (name[i] == 0 || toUpperAscii((char)name[i]) != toUpperAscii(prefix[i])) {
            return false;
        }
    }
    return true;
}

static bool nameEndsWith(const wchar_t *name, const char *suffix) {
    size_t nameLength = nameLen(name);
    size_t suffixLength = 0;
    while (suffix[suffixLength] != '\0') {
        suffixLength++;
    }
    if (nameLength < suffixLength) {
        return false;
    }
    const wchar_t *tail = name + (nameLength - suffixLength);
    for (size_t i = 0; i < suffixLength; i++) {
        if (toUpperAscii((char)tail[i]) != toUpperAscii(suffix[i])) {
            return false;
        }
    }
    return true;
}

/*
 * Accepts the long name and the 8.3 name FAT generates for it. The numeric tail
 * of a short name depends on what else shares the volume, so only the prefix
 * and extension are matched.
 */
static bool isConfigName(const wchar_t *name) {
    if (nameEq(name, "undefshim.toml")) {
        return true;
    }
    return nameStartsWith(name, "UNDEFS~") && nameEndsWith(name, ".TOM");
}

static void *alloc(uintn_t size) {
    void *p = NULL;
    if (EFI_ERROR(BS->AllocatePool(LIP ? LIP->ImageDataType : EfiLoaderData, size, &p))) {
        return NULL;
    }
    return p;
}

static void dealloc(void *p) {
    if (p != NULL) {
        BS->FreePool(p);
    }
}

/*
 * Reads a whole file into a freshly allocated, NUL terminated buffer. The
 * caller owns it.
 */
static char *readFile(efi_file_handle_t *file, size_t *outLen) {
    efi_guid_t infoGuid = EFI_FILE_INFO_GUID;
    uintn_t infoSize = sizeof(efi_file_info_t);
    efi_file_info_t info;
    char *buf;
    size_t size;

    if (EFI_ERROR(file->GetInfo(file, &infoGuid, &infoSize, &info))) {
        return NULL;
    }
    if (info.FileSize > US_CONFIG_MAX_BYTES) {
        return NULL;
    }

    size = (size_t)info.FileSize;
    buf = alloc((uintn_t)(size + 1));
    if (buf == NULL) {
        return NULL;
    }

    uintn_t got = (uintn_t)size;
    if (size != 0 && EFI_ERROR(file->Read(file, &got, buf))) {
        dealloc(buf);
        return NULL;
    }

    buf[got] = '\0';
    *outLen = (size_t)got;
    return buf;
}

/*
 * Looks for the configuration file in one volume's root directory. Returns the
 * parsed config, or NULL when this volume has no candidate.
 */
static UsConfig *tryVolume(efi_handle_t handle, char *msg, size_t msgLen) {
    efi_guid_t sfsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    efi_simple_file_system_protocol_t *sfs = NULL;
    efi_file_handle_t *root = NULL;
    efi_file_handle_t *file = NULL;
    UsConfig *cfg = NULL;
    char *text = NULL;
    size_t textLen = 0;

    if (EFI_ERROR(BS->HandleProtocol(handle, &sfsGuid, (void **)&sfs)) || sfs == NULL) {
        return NULL;
    }
    if (EFI_ERROR(sfs->OpenVolume(sfs, &root)) || root == NULL) {
        return NULL;
    }

    /* Enumerating rather than opening a fixed list of names: the 8.3 name FAT
     * derives from the long name is not predictable, and the firmware may
     * expose either form. */
    for (;;) {
        uintn_t bufSize = sizeof(efi_file_info_t);
        efi_file_info_t info;
        efi_status_t status = root->Read(root, &bufSize, &info);

        if (EFI_ERROR(status) || bufSize == 0) {
            break;
        }
        if (isConfigName(info.FileName)) {
            status = root->Open(root, &file, info.FileName, EFI_FILE_MODE_READ, 0);
            if (!EFI_ERROR(status) && file != NULL) {
                break;
            }
            file = NULL;
        }
    }

    if (file != NULL) {
        text = readFile(file, &textLen);
        file->Close(file);
    }
    root->Close(root);

    if (text == NULL) {
        return NULL;
    }

    {
        char err[192] = { 0 };
        cfg = usConfigParse(text, textLen, err, sizeof(err));
        if (cfg == NULL) {
            snprintf(msg, msgLen, "%s", err[0] != '\0' ? err : "config parse failed");
        }
    }
    dealloc(text);
    return cfg;
}

UsConfigLoad usConfigLoad(UsConfig **out, char *msg, size_t msgLen) {
    efi_guid_t sfsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    efi_handle_t *handles = NULL;
    uintn_t count = 0;
    UsConfig *cfg = NULL;

    *out = NULL;
    msg[0] = '\0';

    if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &sfsGuid, NULL, &count, &handles))) {
        /* No simple file system at all: nothing to read, defaults apply. */
        return UsConfigAbsent;
    }

    /* The volume the driver came from is tried first so that the common case
     * does not depend on the order the firmware happens to report handles. */
    efi_handle_t preferred = LIP != NULL ? LIP->DeviceHandle : NULL;

    for (int pass = 0; pass < 2 && cfg == NULL; pass++) {
        for (uintn_t i = 0; i < count; i++) {
            if (pass == 0 && handles[i] != preferred) {
                continue;
            }
            if (pass == 1 && handles[i] == preferred) {
                continue;
            }

            char err[192] = { 0 };
            cfg = tryVolume(handles[i], err, sizeof(err));
            if (cfg != NULL) {
                break;
            }
            if (err[0] != '\0') {
                /* Found, read, but unusable. Do not look further: the caller
                 * has to fix this rather than get a different file. */
                snprintf(msg, msgLen, "%s", err);
                dealloc(handles);
                return UsConfigBroken;
            }
        }
    }

    dealloc(handles);

    if (cfg == NULL) {
        /* Nothing found: run on defaults. */
        char err[192] = { 0 };
        cfg = usConfigParse("", 0, err, sizeof(err));
        if (cfg == NULL) {
            snprintf(msg, msgLen, "cannot build the default config: %s", err);
            return UsConfigBroken;
        }
        *out = cfg;
        return UsConfigAbsent;
    }

    *out = cfg;
    return UsConfigLoaded;
}
