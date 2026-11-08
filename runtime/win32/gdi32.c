/* GDI32 (stdcall) over 8 bpp indexed surfaces.
 *
 * Palette model: the system palette (`sys_pal`) is the one that goes out in the frame. It starts with the 20
 * reserved Windows 95 colors; RealizePalette copies the selected logical palette into it:
 * SYSPAL_NOSTATIC = identity over all 256 entries; SYSPAL_STATIC = only entries 10..245.
 * Blits copy indices (assumes identity mapping, as WinG recommends).
 * Text: the game's own font (see below). Lines and polygons: stubs.
 */
#include "gdi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OBJ 8192
static Obj *objs[MAX_OBJ];
static unsigned nobj_slots;

uint8_t sys_pal[256][4];
int sys_pal_use = 1;

static const uint8_t static20[20][3] = {
    {0,0,0},{128,0,0},{0,128,0},{128,128,0},{0,0,128},{128,0,128},{0,128,128},{192,192,192},{192,220,192},{166,202,240},
    {255,251,240},{160,160,164},{128,128,128},{255,0,0},{0,255,0},{255,255,0},{0,0,255},{255,0,255},{0,255,255},{255,255,255}};

static uint32_t syscolors[25] = {
    0x00C0C0C0, 0x00808000, 0x00800000, 0x00808080, 0x00C0C0C0, 0x00FFFFFF, 0x00000000, 0x00000000, 0x00000000,
    0x00FFFFFF, 0x00C0C0C0, 0x00C0C0C0, 0x00808080, 0x00800000, 0x00FFFFFF, 0x00C0C0C0, 0x00808080, 0x00808080,
    0x00000000, 0x00C0C0C0, 0x00FFFFFF, 0x00000000, 0x00C0C0C0, 0x00000000, 0x00E1FFFF};

uint32_t gdi_sys_color(int idx) { return idx >= 0 && idx < 25 ? syscolors[idx] : 0; }

uint32_t obj_new(int type) {
    unsigned i;
    for (i = 0; i < nobj_slots; i++) if (!objs[i]) break;
    if (i == MAX_OBJ) win32_die("GDI object table full");
    if (i == nobj_slots) nobj_slots++;
    objs[i] = (Obj *)calloc(1, sizeof(Obj));
    objs[i]->type = type;
    return H_BASE + 4u * i;
}
Obj *obj_get(uint32_t h, int type) {
    unsigned i;
    if (h < H_BASE || (h - H_BASE) % 4u) return 0;
    i = (h - H_BASE) / 4u;
    if (i >= nobj_slots || !objs[i]) return 0;
    if (type && objs[i]->type != type) return 0;
    return objs[i];
}
void obj_free(uint32_t h) {
    unsigned i;
    if (h < H_BASE || (h - H_BASE) % 4u) return;
    i = (h - H_BASE) / 4u;
    if (i >= nobj_slots || !objs[i]) return;
    if (objs[i]->type == OB_BITMAP) heap_free(objs[i]->u.bm.bits);       /* pixels in the guest heap */
    else if (objs[i]->type == OB_DC && objs[i]->u.dc.defbm) {
        uint32_t d = objs[i]->u.dc.defbm;
        objs[i]->u.dc.defbm = 0;
        obj_free(d);
    }
    free(objs[i]); objs[i] = 0;
}

/* counters for the log (leaks): live objects by type and bitmap bytes */
void gdi_stats(unsigned cnt[8], unsigned long long *bm_bytes) {
    memset(cnt, 0, 8 * sizeof(unsigned));
    *bm_bytes = 0;
    for (unsigned i = 0; i < nobj_slots; i++) {
        if (!objs[i]) continue;
        cnt[objs[i]->type & 7]++;
        if (objs[i]->type == OB_BITMAP) *bm_bytes += (unsigned long long)objs[i]->u.bm.stride * (unsigned)objs[i]->u.bm.h;
    }
}

uint32_t gdi_new_bitmap(int w, int h, int topdown) {
    uint32_t hb = obj_new(OB_BITMAP);
    Bitmap *b = &obj_get(hb, OB_BITMAP)->u.bm;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    b->w = w; b->h = h; b->stride = (w + 3) & ~3; b->topdown = topdown;
    b->bits = heap_alloc((uint32_t)(b->stride * h));
    if (!b->bits) win32_die("out of guest memory for bitmap %dx%d", w, h);
    memset(M + b->bits, 0, (size_t)(b->stride * h));
    return hb;
}
uint8_t *bm_row(const Bitmap *b, int y) {
    return M + b->bits + (uint32_t)((b->topdown ? y : b->h - 1 - y) * b->stride);
}

uint32_t gdi_new_dc(void) {
    uint32_t h = obj_new(OB_DC);
    DC *d = &obj_get(h, OB_DC)->u.dc;
    d->textcolor = 0; d->bkcolor = 0x00FFFFFFu; d->bkmode = 2;
    return h;
}
Bitmap *dc_bitmap(uint32_t hdc) {
    Obj *o = obj_get(hdc, OB_DC);
    Obj *b;
    if (!o) return 0;
    b = obj_get(o->u.dc.hbm, OB_BITMAP);
    return b ? &b->u.bm : 0;
}

/* ---- colors ---------------------------------------------------------------------------------- */
uint32_t gdi_color_index(uint32_t c) {
    int best = 0, bd = 0x7FFFFFFF;
    uint32_t r = c & 0xFF, g_ = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    if ((c >> 24) == 1) return c & 0xFF;                      /* PALETTEINDEX */
    for (int i = 0; i < 256; i++) {
        int dr = (int)r - sys_pal[i][0], dg = (int)g_ - sys_pal[i][1], db = (int)b - sys_pal[i][2];
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; if (d == 0) break; }
    }
    return (uint32_t)best;
}

static uint32_t stock[20];

