/* WING32 (WinG 1.0): 8 bpp DCs and DIBs in guest memory.
 * WinGBitBlt/WinGStretchBlt to a window DC copy indices and present the frame. */
#include "gdi.h"

#include <string.h>

static uint32_t w_WinGCreateDC(void) {
    uint32_t h = gdi_new_dc();
    obj_get(h, OB_DC)->u.dc.hbm = gdi_new_bitmap(1, 1, 1);
    obj_get(h, OB_DC)->u.dc.defbm = obj_get(h, OB_DC)->u.dc.hbm;
    return h;
}

/* BITMAPINFOHEADER: biSize +0, biWidth +4, biHeight +8, biPlanes +12, biBitCount +14, biCompression +16 */
static uint32_t w_WinGRecommendDIBFormat(void) {
    uint32_t p = ARG(0);
    for (uint32_t i = 0; i < 40; i += 4) W32(p + i, 0);
    W32(p, 40); W32(p + 4, 1); W32(p + 8, 0xFFFFFFFFu);   /* 1 x -1: top-down DIB */
    W16(p + 12, 1); W16(p + 14, 8); W32(p + 16, 0);       /* 8 bpp, BI_RGB */
    return 1;
}

static uint32_t w_WinGCreateBitmap(void) {
    uint32_t bi = ARG(1), ppbits = ARG(2);
    int w = (int)R32(bi + 4), h = (int)R32(bi + 8), bpp = (int)R16(bi + 14), td = 0;
    uint32_t hb;
    if (bpp != 8) win32_die("WinGCreateBitmap: %d bpp not supported", bpp);
    if (h < 0) { h = -h; td = 1; }
    hb = gdi_new_bitmap(w, h, td);
    if (ppbits) W32(ppbits, obj_get(hb, OB_BITMAP)->u.bm.bits);
    return hb;
}

static uint32_t w_WinGGetDIBPointer(void) {
    Obj *o = obj_get(ARG(0), OB_BITMAP);
    uint32_t bi = ARG(1);
    Bitmap *b;
    if (!o) return 0;
    b = &o->u.bm;
    if (bi) {
        W32(bi, 40); W32(bi + 4, (uint32_t)b->w); W32(bi + 8, (uint32_t)(b->topdown ? -b->h : b->h));
        W16(bi + 12, 1); W16(bi + 14, 8); W32(bi + 16, 0);
        W32(bi + 20, (uint32_t)(b->stride * b->h));
    }
    return b->bits;
}

static uint32_t w_WinGSetDIBColorTable(void) {
    Bitmap *b = dc_bitmap(ARG(0));
    uint32_t start = ARG(1), n = ARG(2), src = ARG(3), i;
    if (!b) return 0;
    for (i = 0; i < n && start + i < 256u; i++) {
        uint32_t q = R32(src + 4u * i);                /* RGBQUAD: blue, green, red */
        b->ct[start + i][0] = (uint8_t)(q >> 16); b->ct[start + i][1] = (uint8_t)(q >> 8);
        b->ct[start + i][2] = (uint8_t)q; b->ct[start + i][3] = 0;
    }
    b->has_ct = 1;
    return i;
}

static uint32_t w_WinGBitBlt(void) {
    uint32_t hd = ARG(0);
    Obj *d = obj_get(hd, OB_DC);
    int ok = gdi_blit(hd, (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), ARG(5), (int)ARG(6), (int)ARG(7), 0x00CC0020);
    if (ok && d && d->u.dc.hwnd) user_window_drawn(d->u.dc.hwnd, "WING32.dll!WinGBitBlt");
    return 1;
}

static uint32_t w_WinGStretchBlt(void) {
    uint32_t hd = ARG(0);
    Obj *d = obj_get(hd, OB_DC);
    int ok = gdi_stretch(hd, (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), ARG(5), (int)ARG(6), (int)ARG(7),
                         (int)ARG(8), (int)ARG(9), 0x00CC0020);
    if (ok && d && d->u.dc.hwnd) user_window_drawn(d->u.dc.hwnd, "WING32.dll!WinGStretchBlt");
    return 1;
}

void wing32_register(void) {
#define R(n, f) win32_register("WING32.dll!" n, f)
    R("WinGCreateDC", w_WinGCreateDC); R("WinGRecommendDIBFormat", w_WinGRecommendDIBFormat);
    R("WinGCreateBitmap", w_WinGCreateBitmap); R("WinGGetDIBPointer", w_WinGGetDIBPointer);
    R("WinGSetDIBColorTable", w_WinGSetDIBColorTable); R("WinGBitBlt", w_WinGBitBlt);
    R("WinGStretchBlt", w_WinGStretchBlt);
#undef R
}
