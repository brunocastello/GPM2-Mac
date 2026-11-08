/* Reads the .rsrc of the exe already loaded in guest memory. */
#include "win32.h"

#include <stdio.h>

static uint32_t rsrc_va, rsrc_size;

#define IMG_BASE 0x00400000u

int res_load(void) {
    uint32_t pe = R32(IMG_BASE + 0x3Cu) + IMG_BASE;
    uint32_t opt = pe + 24u;
    uint32_t rva = R32(opt + 96u + 2u * 8u), sz = R32(opt + 96u + 2u * 8u + 4u);   /* directory 2 = resources */
    if (!rva) return 1;
    rsrc_va = IMG_BASE + rva; rsrc_size = sz;
    return 0;
}

/* looks up `id` in a resource directory (offset relative to .rsrc); returns the offset of the
 * entry (bit 31 = subdirectory) or 0xFFFFFFFF. id = 0xFFFFFFFF matches the first entry. */
static uint32_t dir_find(uint32_t dir, uint32_t id) {
    uint32_t nn = R16(rsrc_va + dir + 12u), ni = R16(rsrc_va + dir + 14u), e = rsrc_va + dir + 16u;
    uint32_t i;
    e += nn * 8u;                                   /* named entries come first; ignored */
    for (i = 0; i < ni; i++, e += 8u)
        if (id == 0xFFFFFFFFu || R32(e) == id) return R32(e + 4u);
    return 0xFFFFFFFFu;
}

/* named entry `name` (case-insensitive) of a directory; same return format as dir_find */
static uint32_t dir_find_name(uint32_t dir, const char *name) {
    uint32_t nn = R16(rsrc_va + dir + 12u), e = rsrc_va + dir + 16u;
    for (uint32_t i = 0; i < nn; i++, e += 8u) {
        uint32_t np = rsrc_va + (R32(e) & 0x7FFFFFFFu), len = R16(np), k;
        char buf[64];
        for (k = 0; k < len && k + 1 < sizeof buf; k++) buf[k] = (char)R16(np + 2u + 2u * k);
        buf[k] = 0;
        if (gstr_cmp_ci(buf, name) == 0) return R32(e + 4u);
    }
    return 0xFFFFFFFFu;
}

/* resource by name */
uint32_t res_find_name(uint32_t type, const char *name, uint32_t *size) {
    uint32_t t, i, l, d;
    if (!rsrc_va) return 0;
    t = dir_find(0, type);
    if (t == 0xFFFFFFFFu || !(t & 0x80000000u)) return 0;
    i = dir_find_name(t & 0x7FFFFFFFu, name);
    if (i == 0xFFFFFFFFu || !(i & 0x80000000u)) return 0;
    l = dir_find(i & 0x7FFFFFFFu, 0xFFFFFFFFu);
    if (l == 0xFFFFFFFFu || (l & 0x80000000u)) return 0;
    d = rsrc_va + l;
    if (size) *size = R32(d + 4u);
    return IMG_BASE + R32(d);
}

uint32_t res_find(uint32_t type, uint32_t id, uint32_t *size) {
    uint32_t t, i, l, d;
    if (!rsrc_va) return 0;
    t = dir_find(0, type);
    if (t == 0xFFFFFFFFu || !(t & 0x80000000u)) return 0;
    i = dir_find(t & 0x7FFFFFFFu, id);
    if (i == 0xFFFFFFFFu || !(i & 0x80000000u)) return 0;
    l = dir_find(i & 0x7FFFFFFFu, 0xFFFFFFFFu);      /* first language */
    if (l == 0xFFFFFFFFu || (l & 0x80000000u)) return 0;
    d = rsrc_va + l;
    if (size) *size = R32(d + 4u);
    return IMG_BASE + R32(d);
}

int res_string(uint32_t id, char *out, size_t cap) {
    uint32_t sz, a = res_find(6, (id >> 4) + 1u, &sz), i, len;
    if (!a) return -1;
    for (i = 0; i < (id & 15u); i++) a += 2u + 2u * R16(a);
    len = R16(a);
    if (len == 0) return -1;                         /* empty entry = does not exist */
    a += 2u;
    for (i = 0; i < len && i + 1 < cap; i++) out[i] = (char)R16(a + 2u * i);
    out[i < cap ? i : cap - 1] = 0;
    return (int)i;
}