void gdi_init(void) {
    for (int i = 0; i < 10; i++) { sys_pal[i][0] = static20[i][0]; sys_pal[i][1] = static20[i][1]; sys_pal[i][2] = static20[i][2]; }
    for (int i = 0; i < 10; i++) {
        sys_pal[246 + i][0] = static20[10 + i][0]; sys_pal[246 + i][1] = static20[10 + i][1]; sys_pal[246 + i][2] = static20[10 + i][2];
    }
}

static uint32_t stock_obj(uint32_t n) {
    if (n >= 20) return 0;
    if (!stock[n]) {
        uint32_t h;
        if (n <= 5) {                                       /* brushes */
            static const uint32_t col[] = {0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000, 0};
            h = obj_new(OB_BRUSH);
            obj_get(h, OB_BRUSH)->u.br.color = col[n];
            obj_get(h, OB_BRUSH)->u.br.style = n == 5 ? 1 : 0;      /* 1 = BS_HOLLOW */
        } else if (n <= 8) {                                /* pens */
            h = obj_new(OB_PEN);
            obj_get(h, OB_PEN)->u.pen.color = n == 6 ? 0xFFFFFFu : 0;
            obj_get(h, OB_PEN)->u.pen.style = n == 8 ? 5 : 0;      /* PS_NULL */
        } else if (n == 15) {                               /* DEFAULT_PALETTE */
            h = obj_new(OB_PALETTE);
            Palette *p = &obj_get(h, OB_PALETTE)->u.pal;
            p->n = 20;
            for (int i = 0; i < 20; i++) { p->e[i][0] = static20[i][0]; p->e[i][1] = static20[i][1]; p->e[i][2] = static20[i][2]; }
        } else {                                            /* fonts */
            h = obj_new(OB_FONT);
            snprintf(obj_get(h, OB_FONT)->u.font.face, 32, "%s", n == 13 ? "System" : "Fixedsys");
            obj_get(h, OB_FONT)->u.font.height = 16;
        }
        stock[n] = h;
    }
    return stock[n];
}

/* ---- blit ---------------------------------------------------------------------------------- */
static uint8_t rop_apply(uint32_t rop, uint8_t s, uint8_t d, uint8_t p) {
    switch (rop) {
    case 0x00CC0020: return s;                    /* SRCCOPY */
    case 0x00330008: return (uint8_t)~s;          /* NOTSRCCOPY */
    case 0x00EE0086: return (uint8_t)(s | d);     /* SRCPAINT */
    case 0x008800C6: return (uint8_t)(s & d);     /* SRCAND */
    case 0x00660046: return (uint8_t)(s ^ d);     /* SRCINVERT */
    case 0x00440328: return (uint8_t)(s & ~d);    /* SRCERASE */
    case 0x00550009: return (uint8_t)~d;          /* DSTINVERT */
    case 0x00F00021: return p;                    /* PATCOPY */
    case 0x005A0049: return (uint8_t)(p ^ d);     /* PATINVERT */
    case 0x00000042: return 0;                    /* BLACKNESS */
    case 0x00FF0062: return 255;                  /* WHITENESS */
    default: win32_stub_note("BitBlt(rop)"); return s;
    }
}

int gdi_stretch(uint32_t hdst, int xd, int yd, int wd, int hd, uint32_t hsrc, int xs, int ys, int ws, int hs, uint32_t rop) {
    Bitmap *D = dc_bitmap(hdst), *S = hsrc ? dc_bitmap(hsrc) : 0;
    int flipx = 0, flipy = 0, x, y, any = 0;
    if (!D) return 0;
    if (wd < 0) { xd += wd; wd = -wd; flipx = 1; }
    if (hd < 0) { yd += hd; hd = -hd; flipy = 1; }
    if (wd == 0 || hd == 0) return 0;
    if (!S && rop != 0x00F00021 && rop != 0x42 && rop != 0x00FF0062 && rop != 0x00550009) return 0;
    for (y = 0; y < hd; y++) {
        int dy = yd + y, sy;
        if (dy < 0 || dy >= D->h) continue;
        sy = flipy ? ys + (hd - 1 - y) * hs / hd : ys + y * hs / hd;
        if (S && (sy < 0 || sy >= S->h)) continue;
        for (x = 0; x < wd; x++) {
            int dx = xd + x, sx;
            uint8_t *dp, sv = 0;
            if (dx < 0 || dx >= D->w) continue;
            sx = flipx ? xs + (wd - 1 - x) * ws / wd : xs + x * ws / wd;
            if (S) { if (sx < 0 || sx >= S->w) continue; sv = bm_row(S, sy)[sx]; }
            dp = bm_row(D, dy) + dx;
            *dp = rop_apply(rop, sv, *dp, (uint8_t)gdi_color_index(obj_get(obj_get(hdst, OB_DC)->u.dc.hbrush, OB_BRUSH) ? obj_get(obj_get(hdst, OB_DC)->u.dc.hbrush, OB_BRUSH)->u.br.color : 0));
            any = 1;
        }
    }
    return any;
}

int gdi_blit(uint32_t hdst, int xd, int yd, int w, int h, uint32_t hsrc, int xs, int ys, uint32_t rop) {
    Bitmap *D = dc_bitmap(hdst), *S = hsrc ? dc_bitmap(hsrc) : 0;
    int x0, y0, x1, y1, y, any = 0;
    if (!D) return 0;
    if (rop == 0x00CC0020 && S) {                           /* fast path: SRCCOPY */
        int cx0 = 0, cy0 = 0;
        x0 = xd; y0 = yd; x1 = xd + w; y1 = yd + h;
        if (x0 < 0) { cx0 = -x0; x0 = 0; }
        if (y0 < 0) { cy0 = -y0; y0 = 0; }
        if (x1 > D->w) x1 = D->w;
        if (y1 > D->h) y1 = D->h;
        if (xs + cx0 < 0) { x0 += -(xs + cx0); cx0 += -(xs + cx0); }
        if (ys + cy0 < 0) { y0 += -(ys + cy0); cy0 += -(ys + cy0); }
        if (x1 - x0 > S->w - (xs + cx0)) x1 = x0 + (S->w - (xs + cx0));
        if (y1 - y0 > S->h - (ys + cy0)) y1 = y0 + (S->h - (ys + cy0));
        if (x1 <= x0 || y1 <= y0) return 0;
        for (y = y0; y < y1; y++)
            memmove(bm_row(D, y) + x0, bm_row(S, ys + cy0 + (y - y0)) + xs + cx0, (size_t)(x1 - x0));
        return 1;
    }
    return gdi_stretch(hdst, xd, yd, w, h, hsrc, xs, ys, w, h, rop) || any;
}

