/*
 * What runs at the handover
 *
 * The stub in the loader's spare slot calls this, so this is the first code of
 * ours that runs after the kernel's address space is in force. It has two
 * things it can do that nothing later can:
 *
 *   - read the tables, because physical memory is still reachable at its own
 *     address and a table entry names a physical address
 *   - be reached from the loader at all, before anything of the kernel runs
 *
 * The result is the pool's address in the kernel's own space, which is what
 * everything afterwards needs and nothing can compute
 */

#ifndef US_TRANSFER_H
#define US_TRANSFER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Called from the handover stub with the kernel's entry point and the loader's
 * argument
 *
 * Returns nothing: the stub continues into the kernel whatever happens here.
 * A failure is reported and recorded rather than returned, because there is
 * no caller to answer to
 */
void usTransferEntry(uint64_t kernelEntryVA, uint64_t loaderBlockVA);

#endif
