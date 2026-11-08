/* GDI objects shared between gdi32.c, wing32.c and user32.c. */
#ifndef GPM2_GDI_H
#define GPM2_GDI_H

#include "win32.h"

enum { OB_NONE = 0, OB_DC = 1, OB_BITMAP, OB_PALETTE, OB_BRUSH, OB_PEN, OB_FONT, OB_ICON };

typedef struct {
    int w, h, stride, topdown;
    uint32_t bits;                  /* guest address of the pixels (8 bpp, indices) */
    uint8_t ct[256][4];             /* DIB color table (r,g,b,0), WinGSetDIBColorTable */
    int has_ct;
} Bitmap;

typedef struct {
    uint32_t hbm, hpal, hbrush, hpen, hfont;
    uint32_t textcolor, bkcolor;
    int bkmode;
    uint32_t defbm;                 /* default 1x1 bitmap of the memory DC (its own; freed with the DC) */
    uint32_t hwnd;                  /* window DC: owner HWND (0 = screen/memory) */
    int curx, cury;
} DC;

typedef struct { uint8_t e[256][4]; int n; } Palette;   /* r,g,b,flags */
typedef struct { int style; uint32_t color; uint32_t pattern; int sysidx; } Brush;
typedef struct { int style, width; uint32_t color; } Pen;
typedef struct { int height, width, weight; char face[32]; } Font;

typedef struct {
    int type;
    union { Bitmap bm; DC dc; Palette pal; Brush br; Pen pen; Font font; } u;
} Obj;

#define H_BASE 0x00001000u
uint32_t obj_new(int type);
Obj *obj_get(uint32_t h, int type);          /* 0 if invalid or of another type */
void obj_free(uint32_t h);

uint32_t gdi_new_bitmap(int w, int h, int topdown);          /* 8 bpp indexed, zeroed, in guest memory */
uint32_t gdi_new_dc(void);
Bitmap *dc_bitmap(uint32_t hdc);
uint8_t *bm_row(const Bitmap *b, int y);                     /* host pointer to logical row y (top = 0) */

/* system palette and colors */
extern uint8_t sys_pal[256][4];                              /* r,g,b,flags */
extern int sys_pal_use;                                      /* SYSPAL_STATIC=1, NOSTATIC=2 */
uint32_t gdi_color_index(uint32_t colorref);                 /* COLORREF -> index in the system palette */
uint32_t gdi_sys_color(int idx);
void gdi_init(void);

/* blit of 8 bpp surfaces (SRCCOPY and common ROPs); clips; returns 1 if anything was copied */
int gdi_blit(uint32_t hdst, int xd, int yd, int w, int h, uint32_t hsrc, int xs, int ys, uint32_t rop);
int gdi_stretch(uint32_t hdst, int xd, int yd, int wd, int hd, uint32_t hsrc, int xs, int ys, int ws, int hs, uint32_t rop);
void gdi_fill(uint32_t hdc, int x, int y, int w, int h, uint32_t brush);

void gdi_ui_rect(uint32_t hbm, int x, int y, int w, int h, int sys);
void gdi_ui_edge(uint32_t hbm, int x, int y, int w, int h, int style);
void gdi_ui_text(uint32_t hbm, int x, int y, int w, int h, const char *t, int align, int sys);
int gdi_ui_text_w(const char *t);

/* called by gdi32/wing32 after drawing on a window DC */
void user_window_drawn(uint32_t hwnd, const char *who);
/* surface (HBITMAP) of the window's client area */
uint32_t user_window_surface(uint32_t hwnd);
uint32_t user_screen_dc(void);

#endif
