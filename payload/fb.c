/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (C) 2026 Yun Dou <dixyes@gmail.com>
 *
 * Drawing on the firmware's frame buffer, see fb.h
 *
 * What is drawn here is not a console. The screen the driver keeps up to date
 * stops being writable when the kernel's tables replace the firmware's, and
 * what is left is memory that the firmware goes on displaying because this
 * machine has no graphics driver and the desktop is redrawn into it. So the
 * payload writes into that memory directly, at an address it mapped itself,
 * and the result is what a camera pointed at the machine sees
 *
 * Two rules run through all of it. Nothing may fault: the moment this is used
 * is the moment after something already went wrong, and a fault inside it is a
 * stop with nothing to say. And nothing may be assumed about the mapping: the
 * kernel may take the address back at any time, so every use begins by asking
 * the tables whether it is still there
 */

#include "common/font.h"
#include "common/layout.h"
#include "core/mair.h"
#include "core/pgtable.h"
#include "core/pixel.h"
#include "core/pool.h"
#include "core/translate.h"
#include "payload/fb.h"
#include "payload/payload.h"

/* A descriptor read that was refused, as a shape no level may hold */
#define US_FB_UNREADABLE 2U

/*
 * The most two megabyte entries one frame buffer may take
 *
 * It is the room the addresses of those entries are kept in while a run of
 * them is being written, so it is a bound on the array and not a limit that
 * means anything to a screen: 32 of them is 64 megabytes of frame buffer,
 * and a machine with more than that to draw on is not one of these
 */
#define US_FB_MAX_BLOCKS 32U

static uint32_t gFBRow;
static uint32_t gFBColumn;

/*
 * A descriptor, read the way this path reads anything that may not be there
 *
 * A read that would fault answers as a shape no level may hold rather than
 * becoming a fault inside a handler: two reserved low bits are not a table, a
 * block or the mapped page, so the walk stops at this entry and nothing is
 * written over it. Treating an unreadable entry as taken is the whole point --
 * an entry whose contents are unknown is not a free one
 */
static uint64_t readDescriptor(void *ctx, uint64_t va) {
    uint64_t value = 0;

    (void)ctx;
    if (!usPayloadProbeRead(va, &value)) {
        return US_FB_UNREADABLE;
    }
    return value;
}

/*
 * A descriptor, written only if it read back as what was stored
 *
 * A store the mapping refuses is not the only way this can fail: a store that
 * lands somewhere else leaves a table that says something the walk did not
 * decide, and the caller has no way to tell afterwards. So the answer comes
 * from reading it again rather than from the store returning
 */
static bool writeDescriptor(uint64_t va, uint64_t value) {
    uint64_t readback = 0;

    if (!usPayloadProbeWrite(va, value)) {
        return false;
    }
    return usPayloadProbeRead(va, &readback) && readback == value;
}

/*
 * Our own information, or nothing when there is none to be had
 *
 * The pool is reached through the address the address change produced, so a
 * payload that never got one has nothing to write into and nothing was built
 */
static UsPool *poolOf(UsPayloadConfig *cfg) {
    UsPool *pool;

    if (cfg->poolBase == 0 || cfg->fbPA == 0 || cfg->fbBytes == 0) {
        return NULL;
    }
    pool = (UsPool *)(uintptr_t)cfg->poolBase;
    /*
     * The record has to be one the boot made, because this writes into it:
     * a pool whose record was never filled in is a machine whose firmware
     * described no screen, and writing a mapping and its reason into a
     * zeroed record would make a reader believe there was one to fail
     */
    return pool->fb.magic == US_POOL_FB_MAGIC ? pool : NULL;
}

