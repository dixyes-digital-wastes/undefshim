/*
 * The handover, see transfer.h.
 *
 * Nothing here writes to the serial port, and that is not an oversight.
 *
 * The kernel's page tables are in force by the time this runs, and they do
 * not map the UART: it was reachable a moment earlier through the firmware's
 * tables, and it is not reachable now. Writing to it stops the machine, which
 * is a hard lesson to learn from a silent hang and is why it is written down
 * here instead.
 *
 * What can be reached is the pool, at its boot address, and the loader and
 * kernel images. So the result goes into the pool, where anything that runs
 * later can read it, and reporting it is left to whatever has a way to report.
 */

#include "common/layout.h"
#include "payload/payload.h"
#include "payload/selfmap.h"
#include "payload/transfer.h"

/* Where the pool was reached from before the switch, and where it will be
 * reached from after. The second is what the walk is for. */
typedef struct UsTransferRecord_t {
    uint64_t magic;
    uint64_t poolPa;
    uint64_t poolVaBefore;
    uint64_t poolVaAfter;
    uint64_t kernelEntry;
    uint64_t mappedSize;
    uint64_t probes;
    uint32_t mapped;
    uint32_t exhausted;
    uint32_t faulted;
} UsTransferRecord;

#define US_TRANSFER_MAGIC 0x5241544e55534555ULL /* "UUSENTAR", readable in a dump */

/*
 * Kept in the payload's own data so a later stage, or a dump of the running
 * machine, can read what was found without searching for it again. Written
 * once, by the CPU the loader hands over on: the others are started by the
 * kernel afterwards, so there is nothing to guard against here.
 */
UsTransferRecord usTransferRecord;

void usTransferEntry(uint64_t kernelEntryVa) {
    UsPayloadConfig *cfg = usPayloadConfig();
    UsSelfMap self;
    uint64_t here;

    __asm__ volatile("adr %0, usTransferEntry" : "=r"(here));

    usTransferRecord.magic = US_TRANSFER_MAGIC;
    usTransferRecord.kernelEntry = kernelEntryVa;
    usTransferRecord.poolPa = cfg->selfVa;
    usTransferRecord.poolVaBefore = here;

    /*
     * The question this whole arrangement exists for: the pool's address once
     * the kernel's tables are what translates it. The kernel's entry point is
     * what the search is anchored on, since it is the one address in the new
     * space that is known here for certain.
     */
    self = usSelfMapFind(cfg->selfVa, US_POOL_BYTES, kernelEntryVa);

    usTransferRecord.mapped = self.found ? 1U : 0U;
    usTransferRecord.poolVaAfter = self.va;
    usTransferRecord.mappedSize = self.size;
    usTransferRecord.probes = self.probes;
    usTransferRecord.exhausted = self.exhausted ? 1U : 0U;
    usTransferRecord.faulted = self.faulted ? 1U : 0U;
}