void gdi_fill(uint32_t hdc, int x, int y, int w, int h, uint32_t brush) {
    Bitmap *D = dc_bitmap(hdc);
    Obj *b;
    uint8_t idx;
    int xx, yy;
    if (!D || w <= 0 || h <= 0) return;
    if (brush > 0 && brush < 32) idx = (uint8_t)gdi_color_index(gdi_sys_color((int)brush - 1));
    else {
        b = obj_get(brush, OB_BRUSH);
        if (!b || b->u.br.style == 1) return;
        if (b->u.br.pattern) {                               /* pattern brush: tiles the 8x8 bitmap */
            Obj *pb = obj_get(b->u.br.pattern, OB_BITMAP);
            if (pb) {
                Bitmap *P = &pb->u.bm;
                for (yy = y; yy < y + h; yy++) {
                    if (yy < 0 || yy >= D->h) continue;
                    for (xx = x; xx < x + w; xx++)
                        if (xx >= 0 && xx < D->w) bm_row(D, yy)[xx] = bm_row(P, ((yy % P->h) + P->h) % P->h)[((xx % P->w) + P->w) % P->w];
                }
                return;
            }
        }
        idx = (uint8_t)gdi_color_index(b->u.br.color);
    }
    for (yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= D->h) continue;
        for (xx = x; xx < x + w; xx++) if (xx >= 0 && xx < D->w) bm_row(D, yy)[xx] = idx;
    }
}

/* ---- API ----------------------------------------------------------------------------------- */
static uint32_t g_CreateCompatibleDC(void) {
    uint32_t h = gdi_new_dc();
    uint32_t def = gdi_new_bitmap(1, 1, 1);
    obj_get(h, OB_DC)->u.dc.hbm = def;
    obj_get(h, OB_DC)->u.dc.defbm = def;
    return h;
}
static uint32_t g_DeleteDC(void) {
    Obj *o = obj_get(ARG(0), OB_DC);
    if (!o) return 0;
    obj_free(ARG(0));
    return 1;
}
static uint32_t g_CreateCompatibleBitmap(void) { return gdi_new_bitmap((int)ARG(1), (int)ARG(2), 1); }
static uint32_t g_CreateBitmapIndirect(void) {
    uint32_t p = ARG(0), bits = R32(p + 20);
    int w = (int)R32(p + 4), h = (int)R32(p + 8), wb = (int)R16(p + 12), bpp = (int)R16(p + 18);
    uint32_t hb = gdi_new_bitmap(w, h, 1);
    Bitmap *b = &obj_get(hb, OB_BITMAP)->u.bm;
    if (bits) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t a = bits + (uint32_t)(y * wb);
                uint8_t v = bpp == 8 ? (uint8_t)R8(a + (uint32_t)x) : bpp == 1 ? (uint8_t)(((R8(a + (uint32_t)(x >> 3)) >> (7 - (x & 7))) & 1) ? 255 : 0) : 0;
                bm_row(b, y)[x] = v;
            }
        if (bpp != 8 && bpp != 1) win32_stub_note("CreateBitmapIndirect(bpp)");
    }
    return hb;
}
static uint32_t g_SelectObject(void) {
    Obj *d = obj_get(ARG(0), OB_DC), *o = obj_get(ARG(1), 0);
    uint32_t old = 0;
    if (!d || !o) return 0;
    switch (o->type) {
    case OB_BITMAP: old = d->u.dc.hbm; d->u.dc.hbm = ARG(1); break;
    case OB_BRUSH: old = d->u.dc.hbrush ? d->u.dc.hbrush : stock_obj(0); d->u.dc.hbrush = ARG(1); break;
    case OB_PEN: old = d->u.dc.hpen ? d->u.dc.hpen : stock_obj(7); d->u.dc.hpen = ARG(1); break;
    case OB_FONT: old = d->u.dc.hfont ? d->u.dc.hfont : stock_obj(13); d->u.dc.hfont = ARG(1); break;
    default: break;
    }
    return old;
}
static uint32_t g_DeleteObject(void) {
    Obj *o = obj_get(ARG(0), 0);
    if (!o) return 0;
    for (int i = 0; i < 20; i++) if (stock[i] == ARG(0)) return 1;      /* stock objects never die */
    obj_free(ARG(0));
    return 1;
}
static uint32_t g_GetStockObject(void) { return stock_obj(ARG(0)); }
static uint32_t g_CreateSolidBrush(void) {
    uint32_t h = obj_new(OB_BRUSH);
    obj_get(h, OB_BRUSH)->u.br.color = ARG(0);
    return h;
}
static uint32_t g_CreateBrushIndirect(void) {
    uint32_t h = obj_new(OB_BRUSH), p = ARG(0);
    Brush *b = &obj_get(h, OB_BRUSH)->u.br;
    b->style = (int)R32(p) == 1 ? 1 : 0;
    b->color = R32(p + 4);
    if (R32(p) == 3) b->pattern = R32(p + 8);
    return h;
}
static uint32_t g_CreatePatternBrush(void) {
    uint32_t h = obj_new(OB_BRUSH);
    obj_get(h, OB_BRUSH)->u.br.pattern = ARG(0);
    return h;
}
static uint32_t g_CreatePen(void) {
    uint32_t h = obj_new(OB_PEN);
    Pen *p = &obj_get(h, OB_PEN)->u.pen;
    p->style = (int)ARG(0); p->width = (int)ARG(1); p->color = ARG(2);
    return h;
}
static uint32_t g_CreateFontA(void) {
    uint32_t h = obj_new(OB_FONT);
    Font *f = &obj_get(h, OB_FONT)->u.font;
    f->height = (int)ARG(0); f->width = (int)ARG(1); f->weight = (int)ARG(4);
    gstr_get(f->face, sizeof f->face, ARG(13));
    return h;
}
static uint32_t g_SetBkMode(void) {
    Obj *d = obj_get(ARG(0), OB_DC);
    int old;
    if (!d) return 0;
    old = d->u.dc.bkmode; d->u.dc.bkmode = (int)ARG(1);
    return (uint32_t)old;
}
static uint32_t g_SetBkColor(void) {
    Obj *d = obj_get(ARG(0), OB_DC);
    uint32_t old;
    if (!d) return 0xFFFFFFFFu;
    old = d->u.dc.bkcolor; d->u.dc.bkcolor = ARG(1);
    return old;
}
static uint32_t g_SetTextColor(void) {
    Obj *d = obj_get(ARG(0), OB_DC);
    uint32_t old;
    if (!d) return 0xFFFFFFFFu;
    old = d->u.dc.textcolor; d->u.dc.textcolor = ARG(1);
    return old;
}