/*
 * Where the payload's own mapping of the frame buffer goes
 *
 * Not an address the kernel chose, and not one that can be worked out from
 * anywhere: this kernel has no region where physical memory appears at a
 * constant offset -- its tables are page maps all the way down, so a physical
 * address says nothing about any virtual one
 *
 * So the address is invented, and two things make that safe. Every entry on
 * the way to it has to be empty before anything is written, which is checked
 * rather than reasoned about. And the search starts from an address that is
 * known to be mapped -- the pool's own -- because a proposal in a part of the
 * address space the kernel never filled in has no tables to write into at all,
 * which is a refusal and not a mapping
 *
 * The block is written at the level that covers two megabytes, not one that
 * covers a gigabyte. Both would hold the screen, and the gigabyte would be one
 * entry as well, but it would map a gigabyte of physical memory of which all
 * but the first two megabytes is something else -- other memory, aliased at an
 * address nothing asked for
 */

/* Marks a run of memory as changed, which is what a new mapping needs */
static void invalidate(uint64_t va) {
    uint64_t arg = va >> 12;

    __asm__ volatile("dsb ishst" ::: "memory");
    __asm__ volatile("tlbi vaae1, %0" ::"r"(arg) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

/*
 * Whether an address translates to what it is supposed to
 *
 * The comparison is made at the size of the mapping, and that is not a detail:
 * a translation answers about the mapping that covers the address, so for a
 * two megabyte block it gives the block's own base and not the frame buffer's
 * offset within it. Comparing at four kilobytes would find the two different
 * whenever the frame buffer is not itself block aligned, which it is not --
 * and would report a mapping that is there as missing
 *
 * The question is asked of the tables rather than of a flag written earlier,
 * because between the two the kernel may have replaced the entry with one of
 * its own. Only the physical address is compared, so a kernel that mapped the
 * same memory itself is an answer that still holds
 */
static bool mapsTo(uint64_t va, uint64_t pa, uint64_t size) {
    uint64_t got = 0;

    return usTranslateAddress(va, true, &got)
           && (got & ~(size - 1U)) == (pa & ~(size - 1U));
}

/*
 * Where the kernel's descriptor mapping is right now
 *
 * Read afresh every time rather than kept, because the value it holds at the
 * handover is not the one it ends up with: the kernel sets that variable when
 * it builds its own self map, which is after the payload has already run once,
 * and an address computed from the earlier value names a table the kernel does
 * not use. Reading it at the moment it is needed is the only way to be sure
 * which of the two is in hand
 */
static bool descriptorBase(uint64_t *out) {
    UsPayloadConfig *cfg = usPayloadConfig();
    uint64_t base = 0;

    if (cfg->kernelBase == 0 || cfg->descriptorBaseRVA == 0) {
        return false;
    }
    if (!usPayloadProbeRead(cfg->kernelBase + cfg->descriptorBaseRVA, &base)
        || base == 0 || (base & 0xFFFU) != 0) {
        return false;
    }
    *out = base;
    return true;
}

/*
 * Puts the frame buffer at an address of the payload's own choosing
 *
 * The entries are written at the level that covers two megabytes, so that one
 * store covers two megabytes of the screen: a page at a time would be several
 * hundred writes into a table, on a path that has nothing left to recover
 * with, and every one of them another chance to be the thing that faults
 *
 * More than one entry, and that is the part that had to be got right. The
 * frame buffer is not aligned to the start of a block, so what it needs is the
 * rest of the block it starts in and then however many whole ones its length
 * takes: a machine whose frame buffer is a little under two megabytes and
 * starts a little into its block needs two, the first covering the screen up
 * to the end of that block and the second the rows past it. One entry and no
 * more was a screen whose last rows were not there at all -- and nothing had
 * run into them, because everything drawn on it until then was near the top
 *
 * The search is inside the gigabyte the pool is already in, and that is the
 * whole of it. The pool's own descriptors are there, so there is a table to
 * write into; the kernel maps what it needs and leaves the rest of that
 * table alone, which is what makes a free entry in it the ordinary case
 * rather than the lucky one. A gigabyte block would also fit and would be one
 * entry, but it would map a gigabyte of physical memory of which all but the
 * first part is something else -- an alias of memory nothing asked to see at
 * that address
 *
 * The entries have to be a run: every one of them is free before any of them
 * is written, and one that does not take effect takes the whole mapping back
 * with it. A surface that is half there and says it is whole is worse than
 * one that was never made, because nothing downstream can tell
 *
 * Nothing is assumed about which entries are free: each candidate's entry is
 * read, and only an entry that reads as zero is written. A read that is
 * refused is treated as taken rather than as free, because an entry that
 * cannot be read is one whose contents are unknown, and writing over an
 * unknown entry in the kernel's own tables is the one mistake here that
 * cannot be undone
 */
static bool mapFB(UsPool *pool, uint64_t *vaOut) {
    UsPayloadConfig *cfg = usPayloadConfig();
    const uint64_t slots = US_GRANULE_1G / US_GRANULE_2M;
    const uint64_t gigabyte = cfg->poolBase & ~(US_GRANULE_1G - 1U);
    const uint64_t mine = (cfg->poolBase & (US_GRANULE_1G - 1U)) / US_GRANULE_2M;
    const uint64_t offset = cfg->fbPA & (US_GRANULE_2M - 1U);
    const uint64_t paBase = cfg->fbPA & ~(US_GRANULE_2M - 1U);
    /* How many of them the frame buffer takes from the block it starts in */
    const uint64_t blocks = usFBBlocksFor(cfg->fbPA, cfg->fbBytes);
    uint64_t where[US_FB_MAX_BLOCKS];
    uint64_t base;
    uint64_t mair;
    int attr;

    /*
     * A frame buffer longer than a gigabyte of entries is a machine this was
     * not written for, and the array below is what says so rather than a loop
     * that would run off its end
     */
    if (blocks == 0 || blocks > US_FB_MAX_BLOCKS || blocks > slots) {
        usPoolFBRefuse(pool, US_FB_NO_SLOT, blocks);
        return false;
    }

    if (!descriptorBase(&base)) {
        usPoolFBRefuse(pool, US_FB_NO_BASE, 0);
        return false;
    }
    pool->fb.mpteBase = base;

    /*
     * The attribute, looked up rather than assumed: which of the eight entries
     * holds the non-cacheable byte is the kernel's choice, and a wrong index
     * is a mapping that draws nothing while looking correct in every field.
     * Only a non-cacheable one is accepted: writes to a cached mapping of a
     * frame buffer are visible to the CPU and to nothing else, which is a
     * screen that does not change while the machine is fine
     */
    __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
    attr = usMAIRNormalNonCacheableIndex(mair);
    if (attr < 0) {
        usPoolFBRefuse(pool, US_FB_NO_ATTR, mair);
        return false;
    }

    for (uint64_t n = 0; n < slots; n++) {
        /*
         * The pool's own slot first and then outwards in both directions, so
         * the entries taken are the ones nearest to where the pool already is
         */
        uint64_t slot = n == 0 ? mine
                               : ((n & 1U) != 0
                                      ? (mine + slots - (n + 1U) / 2U) % slots
                                      : (mine + n / 2U) % slots);
        uint64_t va;
        bool free = true;

        /*
         * A run that stays inside the gigabyte, because the entries of one
         * gigabyte are the entries of one table: a run that wrapped would be
         * two runs, and the screen between them would be wherever the wrap
         * landed
         */
        if (slot + blocks > slots) {
            continue;
        }
        for (uint64_t i = 0; i < blocks; i++) {
            UsFreeSlot entry = usFreeSlotFind(
                base, readDescriptor, NULL,
                gigabyte + (slot + i) * US_GRANULE_2M);

            /*
             * Only an empty entry at the two megabyte level. Anything else
             * means either that the address is already used, or that the walk
             * could not get to the level that would hold the block
             */
            if (!entry.found || entry.level + 2U != US_SELF_MAP_LEVELS) {
                free = false;
                break;
            }
            where[i] = entry.descriptorVA;
        }
        if (!free) {
            continue;
        }
        /*
         * The mapping goes at the frame buffer's own place inside the first
         * block and not at the block's start, and the rest follow it: the
         * frame buffer is one run of memory and the screen is drawn through
         * it as one, so what is before its start is not part of it. Writing
         * before it is not a fault and nothing complains -- the rows go
         * somewhere that is not displayed, and the report is lost in a way
         * that looks exactly like the drawing never happened
         */
        for (uint64_t i = 0; i < blocks; i++) {
            (void)writeDescriptor(
                where[i], usBlockDescriptor(paBase + i * US_GRANULE_2M,
                                            US_GRANULE_2M, (uint64_t)attr));
        }
        va = gigabyte + slot * US_GRANULE_2M + offset;
        for (uint64_t i = 0; i < blocks; i++) {
            invalidate(gigabyte + (slot + i) * US_GRANULE_2M);
        }
        /*
         * Both ends of it, because the first entry taking effect says nothing
         * about the last: it is the far end of a screen that a mapping one
         * entry short leaves out, and asking about it here is what turns that
         * into a refusal rather than into a fault in the middle of a report
         */
        if (!mapsTo(va, cfg->fbPA, US_GRANULE_2M)
            || !mapsTo(va - offset + (blocks - 1U) * US_GRANULE_2M,
                       paBase + (blocks - 1U) * US_GRANULE_2M,
                       US_GRANULE_2M)) {
            /*
             * The stores did not take effect. Put nothing behind: the walk
             * found these entries empty, so empty ones are what belong there
             */
            for (uint64_t i = 0; i < blocks; i++) {
                (void)writeDescriptor(where[i], 0);
                invalidate(gigabyte + (slot + i) * US_GRANULE_2M);
            }
            continue;
        }
        *vaOut = va;
        usPoolFBSet(pool, va, blocks, (uint64_t)attr);
        return true;
    }

    usPoolFBRefuse(pool, US_FB_NO_SLOT, mine);
    return false;
}

bool usFBBuild(uint64_t kernelBase) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsPool *pool = poolOf(cfg);

    if (pool == NULL) {
        return false;
    }
    if (kernelBase == 0) {
        usPoolFBRefuse(pool, US_FB_NO_KERNEL, 0);
        return false;
    }
    if (cfg->descriptorBaseRVA == 0) {
        usPoolFBRefuse(pool, US_FB_NO_BASE_RVA, 0);
        return false;
    }
    /*
     * Where the kernel's image is, recorded and nothing more
     *
     * The mapping is not built here, and that is the correction this went
     * through: at this moment the kernel has not built its own descriptor
     * mapping, so the base its variable holds is not the one it will use, and
     * an entry written through it would be in a table nothing reads. The image
     * base, on the other hand, is a fact from here on, so it is kept and the
     * mapping is deferred to the moment it is wanted
     */
    cfg->kernelBase = kernelBase;
    pool->fb.kernelBase = kernelBase;
    return true;
}

/* Whether the mapping exists, building it on the first question asked */
static bool ensureMapped(UsPool *pool) {
    uint64_t va = 0;

    if (pool->fb.aliasVA != 0) {
        return true;
    }
    if (!mapFB(pool, &va)) {
        return false;
    }
    return true;
}

bool usFBAvailable(void) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsPool *pool = poolOf(cfg);

    if (pool == NULL || pool->fb.pa == 0) {
        return false;
    }
    if (!ensureMapped(pool)) {
        return false;
    }
    /*
     * Asked of the tables every time rather than remembered, which is the
     * whole reason drawing can be attempted at all: the kernel is free to take
     * the address back, and a remembered "yes" would make the next draw a
     * fault on a path that has nothing left to recover with
     *
     * A mapping that is gone is not put back here. It is forgotten, so that
     * the next question builds one somewhere else -- the address the kernel
     * took is not one to argue about
     */
    if (!mapsTo(pool->fb.aliasVA, pool->fb.pa, US_GRANULE_2M)) {
        pool->fb.aliasVA = 0;
        return false;
    }
    /*
     * The far end of the screen, and not just its first entry: the kernel may
     * have taken back any part of the mapping, and a screen that is drawn on
     * while its last rows are not there is a fault in the middle of a report --
     * which is the one thing this path may not do. Asking about the last row is
     * asking about the whole of it, because a mapping is a run and what is
     * between two of its addresses is mapped too
     */
    if (!mapsTo(pool->fb.aliasVA
                    + (uint64_t)(pool->fb.height - 1U) * pool->fb.stride * 4U,
                pool->fb.pa
                    + (uint64_t)(pool->fb.height - 1U) * pool->fb.stride * 4U,
                US_GRANULE_2M)) {
        pool->fb.aliasVA = 0;
        return false;
    }
    return true;
}

