/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Finding the frame buffer, see gop.h for what is answered and screen.h for
 * what is drawn on it
 *
 * Everything the firmware says about the screen is read here and turned into
 * the four numbers a drawer needs, so that the drawer works on a block of
 * pixels and never on a protocol
 */

#include <uefi.h>

#include "uefi/gop.h"

/*
 * The graphics output protocol, which the firmware headers here do not
 * declare: they carry the pixel formats and the GUID and say the rest is not
 * used. Only the mode is needed, and only the fields of it that say where the
 * frame buffer is and how wide it is
 */
typedef struct {
    uint32_t               Version;
    uint32_t               HorizontalResolution;
    uint32_t               VerticalResolution;
    efi_gop_pixel_format_t PixelFormat;
    efi_gop_pixel_bitmask_t PixelInformation;
    uint32_t               PixelsPerScanLine;
} UsGOPModeInfo;

typedef struct {
    uint32_t       MaxMode;
    uint32_t       Mode;
    UsGOPModeInfo *Info;
    uint64_t       SizeOfInfo;
    uint64_t       FrameBufferBase;
    uint64_t       FrameBufferSize;
} UsGOPMode;

typedef struct {
    void *QueryMode;
    void *SetMode;
    void *Blt;
    UsGOPMode *Mode;
} UsGOP;

bool usGOPFind(UsGOPFrame *out) {
    efi_guid_t guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    UsGOP *gop = NULL;
    UsGOPModeInfo *info;
    efi_gop_pixel_format_t format;

    if (out == NULL) {
        return false;
    }
    if (EFI_ERROR(BS->LocateProtocol(&guid, NULL, (void **)&gop)) || gop == NULL
        || gop->Mode == NULL || gop->Mode->Info == NULL) {
        return false;
    }
    info = gop->Mode->Info;
    /* Only the two formats whose channels are eight bits wide are drawn on: a
     * bit mask or a blt-only mode would need a translation this does not
     * have, and guessing at one would put unreadable pixels on the screen */
    format = info->PixelFormat;
    if (format != PixelRedGreenBlueReserved8BitPerColor
        && format != PixelBlueGreenRedReserved8BitPerColor) {
        return false;
    }
    if (gop->Mode->FrameBufferBase == 0 || info->PixelsPerScanLine == 0
        || info->HorizontalResolution == 0 || info->VerticalResolution == 0) {
        return false;
    }
    *out = (UsGOPFrame){
        .pixels = (uint8_t *)(uintptr_t)gop->Mode->FrameBufferBase,
        .width = info->HorizontalResolution,
        .height = info->VerticalResolution,
        .stride = info->PixelsPerScanLine,
        .blueFirst = format == PixelBlueGreenRedReserved8BitPerColor,
    };
    return true;
}
