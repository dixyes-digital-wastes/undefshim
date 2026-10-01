/*
 * The payload's memory, as one contiguous position independent blob.
 *
 * Two things here are load bearing.
 *
 * Everything the payload owns lies between usPayloadStart and usPayloadEnd,
 * because the boot copies exactly that range. A section the linker placed
 * outside it would be silently left behind, and the payload would then read
 * zeros where its data should be.
 *
 * There is no .bss. A blob copied as bytes has no loader to zero anything, so
 * zero initialised data has to be carried as data. The build passes
 * -fno-zero-initialized-in-bss to make sure of that, and checks the binary is
 * as large as the range claims, so a stray .bss cannot pass unnoticed.
 */

ENTRY(usSyncEntry)

SECTIONS
{
    . = 0;

    .text : ALIGN(16) {
        usPayloadStart = .;
        *(.text.entry)
        *(.text*)
    }

    .rodata : ALIGN(16) {
        *(.rodata*)
    }

    .data : ALIGN(16) {
        *(.data*)
    }

    /*
     * The end is the end of the last section that carries contents, and it is
     * deliberately not rounded up: this symbol is compared against the size of
     * the binary, and padding the address would only make the two disagree.
     */
    usPayloadEnd = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.ARM.exidx*)
    }
}