/* palettes */
static uint32_t g_CreatePalette(void) {
    uint32_t p = ARG(0), h = obj_new(OB_PALETTE);
    Palette *pal = &obj_get(h, OB_PALETTE)->u.pal;
    int n = (int)R16(p + 2);
    if (n > 256) n = 256;
    pal->n = n;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 4; k++) pal->e[i][k] = (uint8_t)R8(p + 4u + 4u * (uint32_t)i + (uint32_t)k);
    return h;
}
static uint32_t g_SelectPalette(void) {
    Obj *d = obj_get(ARG(0), OB_DC);
    uint32_t old;
    if (!d || !obj_get(ARG(1), OB_PALETTE)) return 0;
    old = d->u.dc.hpal ? d->u.dc.hpal : stock_obj(15);
    d->u.dc.hpal = ARG(1);
    return old;
}
static uint32_t g_RealizePalette(void) {
    Obj *d = obj_get(ARG(0), OB_DC), *po;
    uint32_t changed = 0;
    int lo, hi;
    if (!d || !d->u.dc.hpal) return 0;
    po = obj_get(d->u.dc.hpal, OB_PALETTE);
    if (!po) return 0;
    lo = sys_pal_use == 1 ? 10 : 0;
    hi = sys_pal_use == 1 ? 246 : 256;
    for (int i = lo; i < hi && i < po->u.pal.n; i++) {
        if (memcmp(sys_pal[i], po->u.pal.e[i], 3) != 0) changed++;
        sys_pal[i][0] = po->u.pal.e[i][0]; sys_pal[i][1] = po->u.pal.e[i][1];
        sys_pal[i][2] = po->u.pal.e[i][2]; sys_pal[i][3] = po->u.pal.e[i][3];
    }
    return changed;
}
static uint32_t g_GetPaletteEntries(void) {
    Obj *po = obj_get(ARG(0), OB_PALETTE);
    uint32_t start = ARG(1), n = ARG(2), out = ARG(3), i;
    if (!po) return 0;
    for (i = 0; i < n && start + i < (uint32_t)po->u.pal.n; i++)
        gmem_put(out + 4u * i, po->u.pal.e[start + i], 4);
    return i;
}
static uint32_t g_GetSystemPaletteEntries(void) {
    uint32_t start = ARG(1), n = ARG(2), out = ARG(3), i;
    for (i = 0; i < n && start + i < 256u; i++) gmem_put(out + 4u * i, sys_pal[start + i], 4);
    return i;
}
static uint32_t g_SetSystemPaletteUse(void) {
    int old = sys_pal_use;
    if (ARG(1) == 1 || ARG(1) == 2 || ARG(1) == 3) sys_pal_use = (int)ARG(1);
    return (uint32_t)old;
}
static uint32_t g_GetSystemPaletteUse(void) { return (uint32_t)sys_pal_use; }

static uint32_t g_GetDeviceCaps(void) {
    switch (ARG(1)) {
    case 0: return 0x400;           /* DRIVERVERSION */
    case 2: return 1;               /* TECHNOLOGY = DT_RASDISPLAY */
    case 4: return 320;  case 6: return 240;             /* HORZSIZE, VERTSIZE (mm) */
    case 8: return 640;  case 10: return 480;            /* HORZRES, VERTRES */
    case 12: return 8;   case 14: return 1;              /* BITSPIXEL, PLANES */
    case 16: return 0xFFFFFFFFu;                         /* NUMBRUSHES */
    case 18: return 0xFFFFFFFFu; case 20: return 0xFFFFFFFFu; case 22: return 0xFFFFFFFFu;
    case 24: return 20;                                  /* NUMCOLORS (reserved colors) */
    case 26: return 0;
    case 28: return 0xFF; case 30: return 0x7F; case 32: return 0xFF; case 34: return 0x7E19;
    case 36: return 1;                                   /* CLIPCAPS */
    case 38: return 0x7F99;                              /* RASTERCAPS (includes RC_PALETTE 0x100) */
    case 40: case 42: return 36; case 44: return 51;
    case 88: case 90: return 96;                         /* LOGPIXELSX/Y */
    case 104: return 256;                                /* SIZEPALETTE */
    case 106: return 20;                                 /* NUMRESERVED */
    case 108: return 18;                                 /* COLORRES */
    default: win32_stub_note("GetDeviceCaps(?)"); return 0;
    }
}

