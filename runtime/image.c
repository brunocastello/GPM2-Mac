/* Loads the PE file into guest memory. */
#include "machine.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t rd16_(const uint8_t *d, uint32_t o) { return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8); }
static uint32_t rd32_(const uint8_t *d, uint32_t o) { return rd16_(d, o) | (rd16_(d, o + 2) << 16); }

int image_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return 1; }
    fclose(f);
    if (n < 0x100 || d[0] != 'M' || d[1] != 'Z') { free(d); return 2; }
    uint32_t pe = rd32_(d, 0x3C), fh = pe + 4;
    uint32_t nsec = rd16_(d, fh + 2), optsz = rd16_(d, fh + 16), opt = fh + 20;
    uint32_t base = rd32_(d, opt + 28), hdrs = rd32_(d, opt + 60);
    if (base + hdrs > MEM_SIZE) { free(d); return 3; }
    memcpy(M + base, d, hdrs);
    uint32_t so = opt + optsz;
    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t h = so + 40u * i;
        uint32_t vsize = rd32_(d, h + 8), rva = rd32_(d, h + 12);
        uint32_t rawsize = rd32_(d, h + 16), rawptr = rd32_(d, h + 20);
        uint32_t va = base + rva;
        if (rawsize > (uint32_t)n - rawptr) rawsize = (uint32_t)n - rawptr;
        if ((uint64_t)va + (rawsize > vsize ? rawsize : vsize) > MEM_SIZE) { free(d); return 3; }
        memset(M + va, 0, vsize);
        memcpy(M + va, d + rawptr, rawsize);
    }
    free(d);
    for (unsigned i = 0; i < import_count; i++)
        { uint32_t v = LE32_(IMPORT_MAGIC_BASE + 4u * i); memcpy(M + import_table[i].iat, &v, 4); }
    return 0;
}
