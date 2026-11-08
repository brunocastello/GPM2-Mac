/* Software sharp bilinear (see sharpscale.h). Integer arithmetic; the pre-scaled image is not materialized:
 * the position P(x) of the image k times larger maps to source pixel x / k. */
#include "sharpscale.h"
#include <stdlib.h>

static void axis(int n, int k, int t, int *i0, int *i1, int *w) {
    int pn = n * k, o;
    for (o = 0; o < t; o++) {
        long u = (long)((2L * o + 1) * pn) * 128L / t - 128L;        /* position in 1/256 pixel of the pre-scaled image */
        int a, b;
        if (u < 0) u = 0;
        a = (int)(u >> 8); b = a + 1;
        if (a > pn - 1) a = pn - 1;
        if (b > pn - 1) b = pn - 1;
        i0[o] = a / k; i1[o] = b / k; w[o] = (int)(u & 255);
    }
}

int sharp_scale(const uint8_t *src, int sw, int sh, const uint8_t pal[256][4], int tw, int th, uint32_t *dst, int dstride) {
    int k = tw / sw, kv = th / sh, *tx, *y0, *y1, *wy, *x0, *x1, *wx, x, y;
    uint32_t rgb[256];
    if (kv < k) k = kv;
    if (k < 1) k = 1;
    tx = (int *)malloc(sizeof(int) * (size_t)(3 * tw + 3 * th));
    if (!tx) return 0;
    x0 = tx; x1 = x0 + tw; wx = x1 + tw; y0 = wx + tw; y1 = y0 + th; wy = y1 + th;
    axis(sw, k, tw, x0, x1, wx);
    axis(sh, k, th, y0, y1, wy);
    for (x = 0; x < 256; x++) rgb[x] = ((uint32_t)pal[x][0] << 16) | ((uint32_t)pal[x][1] << 8) | pal[x][2];
    for (y = 0; y < th; y++) {
        const uint8_t *r0 = src + (long)y0[y] * sw, *r1 = src + (long)y1[y] * sw;
        uint32_t *o = (uint32_t *)((char *)dst + (long)y * dstride);
        int fy = wy[y];
        for (x = 0; x < tw; x++) {
            int a = r0[x0[x]], b = r0[x1[x]], c = r1[x0[x]], d = r1[x1[x]], fx = wx[x];
            if (a == b && b == c && c == d) { o[x] = rgb[a]; continue; }
            {
                uint32_t pa = rgb[a], pb = rgb[b], pc = rgb[c], pd = rgb[d], out = 0;
                int sft;
                for (sft = 16; sft >= 0; sft -= 8) {
                    int va = (int)((pa >> sft) & 255), vb = (int)((pb >> sft) & 255), vc = (int)((pc >> sft) & 255), vd = (int)((pd >> sft) & 255);
                    int top = va * (256 - fx) + vb * fx, bot = vc * (256 - fx) + vd * fx;
                    out |= (uint32_t)((top * (256 - fy) + bot * fy + 32768) >> 16) << sft;
                }
                o[x] = out;
            }
        }
    }
    free(tx);
    return 1;
}
