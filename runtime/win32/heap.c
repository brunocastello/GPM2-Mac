/* Deterministic guest heap: first-fit with an 8-byte header
 * (size | bit 0 = in use), contiguous blocks from HEAP_BASE to the end of memory. */
#include "win32.h"

#define HDR 8u

static uint32_t heap_end, first_free;

void heap_init(void) {
    heap_end = MEM_SIZE;
    W32(HEAP_BASE, (heap_end - HEAP_BASE) & ~1u);       /* one giant free block */
    W32(HEAP_BASE + 4u, 0);
    first_free = HEAP_BASE;
}

static uint32_t bsize(uint32_t b) { return R32(b) & ~1u; }
static int bused(uint32_t b) { return (int)(R32(b) & 1u); }

uint32_t heap_alloc(uint32_t n) {
    uint32_t need = (n + 7u) & ~7u, b;
    if (n > MEM_SIZE) return 0;
    if (need == 0) need = 8u;
    need += HDR;
    for (b = first_free; b < heap_end; b += bsize(b)) {
        if (bused(b)) continue;
        /* merge with the following free blocks */
        while (b + bsize(b) < heap_end && !bused(b + bsize(b)))
            W32(b, bsize(b) + bsize(b + bsize(b)));
        if (bsize(b) < need) continue;
        if (bsize(b) >= need + HDR + 8u) {                /* split */
            uint32_t rest = b + need;
            W32(rest, bsize(b) - need);
            W32(rest + 4u, 0);
            W32(b, need | 1u);
        } else {
            W32(b, bsize(b) | 1u);
        }
        W32(b + 4u, 0);
        if (b == first_free) first_free = b + bsize(b);
        return b + HDR;
    }
    return 0;
}

void heap_free(uint32_t p) {
    uint32_t b;
    if (p < HEAP_BASE + HDR || p >= heap_end) return;
    b = p - HDR;
    if (!bused(b)) return;
    W32(b, bsize(b));
    if (b < first_free) first_free = b;
}

uint32_t heap_size(uint32_t p) {
    if (p < HEAP_BASE + HDR || p >= heap_end) return 0;
    return bsize(p - HDR) - HDR;
}

uint32_t heap_realloc(uint32_t p, uint32_t n) {
    uint32_t old, q;
    if (!p) return heap_alloc(n);
    old = heap_size(p);
    if (n <= old) return p;
    q = heap_alloc(n);
    if (!q) return 0;
    memmove(M + q, M + p, old);
    heap_free(p);
    return q;
}

/* heap usage: bytes in use, blocks in use, largest free block (log only) */
void heap_stats(uint32_t *used, uint32_t *nused, uint32_t *maxfree) {
    uint32_t b = HEAP_BASE;
    *used = *nused = *maxfree = 0;
    while (b < heap_end && bsize(b)) {
        if (bused(b)) { *used += bsize(b); (*nused)++; }
        else if (bsize(b) > *maxfree) *maxfree = bsize(b);
        b += bsize(b);
    }
}
