/*
 * Replacing an RCpc load where it stands, see rewrite.c
 *
 * The question the caller has is only whether it happened: a site that was not
 * replaced is one that keeps trapping, which is what the handler already knows
 * how to answer
 */

#ifndef US_REWRITE_H
#define US_REWRITE_H

#include <stdbool.h>
#include <stdint.h>

/* What came of one attempt, recorded for the host to read. The first three are
 * answers; the rest are the ways it can fail, kept apart because which one
 * happened is the difference between "this machine cannot" and "this address
 * cannot". */
typedef enum UsRewriteResult_e {
    UsRewriteWritten = 1,  /* the site now holds the acquire load */
    UsRewriteAlready = 2,  /* it did already; this processor's copy was stale */
    UsRewriteRefused = 3,  /* the store was refused, so nothing changed */
    UsRewriteNoBase = 4,   /* the descriptor base is not known */
    UsRewriteUnmapped = 5, /* no descriptor translates the site */
    UsRewriteReadOnly = 6, /* the descriptor's own page would not take a store */
    UsRewriteStuck = 7,    /* the permission could not be put back */
    UsRewriteNotRcpc = 8,  /* what is there is neither an RCpc load nor its substitute */
    UsRewriteMisaligned = 9, /* an acquire load cannot be reached at that address */

    /*
     * How far an attempt got, written as it goes rather than only at the end
     *
     * An attempt that never finishes is the case this is for: the record it
     * left says which step it was on, and a machine that stopped there says
     * more than one that stopped with no record at all
     */
    UsRewriteReadingBase = 10,
    UsRewriteWalking = 11,
    UsRewriteProbing = 12,
    UsRewriteClearing = 13,
    UsRewriteStoring = 14,
    UsRewritePublishing = 15,
    UsRewriteRestoring = 16,
} UsRewriteResult;

/*
 * Replaces the instruction at site if it is an RCpc load and its page can be
 * made writable for the one store that takes
 *
 * site has to be four-byte aligned; it is the address of the instruction that
 * trapped. Nothing is changed unless the whole sequence can be carried out,
 * and a store the mapping refuses is answered rather than raised, so this can
 * be called from the handler without a failure here becoming a fault in one
 *
 * The address the load read is part of the question, because the substitute is
 * an acquire load and the instruction it replaces is not: a site reached at an
 * alignment the substitute would fault on is left as it is, and the handler
 * keeps carrying that one out. Rewriting it anyway would make a load that only
 * ever worked into one that faults from then on
 */
UsRewriteResult usRewriteSite(uint64_t site, UsLDAPRKind kind, uint64_t address);

#endif
