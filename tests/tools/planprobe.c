/*
 * planprobe - print the plan for a pair of images, on the host.
 *
 * The plan is built the same way here as it is in the driver, over a file
 * rather than over a loaded image, and printed through the same emitter. The
 * two dumps are expected to agree: the images differ in where they are and in
 * what surrounds them, but not in their RVAs, and the plan is expressed in
 * RVAs for exactly that reason.
 *
 * That is what makes this a check rather than a convenience. A plan built
 * from a file exercises the same code the driver will run, in seconds,
 * without a boot; and when the driver's dump disagrees, the difference is
 * somewhere in how the image is read, not in what was decided about it.
 *
 * Usage: planprobe <winload.efi> <ntoskrnl.exe>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/plan.h"

static void sinkPuts(void *ctx, const char *s) {
    (void)ctx;
    fputs(s, stdout);
}

static void sinkHex(void *ctx, uint64_t v) {
    (void)ctx;
    printf("0x%llx", (unsigned long long)v);
}

static void sinkDec(void *ctx, uint64_t v) {
    (void)ctx;
    printf("%llu", (unsigned long long)v);
}

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
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *outLen = (size_t)size;
    return data;
}

int main(int argc, char **argv) {
    static const UsSink sink = { NULL, sinkPuts, sinkHex, sinkDec };
    UsImage winload;
    UsImage kernel;
    UsPlan plan;
    uint8_t *winloadData;
    uint8_t *kernelData;
    size_t winloadLen = 0;
    size_t kernelLen = 0;
    bool haveWinload;
    bool haveKernel;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <winload.efi> <ntoskrnl.exe>\n", argv[0]);
        return 2;
    }

    winloadData = readWhole(argv[1], &winloadLen);
    kernelData = readWhole(argv[2], &kernelLen);
    if (winloadData == NULL || kernelData == NULL) {
        return 2;
    }

    haveWinload = usImageInitFile(&winload, winloadData, winloadLen);
    haveKernel = usImageInitFile(&kernel, kernelData, kernelLen);
    if (!haveWinload) {
        fprintf(stderr, "not a usable image: %s\n", argv[1]);
    }
    if (!haveKernel) {
        fprintf(stderr, "not a usable image: %s\n", argv[2]);
    }
    if (!haveWinload && !haveKernel) {
        return 2;
    }

    usPlanBuild(&plan, haveWinload ? &winload : NULL, haveKernel ? &kernel : NULL);
    usPlanEmit(&plan, &sink);

    free(winloadData);
    free(kernelData);
    return plan.complete ? 0 : 1;
}