/* The bytes of one pixel of a row, or NULL when the row is not drawable */
static volatile uint32_t *rowAt(const UsPoolFB *fb, uint32_t row) {
    if (row >= fb->height || fb->stride < fb->width) {
        return NULL;
    }
    return (volatile uint32_t *)(uintptr_t)(fb->aliasVA
                                            + (uint64_t)row * fb->stride * 4U);
}

static void putGlyph(const UsPoolFB *fb, uint32_t x, const uint8_t *glyph,
                     uint32_t ink, uint32_t paper) {
    for (uint32_t r = 0; r < US_FONT_HEIGHT; r++) {
        volatile uint32_t *row = rowAt(fb, gFBRow + r);

        if (row == NULL) {
            return;
        }
        for (uint32_t c = 0; c < US_FONT_WIDTH; c++) {
            if (x + c >= fb->width) {
                break;
            }
            row[x + c] = (glyph[r] & (0x80U >> c)) != 0 ? ink : paper;
        }
    }
}

void usFBText(const char *text) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsPool *pool = poolOf(cfg);
    uint32_t ink;
    uint32_t paper;

    if (pool == NULL || text == NULL || !usFBAvailable()) {
        return;
    }
    pool->fb.drawn++;
    /*
     * Black on white, which is what the boot's own screen is: a photograph of
     * a machine that has stopped is read by a person, and the two are the
     * colours that read at the widest range of exposures
     */
    ink = usPixelColour(0, 0, 0, pool->fb.blueFirst != 0);
    paper = usPixelColour(0xFF, 0xFF, 0xFF, pool->fb.blueFirst != 0);

    /*
     * Where the pen is is state and not a local
     *
     * A report is written a fragment at a time -- a label, then a number, then
     * the next label -- and each call has to carry on where the one before it
     * stopped. A column that started at zero every time would draw every
     * fragment on top of the last, which is a report that reads as the few
     * characters they all had in common
     */
    for (const char *p = text; *p != '\0'; p++) {
        const uint8_t *glyph;
        char c = *p;

        if (c == '\n'
            || gFBColumn * US_FONT_WIDTH + US_FONT_WIDTH > pool->fb.width) {
            /*
             * A newline, and a line that has reached the edge. Both end the
             * line, and a character that would run off is what ends it rather
             * than being drawn in the margin
             */
            if (c != '\n') {
                p--;
            }
            gFBColumn = 0;
            gFBRow += US_FONT_HEIGHT;
            continue;
        }
        if (c < US_FONT_FIRST || c > US_FONT_LAST) {
            c = '?';
        }
        glyph = kFont[(int)c - US_FONT_FIRST];
        putGlyph(&pool->fb, gFBColumn * US_FONT_WIDTH, glyph, ink, paper);
        gFBColumn++;
    }
}

void usFBHex(uint64_t value) {
    char out[19];
    size_t at = 0;
    bool started = false;

    out[at++] = '0';
    out[at++] = 'x';
    for (int nibble = 15; nibble >= 0; nibble--) {
        unsigned digit = (unsigned)((value >> (nibble * 4)) & 0xFU);

        if (digit != 0 || started || nibble == 0) {
            started = true;
            out[at++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
        }
    }
    out[at] = '\0';
    usFBText(out);
}

void usFBDec(uint64_t value) {
    char out[21];
    size_t at = sizeof(out) - 1U;

    out[at] = '\0';
    do {
        out[--at] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0);
    usFBText(&out[at]);
}
