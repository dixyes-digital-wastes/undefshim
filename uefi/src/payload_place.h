/*
 * Getting the payload into memory, and putting it where it can be entered
 *
 * The blob is built as position independent code and linked at zero, so where
 * it comes to rest is the boot's decision. It has to be a place the kernel
 * will still map, and executable, which rules out the memory the runtime
 * services data pool is in: the OS maps that non-executable, correctly so,
 * and asking for the code class instead is the whole of the difference
 *
 * Nothing here is one shot in the sense the patch table is. The payload is
 * copied once, and the pointers inside it are written once; what is not yet
 * settled is what the address will be after the kernel rebuilds the address
 * space, which is a separate problem and a separate step
 */

#ifndef US_UEFI_PAYLOAD_PLACE_H
#define US_UEFI_PAYLOAD_PLACE_H

#include <stdbool.h>
#include <stdint.h>

/* Named, not included: the session holds a placement, so including it here
 * would be a cycle */
typedef struct UsSession_t UsSession;

typedef struct UsPayloadPlace_t {
    /* Where the blob is, and how big it is */
    uint64_t baseVA;
    uint64_t basePA;
    uint64_t bytes;

    /* Into the blob: the assembly entry, and the configuration block */
    uint64_t entryVA;
    uint64_t configVA;
} UsPayloadPlace;

/*
 * Allocates executable memory, copies the blob into it, and fills in the
 * configuration block. Idempotent: a second call leaves the first placement
 * alone, because the addresses in it may already have been handed out
 *
 * Returns false when the firmware refuses the memory, which is fatal: without
 * a payload there is nothing to enter
 */
bool usPayloadPlace(UsSession *session, UsPayloadPlace *out);

/*
 * Prints what the placement worked out, and calls into the payload to prove
 * the copy is executable and its configuration is reachable
 */
void usPayloadReport(const UsSession *session);

#endif