static uint32_t g_BitBlt(void) {
    uint32_t hd = ARG(0), hs = ARG(5);
    int ok = gdi_blit(hd, (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), hs, (int)ARG(6), (int)ARG(7), ARG(8));
    Obj *d = obj_get(hd, OB_DC);
    if (ok && d && d->u.dc.hwnd) user_window_drawn(d->u.dc.hwnd, "GDI32.dll!BitBlt");
    return 1;
}

/* ---- GDI text (TextOutA, ExtTextOutA, DrawTextA, GetTextExtentPoint32A, GetTextMetricsA) ------------------------
 * Windows 95 would use the MS Sans Serif font; it is not in the game files. We use the game's own font,
 * FONT15.EDY (16x16 cells, glyph 0 = ASCII 32, ink = value 15), with proportional advance by ink width.
 * If the file is missing, nothing is drawn but the measurements stay valid. Line height 14 (ascent 11, descent 3). */
#define LINE_H 14
#define NGLYPH 224
static struct { int loaded, ok; uint8_t g[NGLYPH * 256]; uint8_t adv[NGLYPH]; } tf;
static void tf_load(void) {
    char host[2048];
    FILE *f;
    if (tf.loaded) return;
    tf.loaded = 1;
    vfs_resolve("C:\\GPM2\\FONT15.EDY", 0, host, sizeof host);
    f = fopen(host, "rb");
    if (f) { tf.ok = fread(tf.g, 1, sizeof tf.g, f) == sizeof tf.g; fclose(f); }
    for (int i = 0; i < NGLYPH; i++) {
        int mx = -1;
        if (tf.ok) for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) if (tf.g[i * 256 + r * 16 + c] && c > mx) mx = c;
        tf.adv[i] = (uint8_t)(i == 0 ? 6 : mx < 0 ? 8 : mx + 3);
    }
}
static int ch_adv(unsigned char c) { tf_load(); return c < 32 ? 0 : tf.adv[c - 32]; }
static int str_w(const char *s, int n) { int w = 0; for (int i = 0; i < n; i++) w += ch_adv((unsigned char)s[i]); return w; }

