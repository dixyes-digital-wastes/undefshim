/*
 * Reading the machine's processor list out of ACPI.
 *
 * The payload needs to tell one CPU from another, because everything it keeps
 * per CPU -- a landing pad, a stack -- has to be indexed by something. The
 * obvious something is MPIDR_EL1, and the obvious index is its low byte. That
 * is wrong, and wrong in a way that only shows up with more than one cluster:
 * the low byte is Aff0, which is the core within a cluster, so the first core
 * of every cluster produces the same value. Two CPUs then share one landing
 * pad and one stack, and the corruption is blamed on whatever ran next.
 *
 * ACPI already describes the machine's processors, and the entry for each one
 * carries its MPIDR. Reading that gives a list in which every processor
 * appears once, and a position in that list is an index that means something.
 *
 * Everything here is a pure function over a table in memory, so it can be
 * tested without a machine: the parsing is where the mistakes would be, and
 * a mistake here is a wrong index rather than a visible failure.
 */

#ifndef US_ACPI_H
#define US_ACPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/layout.h"

/* The most processors this will report, which is what the payload has room
 * for. A machine with more is not one this project has anything to say
 * about. */
#define US_ACPI_MAX_CPUS 8

typedef struct UsAcpiCpus_t {
    uint64_t mpidr[US_ACPI_MAX_CPUS];  /* affinity fields only */
    size_t   count;
    bool     overflow;   /* the machine has more processors than this holds */
} UsAcpiCpus;

/*
 * Finds the Multiple APIC Description Table from the root pointer the
 * firmware published. Returns NULL when there is not one, or when any table
 * on the way fails its checksum.
 *
 * The checksum is checked rather than trusted because everything after it is
 * a walk over lengths read out of the table itself: a wrong length is a walk
 * off the end, and this runs before anything is in place to report a fault.
 */
const void *usAcpiFindMadt(const void *rsdp);

/* Collects the processors the MADT describes, in the order it lists them. */
UsAcpiCpus usAcpiCollectCpus(const void *madt);

/*
 * The position of a processor in that list, or -1.
 *
 * This is what the payload indexes everything per CPU by. The value is masked
 * before it is compared, because MPIDR_EL1 and ACPI do not agree about the
 * bits that are not affinity.
 */
int usAcpiCpuIndex(const UsAcpiCpus *cpus, uint64_t mpidr);

#endif
