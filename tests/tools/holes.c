/*
 * holes - report the runs of zero words inside an image's sections.
 *
 * Where the payload goes is a question about space, and space is something an
 * image can answer about itself. A run of zero words inside an executable
 * section is a candidate place to put code: it is mapped executable, so it
 * runs, and it is unreferenced, which is a judgement about the image rather
 * than something readable off it.
 *
 * Runs are reported rather than just the longest one, because the useful
 * question is not "is there a hole" but "is there a hole big enough", and the
 * answer changes with the size of what has to fit.
 *
 * Usage: holes <image> [minBytes]
 */

#include <stdio.h>
#include <stdlib.h>

#include "core/pe.h"
#include "core/scan.h"

/*
 * Runs are summarised by size rather than listed. An image has thousands of
 * short runs and a handful of long ones, so a list is unreadable and the
 * useful question -- is there a run of the size something needs -- is a
 * question about sizes.
 */
#define MAX_BUCKETS 7U
static const uint32_t kBuckets[MAX_BUCKETS] = { 16, 32, 64, 128, 256, 1024, 4096 };

static uint8_t *readWhole(const char *path, size_t *outLen) {
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *data;

    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot size %s\n", path);
        fclose(f);
        return NULL;
    }
    data = malloc((size_t)size);
    if (data == NULL) {
        fprintf(stderr, "out of memory for %s\n", path);
        fclose(f);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "short read on %s\n", path);
        fclose(f);
        free(data);
        return NULL;
    }
    fclose(f);
    *outLen = (size_t)size;
    return data;
}

/*
 * A word inside a section.
 *
 * Beyond the raw data the loader zero fills, so those bytes are zeros here
 * too: that is what the image will look like in memory, and the sections that
 * are only virtual are exactly the ones with room. Reading the file alone
 * would report nothing for them, which is the opposite of the answer wanted.
 */
static bool wordAt(const UsImage *img, const UsPeSection *s, uint32_t rva,
                   uint32_t *out) {
    uint32_t into = rva - s->virtualAddress;
    size_t available = 0;
    const uint8_t *p;

    if (into >= s->rawSize) {
        *out = 0;
        return true;
    }
    p = usImageRvaSpan(img, rva, &available);
    if (p == NULL || available < 4) {
        return false;
    }
    *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
    return true;
}

int main(int argc, char **argv) {
    uint8_t *data;
    size_t len = 0;
    UsImage img;
    uint32_t minBytes = 8;
    unsigned long long totalExec = 0;
    unsigned long long totalWritable = 0;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <image> [minBytes]\n", argv[0]);
        return 2;
    }
    if (argc == 3) {
        minBytes = (uint32_t)strtoul(argv[2], NULL, 0);
    }

    data = readWhole(argv[1], &len);
    if (data == NULL) {
        return 2;
    }
    if (!usImageInitFile(&img, data, len)) {
        fprintf(stderr, "not a usable image: %s\n", argv[1]);
        free(data);
        return 2;
    }

    printf("%s sizeOfImage=0x%x sections=%u\n", argv[1], img.sizeOfImage, img.sectionCount);

    for (uint16_t i = 0; i < img.sectionCount; i++) {
        const UsPeSection *s = &img.sections[i];
        bool exec = (s->characteristics & US_PE_SECTION_EXECUTABLE) != 0;
        bool writable = (s->characteristics & US_PE_SECTION_WRITABLE) != 0;
        bool loaded = s->rawSize != 0;
        uint32_t span = s->virtualSize;
        uint32_t off;
        unsigned long long bucketBytes[MAX_BUCKETS] = { 0 };
        unsigned long long bucketCounts[MAX_BUCKETS] = { 0 };
        uint32_t biggest = 0;
        uint32_t biggestRva = 0;
        unsigned long long slack = 0;

        if (span == 0) {
            continue;
        }

        /*
         * b means the section declares more than the file carries. Whether
         * that excess is mapped is not something the headers say: a section
         * with no raw data at all is only in the table, while one that has
         * some is mapped and the rest of it is zero filled. The distinction
         * matters because code can only go where something is mapped.
         */
        printf("  %-9s va=0x%-8x vsize=0x%-7x raw=0x%-7x %s%s%s%s\n", s->name,
               s->virtualAddress, s->virtualSize, s->rawSize,
               exec ? "X" : "-", writable ? "W" : "-",
               loaded ? "m" : "-",
               (s->virtualSize > s->rawSize) ? "b" : "-");

        for (off = 0; off + 4 <= span;) {
            uint32_t w;
            uint32_t bytes = 0;

            if (!wordAt(&img, s, s->virtualAddress + off, &w) || w != 0) {
                off += 4;
                continue;
            }
            while (off + bytes + 4 <= span && bytes < 0x1000000U) {
                uint32_t n;
                if (!wordAt(&img, s, s->virtualAddress + off + bytes, &n) || n != 0) {
                    break;
                }
                bytes += 4;
            }
            if (bytes >= minBytes) {
                slack += bytes;
                for (uint32_t b = 0; b < MAX_BUCKETS; b++) {
                    if (bytes >= kBuckets[b]) {
                        bucketCounts[b]++;
                        bucketBytes[b] += bytes;
                    }
                }
                if (bytes > biggest) {
                    biggest = bytes;
                    biggestRva = s->virtualAddress + off;
                }
            }
            off += bytes != 0 ? bytes : 4;
        }

        if (biggest != 0) {
            printf("    max %u bytes at +0x%x\n", biggest, biggestRva);
            for (uint32_t b = 0; b < MAX_BUCKETS; b++) {
                if (bucketCounts[b] == 0) {
                    continue;
                }
                printf("    >=%-5u %llu runs, %llu bytes\n", kBuckets[b], bucketCounts[b],
                       bucketBytes[b]);
            }
        } else if (span >= minBytes) {
            printf("    none\n");
        }

        if (exec) {
            totalExec += slack;
        }
        if (writable) {
            totalWritable += slack;
        }
    }

    printf("total >= %u: executable sections %llu bytes, writable sections %llu bytes\n",
           minBytes, totalExec, totalWritable);

    {
        UsVectorTable vt = usLocateVectorTable(&img);

        if (!vt.found) {
            printf("vector table: none\n");
        } else {
            printf("vector table: +0x%x live=%u self=%u candidates=%zu\n", vt.rva,
                   vt.liveSlots, vt.sameSlots, vt.matches);
        }
    }

    {
        UsVbarTables tables = usFindVbarTables(&img);

        printf("vbar tables: %zu sites, %zu unresolved\n", tables.sites, tables.unresolved);
        for (size_t k = 0; k < tables.count; k++) {
            printf("  installed +0x%x\n", tables.rvas[k]);
        }
        if (tables.overflow) {
            printf("  (more than the list holds)\n");
        }
    }

    free(data);
    return 0;
}