static void draw_str(uint32_t hdc, int x, int y, const char *s, int n, int opaque_w) {
    Obj *d = obj_get(hdc, OB_DC);
    Bitmap *B = dc_bitmap(hdc);
    uint8_t ink, bk;
    if (!d || !B) return;
    tf_load();
    ink = (uint8_t)gdi_color_index(d->u.dc.textcolor); bk = (uint8_t)gdi_color_index(d->u.dc.bkcolor);
    if (d->u.dc.bkmode == 2 && opaque_w > 0)
        for (int yy = y; yy < y + LINE_H; yy++) {
            if (yy < 0 || yy >= B->h) continue;
            for (int xx = x; xx < x + opaque_w; xx++) if (xx >= 0 && xx < B->w) bm_row(B, yy)[xx] = bk;
        }
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 32 && tf.ok)
            for (int r = 0; r < 16; r++) {
                int yy = y + r;
                if (yy < 0 || yy >= B->h) continue;
                for (int cc = 0; cc < 16; cc++) {
                    int xx = x + cc;
                    if (xx >= 0 && xx < B->w && tf.g[(c - 32) * 256 + r * 16 + cc]) bm_row(B, yy)[xx] = ink;
                }
            }
        x += ch_adv(c);
    }
}
static void text_done(uint32_t hdc, const char *who) {
    Obj *d = obj_get(hdc, OB_DC);
    if (d && d->u.dc.hwnd) user_window_drawn(d->u.dc.hwnd, who);
}
static int guest_len(uint32_t p, int n) { return n < 0 ? (int)gstr_len(p) : n; }
static char *guest_text(uint32_t p, int n) {
    char *t = (char *)malloc((size_t)n + 1u);
    if (n) gmem_get(t, p, (uint32_t)n);
    t[n] = 0;
    return t;
}
static uint32_t g_TextOutA(void) {
    int n = guest_len(ARG(3), (int)ARG(4));
    char *t = guest_text(ARG(3), n);
    draw_str(ARG(0), (int)ARG(1), (int)ARG(2), t, n, str_w(t, n));
    free(t);
    text_done(ARG(0), "GDI32.dll!TextOutA");
    return 1;
}
static uint32_t g_ExtTextOutA(void) {
    uint32_t r = ARG(4);
    int n = (int)ARG(6);
    char *t = guest_text(ARG(5), n);
    if ((ARG(3) & 2u) && r) {                                  /* ETO_OPAQUE: fills the rectangle with the background color */
        Obj *d = obj_get(ARG(0), OB_DC);
        Bitmap *B = dc_bitmap(ARG(0));
        if (d && B) {
            uint8_t bk = (uint8_t)gdi_color_index(d->u.dc.bkcolor);
            for (int yy = (int)R32(r + 4); yy < (int)R32(r + 12); yy++)
                if (yy >= 0 && yy < B->h) for (int xx = (int)R32(r); xx < (int)R32(r + 8); xx++) if (xx >= 0 && xx < B->w) bm_row(B, yy)[xx] = bk;
        }
    }
    draw_str(ARG(0), (int)ARG(1), (int)ARG(2), t, n, 0);
    free(t);
    text_done(ARG(0), "GDI32.dll!ExtTextOutA");
    return 1;
}
#define DT_CENTER 1
#define DT_RIGHT 2
#define DT_VCENTER 4
#define DT_BOTTOM 8
#define DT_WORDBREAK 0x10
#define DT_SINGLELINE 0x20
#define DT_NOCLIP 0x100
#define DT_CALCRECT 0x400
#define DT_NOPREFIX 0x800
/* wraps into lines: returns the number of lines; start/len per line */
static int dt_lines(const char *s, int n, int maxw, uint32_t fl, int *st, int *ln, int max) {
    int k = 0, i = 0;
    while (k < max) {
        int b = i, w = 0, last = -1;
        while (i < n && s[i] != '\n') {
            int a = ch_adv((unsigned char)s[i]);
            if (s[i] == '\r') { i++; continue; }
            if ((fl & DT_WORDBREAK) && !(fl & DT_SINGLELINE) && w + a > maxw && i > b) {
                if (last > b) i = last + 1;                      /* back to the last space */
                break;
            }
            if (s[i] == ' ') last = i;
            w += a; i++;
        }
        st[k] = b; ln[k] = i - b; k++;
        if (i >= n) break;
        if (s[i] == '\n') i++;
        if (i >= n && s[i - 1] == '\n') { break; }
    }
    return k;
}
static uint32_t g_DrawTextA(void) {
    uint32_t hdc = ARG(0), r = ARG(3), fl = ARG(4);
    int n = guest_len(ARG(1), (int)ARG(2)), L, T, Rr, Bb, maxw, nl, y, st[256], ln[256], maxline = 0;
    char *t, *c;
    if (!r) return 0;
    t = guest_text(ARG(1), n);
    if (!(fl & DT_NOPREFIX)) {                                   /* strips the mnemonic '&' */
        c = t;
        for (char *q = t; *q; q++) { if (*q == '&' && q[1] && q[1] != '&') continue; if (*q == '&') q++; *c++ = *q; if (!*q) break; }
        *c = 0; n = (int)strlen(t);
    }
    L = (int)R32(r); T = (int)R32(r + 4); Rr = (int)R32(r + 8); Bb = (int)R32(r + 12);
    maxw = Rr - L;
    nl = dt_lines(t, n, maxw, fl, st, ln, 256);
    for (int i = 0; i < nl; i++) { int w = str_w(t + st[i], ln[i]); if (w > maxline) maxline = w; }
    if (fl & DT_CALCRECT) {
        if (!(fl & DT_WORDBREAK) || maxline < maxw) W32(r + 8, (uint32_t)(L + maxline));
        W32(r + 12, (uint32_t)(T + nl * LINE_H));
        free(t);
        return (uint32_t)(nl * LINE_H);
    }
    { Obj *dc = obj_get(hdc, OB_DC); if (dc && dc->u.dc.hwnd) dlg_paint_text(dc->u.dc.hwnd, t, L, T, maxw, Bb - T); }
    y = T;
    if (fl & DT_VCENTER) y = T + (Bb - T - nl * LINE_H) / 2;
    else if (fl & DT_BOTTOM) y = Bb - nl * LINE_H;
    for (int i = 0; i < nl; i++, y += LINE_H) {
        int w = str_w(t + st[i], ln[i]), x = L;
        if (fl & DT_CENTER) x = L + (maxw - w) / 2; else if (fl & DT_RIGHT) x = Rr - w;
        draw_str(hdc, x, y, t + st[i], ln[i], w);
    }
    free(t);
    text_done(hdc, "USER32.dll!DrawTextA");
    return (uint32_t)(nl * LINE_H);
}
static uint32_t g_GetTextExtentPoint32A(void) {
    int n = guest_len(ARG(1), (int)ARG(2));
    char *t = guest_text(ARG(1), n);
    W32(ARG(3), (uint32_t)str_w(t, n)); W32(ARG(3) + 4u, LINE_H);
    free(t);
    return 1;
}
static uint32_t g_GetTextMetricsA(void) {
    uint32_t p = ARG(1);
    for (uint32_t i = 0; i < 56; i += 4) W32(p + i, 0);
    W32(p, LINE_H); W32(p + 4, 11); W32(p + 8, 3);           /* height, ascent, descent */
    W32(p + 20, 8); W32(p + 24, 16); W32(p + 28, 400);       /* AveCharWidth, MaxCharWidth, weight */
    W32(p + 36, 96); W32(p + 40, 96);
    p += 44;
    W8(p, 32); W8(p + 1, 255); W8(p + 2, 32); W8(p + 3, 32);  /* first, last, default, break */
    W8(p + 8, 0x20 | 0x04); W8(p + 9, 0x00); W8(p + 10, 1); W8(p + 11, 0);   /* variable pitch... */
    return 1;
}
/* ---- interface primitives for the controls drawn by the core (controls.c) -------------------------------- */
static uint8_t ui_idx(int sys) { return (uint8_t)gdi_color_index(gdi_sys_color(sys)); }
static void ui_fill(Bitmap *B, int x, int y, int w, int h, uint8_t c) {
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= B->h) continue;
        for (int xx = x; xx < x + w; xx++) if (xx >= 0 && xx < B->w) bm_row(B, yy)[xx] = c;
    }
}
void gdi_ui_rect(uint32_t hbm, int x, int y, int w, int h, int sys) {
    Obj *o = obj_get(hbm, OB_BITMAP);
    if (o) ui_fill(&o->u.bm, x, y, w, h, ui_idx(sys));
}
/* style: 0 raised (button), 1 sunken (pressed button), 2 raised + black frame (default button), 3 field (sunken, white), 4 engraved */
void gdi_ui_edge(uint32_t hbm, int x, int y, int w, int h, int style) {
    Obj *o = obj_get(hbm, OB_BITMAP);
    Bitmap *B;
    uint8_t hi = ui_idx(20), lt = ui_idx(22), sh = ui_idx(16), dk = ui_idx(21), face = ui_idx(15), win = ui_idx(5);
    if (!o) return;
    B = &o->u.bm;
    if (style == 2) { ui_fill(B, x, y, w, 1, dk); ui_fill(B, x, y + h - 1, w, 1, dk); ui_fill(B, x, y, 1, h, dk); ui_fill(B, x + w - 1, y, 1, h, dk); x++; y++; w -= 2; h -= 2; }
    if (style == 4) { ui_fill(B, x, y, w, 1, sh); ui_fill(B, x, y + h - 1, w, 1, hi); ui_fill(B, x, y, 1, h, sh); ui_fill(B, x + w - 1, y, 1, h, hi); return; }
    ui_fill(B, x, y, w, h, style == 3 ? win : face);
    if (style == 3 || style == 1) {                       /* sunken: shadow on top/left, light on bottom/right */
        ui_fill(B, x, y, w, 1, sh); ui_fill(B, x, y, 1, h, sh);
        ui_fill(B, x, y + h - 1, w, 1, hi); ui_fill(B, x + w - 1, y, 1, h, hi);
        ui_fill(B, x + 1, y + 1, w - 2, 1, dk); ui_fill(B, x + 1, y + 1, 1, h - 2, dk);
        ui_fill(B, x + 1, y + h - 2, w - 2, 1, lt); ui_fill(B, x + w - 2, y + 1, 1, h - 2, lt);
        return;
    }
    ui_fill(B, x, y, w, 1, hi); ui_fill(B, x, y, 1, h, hi);                 /* raised */
    ui_fill(B, x, y + h - 1, w, 1, dk); ui_fill(B, x + w - 1, y, 1, h, dk);
    ui_fill(B, x + 1, y + 1, w - 2, 1, lt); ui_fill(B, x + 1, y + 1, 1, h - 2, lt);
    ui_fill(B, x + 1, y + h - 2, w - 2, 1, sh); ui_fill(B, x + w - 2, y + 1, 1, h - 2, sh);
}
/* single-line text in the rectangle; align 0 left, 1 center, 2 right; sys = GetSysColor index; vertically centered */
void gdi_ui_text(uint32_t hbm, int x, int y, int w, int h, const char *t, int align, int sys) {
    uint32_t dc = gdi_new_dc();
    Obj *d = obj_get(dc, OB_DC);
    int n = (int)strlen(t), tw = str_w(t, n), tx = x;
    d->u.dc.hbm = hbm; d->u.dc.bkmode = 1; d->u.dc.textcolor = gdi_sys_color(sys);
    if (align == 1) tx = x + (w - tw) / 2; else if (align == 2) tx = x + w - tw;
    draw_str(dc, tx, y + (h - LINE_H) / 2, t, n, 0);
    obj_free(dc);
}
int gdi_ui_text_w(const char *t) { return str_w(t, (int)strlen(t)); }

