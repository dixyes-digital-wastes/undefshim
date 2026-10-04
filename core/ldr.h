/*
 * The loader's module list, see ldr.c
 *
 * The loader knows where every module it mapped ended up, and it says so in a
 * structure that the kernel is started with. Reading it is exact: no scanning,
 * no signature, and no guessing from what the bytes happen to look like
 *
 * The offsets this depends on are the loader's own structures, so they are
 * facts about a version rather than about the problem. Each field is therefore
 * checked before it is used, and a wrong offset has to fail rather than
 * produce a plausible address
 */

#ifndef US_LDR_H
#define US_LDR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What a module entry says about where the module is */
typedef struct UsLDRModule_t {
    uint64_t base;
    uint64_t entry;
    uint64_t size;
    uint32_t nameChars;
} UsLDRModule;

/*
 * Finds a module by name in the loader's list
 *
 * loaderBlock is what the kernel is started with. asciiName is compared
 * against the entry's UTF-16 name, case insensitively, which is how a name
 * spelled in a C string can be matched without a conversion
 *
 * Returns true only when an entry whose name matches also carries a usable
 * base and size. A list whose shape does not fit is walked to its end and
 * answered with false, rather than read past
 */
bool usLDRFindModule(const void *loaderBlock, const char *asciiName,
                     UsLDRModule *out);

#endif