/* lines, polygons: still stubs (not used on the menu path) */

/* ---- lines and shapes (MoveToEx, LineTo, Polygon, Pie) in the 8 bpp core -------------------------------------- */
static int pen_idx(const DC *dc, int *width) {          /* pen color index of the DC; -1 = PS_NULL */
    uint32_t hp = dc->hpen ? dc->hpen : stock_obj(7);
    Obj *o = obj_get(hp, OB_PEN);
    if (!o || o->u.pen.style == 5) return -1;
    *width = o->u.pen.width > 1 ? o->u.pen.width : 1;
    return (int)gdi_color_index(o->u.pen.color);
}
static int brush_idx(const DC *dc) {                    /* -1 = hollow */
    uint32_t hb = dc->hbrush ? dc->hbrush : stock_obj(0);
    Obj *o = obj_get(hb, OB_BRUSH);
    if (!o || o->u.br.style == 1) return -1;
    return (int)gdi_color_index(o->u.br.color);
}
static void put_px(Bitmap *D, int x, int y, int idx) {
    if (x >= 0 && y >= 0 && x < D->w && y < D->h) bm_row(D, y)[x] = (uint8_t)idx;
}
static void stamp(Bitmap *D, int x, int y, int w, int idx) {      /* square pen tip, centered on (x,y) */
    int x0 = x - (w - 1) / 2, y0 = y - (w - 1) / 2;
    for (int j = 0; j < w; j++) for (int i = 0; i < w; i++) put_px(D, x0 + i, y0 + j, idx);
}
static void draw_line(Bitmap *D, int x0, int y0, int x1, int y1, int w, int idx) {      /* Bresenham */
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (;;) {
        stamp(D, x0, y0, w, idx);
        if (x0 == x1 && y0 == y1) break;
        if (2 * e >= dy) { e += dy; x0 += sx; }
        if (2 * e <= dx) { e += dx; y0 += sy; }
    }
}
static uint32_t g_MoveToEx(void) {
    Obj *d = obj_get(ARG(0), OB_DC);
    if (!d) return 0;
    if (ARG(3)) { W32(ARG(3), (uint32_t)d->u.dc.curx); W32(ARG(3) + 4, (uint32_t)d->u.dc.cury); }
    d->u.dc.curx = (int)ARG(1); d->u.dc.cury = (int)ARG(2);
    return 1;
}
static uint32_t g_LineTo(void) {
    Obj *d = obj_get(ARG(0), OB_DC);
    Bitmap *D = dc_bitmap(ARG(0));
    int w = 1, pi;
    if (!d || !D) return 0;
    pi = pen_idx(&d->u.dc, &w);
    if (pi >= 0) draw_line(D, d->u.dc.curx, d->u.dc.cury, (int)ARG(1), (int)ARG(2), w, pi);
    d->u.dc.curx = (int)ARG(1); d->u.dc.cury = (int)ARG(2);
    if (d->u.dc.hwnd) user_window_drawn(d->u.dc.hwnd, "GDI32.dll!LineTo");
    return 1;
}
static uint32_t g_Polygon(void) {
    Obj *d = obj_get(ARG(0), OB_DC);
    Bitmap *D = dc_bitmap(ARG(0));
    int n = (int)ARG(2), w = 1, pi, bi, ymin, ymax;
    int px[64], py[64];
    if (!d || !D || n < 2 || n > 64) return 0;
    for (int i = 0; i < n; i++) { px[i] = (int)R32(ARG(1) + 8u * (uint32_t)i); py[i] = (int)R32(ARG(1) + 8u * (uint32_t)i + 4); }
    pi = pen_idx(&d->u.dc, &w); bi = brush_idx(&d->u.dc);
    ymin = ymax = py[0];
    for (int i = 1; i < n; i++) { if (py[i] < ymin) ymin = py[i]; if (py[i] > ymax) ymax = py[i]; }
    if (bi >= 0 && n >= 3)                                  /* even-odd fill per scanline (pixel center) */
        for (int y = ymin; y < ymax; y++) {
            int xs[64], k = 0;
            for (int i = 0; i < n; i++) {
                int j = (i + 1) % n, ya = py[i], yb = py[j];
                if (ya == yb || y < (ya < yb ? ya : yb) || y >= (ya < yb ? yb : ya)) continue;
                xs[k++] = px[i] + (int)(((long)(y - ya) * (px[j] - px[i]) + (yb - ya) / 2) / (yb - ya));
            }
            for (int a = 1; a < k; a++) { int v = xs[a], b = a - 1; while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; } xs[b + 1] = v; }
            for (int a = 0; a + 1 < k; a += 2)
                for (int x = xs[a]; x < xs[a + 1]; x++) put_px(D, x, y, bi);
        }
    if (pi >= 0)
        for (int i = 0; i < n; i++) draw_line(D, px[i], py[i], px[(i + 1) % n], py[(i + 1) % n], w, pi);
    if (d->u.dc.hwnd) user_window_drawn(d->u.dc.hwnd, "GDI32.dll!Polygon");
    return 1;
}
/* Pie(hdc, left, top, right, bottom, xr1, yr1, xr2, yr2): ellipse sector from the ray to (xr1,yr1) to the ray to (xr2,yr2), counterclockwise */
static uint32_t g_Pie(void) {
    Obj *d = obj_get(ARG(0), OB_DC);
    Bitmap *D = dc_bitmap(ARG(0));
    int l = (int)ARG(1), t = (int)ARG(2), r = (int)ARG(3), b = (int)ARG(4), w = 1, pi, bi;
    double cx = (l + r) / 2.0, cy = (t + b) / 2.0, rx = (r - l) / 2.0, ry = (b - t) / 2.0, a1, a2, sw;
    if (!d || !D || rx <= 0 || ry <= 0) return 0;
    pi = pen_idx(&d->u.dc, &w); bi = brush_idx(&d->u.dc);
    a1 = atan2(-((int)ARG(6) - cy) / ry, ((int)ARG(5) - cx) / rx);
    a2 = atan2(-((int)ARG(8) - cy) / ry, ((int)ARG(7) - cx) / rx);
    sw = a2 - a1;
    if (sw <= 0) sw += 6.283185307179586;
    if ((int)ARG(5) == (int)ARG(7) && (int)ARG(6) == (int)ARG(8)) sw = 6.283185307179586;
    if (bi >= 0)
        for (int y = t; y < b; y++)
            for (int x = l; x < r; x++) {
                double ux = (x + 0.5 - cx) / rx, uy = -(y + 0.5 - cy) / ry, a;
                if (ux * ux + uy * uy > 1.0) continue;
                a = atan2(uy, ux) - a1;
                while (a < 0) a += 6.283185307179586;
                if (a <= sw) put_px(D, x, y, bi);
            }
    if (pi >= 0) {
        int steps = (int)((rx > ry ? rx : ry) * sw) + 2, px0 = (int)lround(cx + rx * cos(a1)), py0 = (int)lround(cy - ry * sin(a1));
        draw_line(D, (int)lround(cx), (int)lround(cy), px0, py0, w, pi);
        for (int i = 1; i <= steps; i++) {
            double a = a1 + sw * i / steps;
            int x = (int)lround(cx + rx * cos(a)), y = (int)lround(cy - ry * sin(a));
            draw_line(D, px0, py0, x, y, w, pi); px0 = x; py0 = y;
        }
        draw_line(D, px0, py0, (int)lround(cx), (int)lround(cy), w, pi);
    }
    if (d->u.dc.hwnd) user_window_drawn(d->u.dc.hwnd, "GDI32.dll!Pie");
    return 1;
}

void gdi32_register(void) {
#define R(n, f) win32_register("GDI32.dll!" n, f)
    gdi_init();
    R("CreateCompatibleDC", g_CreateCompatibleDC); R("DeleteDC", g_DeleteDC);
    R("CreateCompatibleBitmap", g_CreateCompatibleBitmap); R("CreateBitmapIndirect", g_CreateBitmapIndirect);
    R("SelectObject", g_SelectObject); R("DeleteObject", g_DeleteObject); R("GetStockObject", g_GetStockObject);
    R("CreateSolidBrush", g_CreateSolidBrush); R("CreateBrushIndirect", g_CreateBrushIndirect);
    R("CreatePatternBrush", g_CreatePatternBrush); R("CreatePen", g_CreatePen); R("CreateFontA", g_CreateFontA);
    R("SetBkMode", g_SetBkMode); R("SetBkColor", g_SetBkColor); R("SetTextColor", g_SetTextColor);
    R("CreatePalette", g_CreatePalette); R("SelectPalette", g_SelectPalette); R("RealizePalette", g_RealizePalette);
    R("GetPaletteEntries", g_GetPaletteEntries); R("GetSystemPaletteEntries", g_GetSystemPaletteEntries);
    R("SetSystemPaletteUse", g_SetSystemPaletteUse); R("GetSystemPaletteUse", g_GetSystemPaletteUse);
    R("GetDeviceCaps", g_GetDeviceCaps); R("BitBlt", g_BitBlt);
    R("TextOutA", g_TextOutA); R("ExtTextOutA", g_ExtTextOutA); win32_register("USER32.dll!DrawTextA", g_DrawTextA); R("MoveToEx", g_MoveToEx); R("LineTo", g_LineTo);
    R("Polygon", g_Polygon); R("Pie", g_Pie);
    R("GetTextMetricsA", g_GetTextMetricsA); R("GetTextExtentPoint32A", g_GetTextExtentPoint32A);
#undef R
}

/* used by wing32.c */
uint32_t gdi_stock_palette(void) { return stock_obj(15); }
