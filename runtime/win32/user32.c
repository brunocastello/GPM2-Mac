/* USER32 (stdcall): windows, classes, message queue, timers, resources.
 *
 * Message sequence (modeled on Windows 95):
 *   CreateWindowExA: WM_GETMINMAXINFO, WM_NCCREATE, WM_NCCALCSIZE, WM_CREATE
 *   ShowWindow (1st time, SW_SHOW*): WM_SHOWWINDOW, WM_WINDOWPOSCHANGING, WM_ACTIVATEAPP, WM_NCACTIVATE,
 *     WM_ACTIVATE, WM_SETFOCUS, WM_QUERYNEWPALETTE, WM_WINDOWPOSCHANGED (-> WM_SIZE, WM_MOVE)
 *   afterwards: WM_PAINT leaves the queue when there is no posted message (BeginPaint sends WM_ERASEBKGND).
 * Dialogs are only registered and return a default value. Input, cursor and MessageBoxA are handled here;
 * menus live in menu.c. Top-level windows: each has a rectangle, client area, z-order and owner, and the
 * backend is notified by host_window_update; the desktop is the 640x480 virtual screen (desktop GetWindowRect).
 */
#include "gdi.h"
#include "../host/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_COMMAND 0x0111
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_GETMINMAXINFO 0x0024
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCACTIVATE 0x0086
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MBUTTONDBLCLK 0x0209
#define WM_QUERYNEWPALETTE 0x030F
#define WM_PALETTECHANGED 0x0311

#define DEFPROC 0xFFFF0000u      /* "guest" WndProc that represents the host DefWindowProc */
#define NP_BASE 0xFFFF0001u      /* native procs: NP_BASE + CK_* (controls), NP_DIALOG */
#define NP_DIALOG 0xFFFF0020u
#define MAX_WND 256
#define MAX_CLS 32
#define MAX_Q 1024
#define MAX_TIMER 64
#define WS_CHILD 0x40000000u
#define DESKTOP_HWND (HWND_BASE - 4u)        /* GetDesktopWindow: the 640x480 virtual screen */
#define SCREEN_W 640
#define SCREEN_H 480

typedef struct { char name[64]; uint32_t style, proc, clsextra, wndextra, hinst, icon, cursor, brush, menu; int used; } Class;
typedef struct {
    int used, cls, visible, enabled, invalid, erase;
    uint32_t style, exstyle, proc, parent, userdata, id, hinst, surface;
    int x, y, w, h, cw, ch, rx0, ry0, rx1, ry1;
    int z, topmost, synced;          /* top-level windows: z-order, WS_EX_TOPMOST (via SetWindowPos), host already notified */
    int sized;                       /* already got a size (move_win) or WM_SIZE from the 1st ShowWindow */
    int nohost;                      /* native dialog (dialog.c): the host shows its own panel, not a frame */
    char title[128];
    uint32_t extra[16];
} Window;
typedef struct { uint32_t hwnd, msg, wp, lp, time; int x, y; } Msg;
typedef struct { int used; uint32_t hwnd, id, ms, proc, due; int pending; } Timer;

static Class classes[MAX_CLS];
static Window wnds[MAX_WND];
static Msg queue[MAX_Q];
static unsigned qhead, qtail;
static Timer timers[MAX_TIMER];
static int quit_pending;
static uint32_t quit_code;
static uint32_t focus_hwnd, active_hwnd, capture_hwnd, next_timer_id = 1, screen_bm;
static int idle_count, last_frames;

static uint32_t hw(int i) { return HWND_BASE + 4u * (uint32_t)i; }
static Window *wnd(uint32_t h) {
    unsigned i;
    if (h < HWND_BASE || (h - HWND_BASE) % 4u) return 0;
    i = (h - HWND_BASE) / 4u;
    return i < MAX_WND && wnds[i].used ? &wnds[i] : 0;
}

static uint32_t send(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp);
static void host_sync(uint32_t h);
static void client_origin(Window *w, uint32_t h, int *ox, int *oy);
static int z_next = 16, z_bot;
static int is_top(const Window *w) { return !(w->style & WS_CHILD); }

uint32_t user_screen_dc(void) {
    uint32_t dc = gdi_new_dc();
    if (!screen_bm) screen_bm = gdi_new_bitmap(640, 480, 1);
    obj_get(dc, OB_DC)->u.dc.hbm = screen_bm;
    return dc;
}
uint32_t user_window_surface(uint32_t h) { Window *w = wnd(h); return w ? w->surface : 0; }

void user_window_drawn(uint32_t h, const char *who) {
    Window *w = wnd(h);
    Bitmap *b;
    uint8_t *tmp;
    if (!w || !w->visible || !is_top(w) || w->nohost) return;          /* children and native dialogs have no frame of their own */
    if (!w->synced) host_sync(h);
    ctl_draw_children(h, w->surface);
    b = &obj_get(w->surface, OB_BITMAP)->u.bm;
    tmp = (uint8_t *)malloc((size_t)b->w * (size_t)b->h);
    for (int y = 0; y < b->h; y++) memcpy(tmp + (size_t)y * (size_t)b->w, bm_row(b, y), (size_t)b->w);
    host_present(h, tmp, b->w, b->h, (const uint8_t (*)[4])sys_pal, who);
    free(tmp);
}

static uint32_t def_proc(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp);

static uint32_t call_proc(uint32_t proc, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (proc == DEFPROC) return def_proc(h, msg, wp, lp);
    if (proc >= NP_BASE) {                                  /* native classes: controls and dialogs */
        if (proc == NP_DIALOG) return dlg_wndproc(h, msg, wp, lp);
        return ctl_proc((int)(proc - NP_BASE), h, msg, wp, lp);
    }
    return guest_call(proc, 4, h, msg, wp, lp);
}
static uint32_t send(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    Window *w = wnd(h);
    if (!w) return 0;
    return call_proc(w->proc, h, msg, wp, lp);
}
static void post(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    unsigned n = (qtail + 1u) % MAX_Q;
    if (n == qhead) return;                                  /* queue full: discard */
    queue[qtail].hwnd = h; queue[qtail].msg = msg; queue[qtail].wp = wp; queue[qtail].lp = lp;
    queue[qtail].time = win32_now_ms(); queue[qtail].x = queue[qtail].y = 0;
    qtail = n;
}

/* main window = the first top-level one without an owner (the other top-level ones have an owner: icon bar, chat) */
uint32_t user_main_window(void) {
    for (int i = 0; i < MAX_WND; i++) if (wnds[i].used && !wnds[i].parent && is_top(&wnds[i]) && !wnds[i].nohost) return hw(i);
    return 0;
}
static void post_pt(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp, int x, int y) {
    unsigned n = (qtail + 1u) % MAX_Q;
    if (msg == WM_MOUSEMOVE && qhead != qtail) {                /* coalesces consecutive moves */
        unsigned l = (qtail + MAX_Q - 1u) % MAX_Q;
        if (queue[l].msg == WM_MOUSEMOVE && queue[l].hwnd == h) { queue[l].wp = wp; queue[l].lp = lp; queue[l].x = x; queue[l].y = y; return; }
    }
    if (n == qhead) return;
    queue[qtail].hwnd = h; queue[qtail].msg = msg; queue[qtail].wp = wp; queue[qtail].lp = lp;
    queue[qtail].time = win32_now_ms(); queue[qtail].x = x; queue[qtail].y = y;
    qtail = n;
}
void user_post(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) { post(h, msg, wp, lp); }

/* ---- DefWindowProc ------------------------------------------------------------------------ */
static void erase_bk(uint32_t h) {
    Window *w = wnd(h);
    uint32_t dc;
    if (!w) return;
    dc = gdi_new_dc();
    obj_get(dc, OB_DC)->u.dc.hbm = w->surface;
    obj_get(dc, OB_DC)->u.dc.hwnd = h;
    gdi_fill(dc, 0, 0, w->cw, w->ch, classes[w->cls].brush);
    obj_free(dc);
}

static uint32_t def_proc(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    Window *w = wnd(h);
    if (!w) return 0;
    switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_CLOSE: send(h, WM_SYSCOMMAND, 0xF060u, 0); return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0u) == 0xF060u) { post(h, WM_CLOSE, 0, 0); }
        return 0;
    case WM_ERASEBKGND: erase_bk(h); return 1;
    case WM_PAINT: w->invalid = 0; return 0;
    case WM_NCACTIVATE: return 1;
    case WM_NCHITTEST: return 1;                              /* HTCLIENT */
    case WM_SETCURSOR:
        if ((lp & 0xFFFFu) == 1u) { uint32_t c = classes[w->cls].cursor; host_set_cursor(c >= 0x50000u && c < 0x60000u ? c - 0x50000u : 0x7F00u); return 1; }
        return 0;
    case 0x000C: snprintf(w->title, sizeof w->title, "%s", ""); if (lp) gstr_get(w->title, sizeof w->title, lp); if (w->synced) host_sync(h); return 1;   /* WM_SETTEXT */
    case 0x000D: { uint32_t n = (uint32_t)strlen(w->title); gstr_putn(lp, wp, w->title); return n < wp ? n : (wp ? wp - 1 : 0); }
    case 0x000E: return (uint32_t)strlen(w->title);
    case WM_WINDOWPOSCHANGED: {
        uint32_t fl = R32(lp + 24);
        if (!(fl & 1u)) send(h, WM_SIZE, 0, (uint32_t)(w->cw & 0xFFFF) | ((uint32_t)w->ch << 16));
        if (!(fl & 2u)) send(h, WM_MOVE, 0, (uint32_t)(w->x & 0xFFFF) | ((uint32_t)w->y << 16));
        return 0;
    }
    default: return 0;
    }
}

/* ---- classes -------------------------------------------------------------------------------- */
static Class *find_class(uint32_t name) {
    char n[64];
    if (name < 0x10000u) return 0;
    gstr_get(n, sizeof n, name);
    for (int i = 0; i < MAX_CLS; i++) if (classes[i].used && gstr_cmp_ci(classes[i].name, n) == 0) return &classes[i];
    return 0;
}
static uint32_t u_RegisterClassA(void) {
    uint32_t p = ARG(0);
    int i;
    char n[64];
    gstr_get(n, sizeof n, R32(p + 36));
    for (i = 0; i < MAX_CLS; i++) if (!classes[i].used) break;
    if (i == MAX_CLS) return 0;
    snprintf(classes[i].name, sizeof classes[i].name, "%s", n);
    classes[i].style = R32(p); classes[i].proc = R32(p + 4); classes[i].clsextra = R32(p + 8);
    classes[i].wndextra = R32(p + 12); classes[i].hinst = R32(p + 16); classes[i].icon = R32(p + 20);
    classes[i].cursor = R32(p + 24); classes[i].brush = R32(p + 28); classes[i].menu = R32(p + 32);
    classes[i].used = 1;
    return 0xC000u + (uint32_t)i;
}
static uint32_t u_UnregisterClassA(void) {
    Class *c = find_class(ARG(0));
    if (!c) return 0;
    c->used = 0;
    return 1;
}
static uint32_t u_GetClassInfoA(void) {
    Class *c = find_class(ARG(1));
    uint32_t p = ARG(2);
    if (!c) return 0;
    W32(p, c->style); W32(p + 4, c->proc); W32(p + 8, c->clsextra); W32(p + 12, c->wndextra);
    W32(p + 16, c->hinst); W32(p + 20, c->icon); W32(p + 24, c->cursor); W32(p + 28, c->brush);
    W32(p + 32, c->menu); W32(p + 36, ARG(1));
    return 1;
}
static uint32_t u_GetClassLongA(void) {
    Window *w = wnd(ARG(0));
    int idx = (int)ARG(1);
    if (!w) return 0;
    switch (idx) {
    case -10: return classes[w->cls].brush;
    case -12: return classes[w->cls].cursor;
    case -14: return classes[w->cls].icon;
    case -26: return classes[w->cls].style;
    case -24: return classes[w->cls].proc;
    case -16: return classes[w->cls].hinst;
    default: return 0;
    }
}
static uint32_t u_SetClassLongA(void) {
    Window *w = wnd(ARG(0));
    int idx = (int)ARG(1);
    uint32_t old = 0;
    if (!w) return 0;
    switch (idx) {
    case -10: old = classes[w->cls].brush; classes[w->cls].brush = ARG(2); break;
    case -12: old = classes[w->cls].cursor; classes[w->cls].cursor = ARG(2); break;
    case -14: old = classes[w->cls].icon; classes[w->cls].icon = ARG(2); break;
    default: break;
    }
    return old;
}

/* ---- windows ----------------------------------------------------------------------------------- */
/* A top-level window (without WS_POPUP/WS_CHILD) that is WS_OVERLAPPED (no border/caption bits) has a caption
 * (SM_CYCAPTION = 19). For the game's style 0xA0000 (640 x 2*SM_CYBORDER + SM_CYCAPTION + 0x1CA) the border
 * is subtracted only from the caption: the client area is 640x460, the size of the WinGBitBlt (real Win95
 * would crop 2 columns/rows). A Win32 menu bar (hMenu) subtracts SM_CYMENU = 19; the game
 * does not use one (see menu.c). */
static void nc_sizes(uint32_t style, int *bw, int *cap) {
    *bw = (style & 0x40000u) ? 4 : (style & 0x400000u) ? 3 : (style & 0x800000u) ? 1 : 0;
    *cap = (style & 0xC00000u) == 0xC00000u ? 19 : 0;
    if (*cap && *bw == 0) *bw = 1;
    if (!(style & 0xC0000000u) && (style & 0xC00000u) == 0) *cap = 19;
}
static void set_client(Window *w, uint32_t h) {
    int bw, cap;
    nc_sizes(w->style, &bw, &cap);
    w->cw = w->w - 2 * bw; w->ch = w->h - 2 * bw - cap - (menu_has_bar(h) ? 19 : 0);
    if (w->cw < 0) w->cw = 0;
    if (w->ch < 0) w->ch = 0;
}
/* origin of the client area in screen coordinates (child: relative to the parent's client) */
static void client_origin(Window *w, uint32_t h, int *ox, int *oy) {
    int bw, cap, px = 0, py = 0;
    nc_sizes(w->style, &bw, &cap);
    if (!is_top(w) && wnd(w->parent)) client_origin(wnd(w->parent), w->parent, &px, &py);
    *ox = px + w->x + bw; *oy = py + w->y + bw + cap + (menu_has_bar(h) ? 19 : 0);
}
/* notifies the backend of the top-level window state (rectangle, client, visibility, z, title) */
static void host_sync(uint32_t h) {
    Window *w = wnd(h);
    HostWindow hwn;
    int bw, cap, ox, oy;
    if (!w || !is_top(w) || w->nohost) return;
    nc_sizes(w->style, &bw, &cap);
    client_origin(w, h, &ox, &oy);
    memset(&hwn, 0, sizeof hwn);
    hwn.id = h; hwn.owner = w->parent;
    hwn.x = w->x; hwn.y = w->y; hwn.w = w->w; hwn.h = w->h;
    hwn.cx = ox - w->x; hwn.cy = oy - w->y; hwn.cw = w->cw; hwn.ch = w->ch;
    hwn.bw = bw; hwn.cap = cap;
    hwn.visible = w->visible; hwn.topmost = w->topmost; hwn.z = w->z;
    hwn.main = h == user_main_window();
    hwn.caption = cap > 0; hwn.sysmenu = (w->style & 0x80000u) != 0;
    snprintf(hwn.title, sizeof hwn.title, "%s", w->title);
    w->synced = 1;
    host_window_update(&hwn);
}
static void realloc_surface(Window *w) {
    uint32_t nb = gdi_new_bitmap(w->cw ? w->cw : 1, w->ch ? w->ch : 1, 1);
    if (w->surface) {
        Bitmap *o = &obj_get(w->surface, OB_BITMAP)->u.bm, *n = &obj_get(nb, OB_BITMAP)->u.bm;
        int cw = o->w < n->w ? o->w : n->w, ch = o->h < n->h ? o->h : n->h;
        for (int y = 0; y < ch; y++) memcpy(bm_row(n, y), bm_row(o, y), (size_t)cw);
        obj_free(w->surface);
    }
    w->surface = nb;
}
static void invalidate(Window *w, int x0, int y0, int x1, int y1, int erase) {
    if (!w->invalid) { w->rx0 = x0; w->ry0 = y0; w->rx1 = x1; w->ry1 = y1; }
    else {
        if (x0 < w->rx0) w->rx0 = x0;
        if (y0 < w->ry0) w->ry0 = y0;
        if (x1 > w->rx1) w->rx1 = x1;
        if (y1 > w->ry1) w->ry1 = y1;
    }
    w->invalid = 1;
    if (erase) w->erase = 1;
}

/* Creates a window. `clsname`/`title` are guest pointers (0 = use `htitle`, host text, and `hcls`). The
 * CreateWindowExA arguments come from `a` (12 words). Used by the import and by user_create (dialog.c). */
static uint32_t create_window(const char *hcls, const uint32_t *a, const char *htitle) {
    Class *c;
    int i, ci;
    Window *w;
    uint32_t cs, h, st = a[3];
    if (hcls) {
        for (i = 0; i < MAX_CLS; i++) if (classes[i].used && gstr_cmp_ci(classes[i].name, hcls) == 0) break;
        c = i < MAX_CLS ? &classes[i] : 0;
    } else {
        if (a[1] < 0x10000u) win32_die("CreateWindowExA with atom %08x", a[1]);
        c = find_class(a[1]);
    }
    if (!c) { { char n[64]; if (hcls) snprintf(n, sizeof n, "%s", hcls); else gstr_get(n, sizeof n, a[1]); win32_log("CreateWindowExA: class '%s' not registered", n); } return 0; }
    ci = (int)(c - classes);
    for (i = 0; i < MAX_WND; i++) if (!wnds[i].used) break;
    if (i == MAX_WND) return 0;
    w = &wnds[i];
    memset(w, 0, sizeof *w);
    w->used = 1; w->cls = ci; w->style = st; w->exstyle = a[0]; w->proc = c->proc;
    w->z = (z_next += 16);
    w->parent = a[8]; w->hinst = a[10]; w->id = a[9]; w->enabled = 1;
    w->nohost = c->proc == NP_DIALOG || (!a[8] && !strcmp(c->name, "ChatClass"));   /* the chat becomes a native panel: dlg_adopt */
    w->x = (int)a[4] == (int)0x80000000 ? 0 : (int)a[4];
    w->y = (int)a[5] == (int)0x80000000 ? 0 : (int)a[5];
    w->w = (int)a[6] == (int)0x80000000 ? 640 : (int)a[6];
    w->h = (int)a[7] == (int)0x80000000 ? 480 : (int)a[7];
    if (htitle) snprintf(w->title, sizeof w->title, "%s", htitle);
    else if (a[2]) gstr_get(w->title, sizeof w->title, a[2]);
    h = hw(i);
    if (!(st & 0x40000000u)) menu_window_created(h, a[9]);
    set_client(w, h);
    realloc_surface(w);
    /* CREATESTRUCT (48 bytes) on the guest stack, below the current esp */
    cs = g.esp - 0x100u;
    W32(cs, a[11]); W32(cs + 4, a[10]); W32(cs + 8, a[9]); W32(cs + 12, a[8]);
    W32(cs + 16, (uint32_t)w->h); W32(cs + 20, (uint32_t)w->w); W32(cs + 24, (uint32_t)w->y); W32(cs + 28, (uint32_t)w->x);
    W32(cs + 32, st); W32(cs + 36, a[2]); W32(cs + 40, a[1]); W32(cs + 44, a[0]);
    { uint32_t mm = cs - 0x40u;          /* MINMAXINFO */
      for (uint32_t k = 0; k < 40; k += 4) W32(mm + k, 0);
      W32(mm + 8, 640); W32(mm + 12, 480); W32(mm + 24, 640); W32(mm + 28, 480);
      { uint32_t sp = g.esp; g.esp = mm - 0x10u; send(h, WM_GETMINMAXINFO, 0, mm); g.esp = sp; } }
    { uint32_t sp = g.esp; g.esp = cs - 0x80u;
      if (send(h, WM_NCCREATE, 0, cs) == 0) { w->used = 0; g.esp = sp; return 0; }
      send(h, WM_NCCALCSIZE, 0, cs);
      if (send(h, WM_CREATE, 0, cs) == 0xFFFFFFFFu) { w->used = 0; g.esp = sp; return 0; }
      g.esp = sp; }
    if (st & 0x10000000u) { /* WS_VISIBLE */ w->visible = 1; invalidate(w, 0, 0, w->cw, w->ch, 1); host_sync(h); }
    return h;
}
static uint32_t u_CreateWindowExA(void) {
    uint32_t a[12];
    for (int i = 0; i < 12; i++) a[i] = ARG(i);
    return create_window(0, a, 0);
}
/* internal creation (dialog.c): no guest pointers */
uint32_t user_create(const char *cls, const char *title, uint32_t style, uint32_t exstyle, int x, int y, int w, int h,
                     uint32_t parent, uint32_t id, uint32_t hinst) {
    uint32_t a[12] = {exstyle, 0, 0, style, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, parent, id, hinst, 0};
    return create_window(cls, a, title ? title : "");
}

static uint32_t u_ShowWindow(void) {
    Window *w = wnd(ARG(0));
    uint32_t h = ARG(0), cmd = ARG(1), wasvis;
    uint32_t wp;
    if (!w) return 0;
    wasvis = (uint32_t)w->visible;
    if (cmd != 0 && w->cls >= 0 && w->cls < MAX_CLS && !strcmp(classes[w->cls].name, "CMBClass") && !w->parent) {
        /* the game's "message box" (CMBClass window: text in DAT_0057b3d4 of 1.02B, OK button, non-modal): becomes a native alert of the
         * host; then sends WM_COMMAND, which the game's wndproc handles with DestroyWindow (the logical lifecycle is preserved) */
        char txt[256];
        unsigned n = 0;
        while (n < 255 && M[0x0057B3D4u + n]) { txt[n] = (char)M[0x0057B3D4u + n]; n++; }
        txt[n] = 0;
        host_message_box(w->title, txt, strncmp(w->title, "About", 5) == 0 ? 0x40000000u : 0x40u);
        send(h, WM_COMMAND, 1, 0);
        return 0;
    }
    ctl_touch(h);
    if (cmd == 0) { w->visible = 0; send(h, WM_SHOWWINDOW, 0, 0); if (w->synced) host_sync(h); return wasvis; }
    w->visible = 1;
    ctl_touch(h);
    if (!wasvis && is_top(w)) w->z = (z_next += 16);          /* the shown window goes on top */
    wp = g.esp - 0x100u;
    for (uint32_t k = 0; k < 28; k += 4) W32(wp + k, 0);
    W32(wp, h); W32(wp + 8, (uint32_t)w->x); W32(wp + 12, (uint32_t)w->y); W32(wp + 16, (uint32_t)w->w); W32(wp + 20, (uint32_t)w->h);
    W32(wp + 24, 0x0043u);                                    /* SWP_NOSIZE|SWP_NOMOVE|SWP_SHOWWINDOW */
    { uint32_t sp = g.esp; g.esp = wp - 0x20u;
      if (!wasvis) {
          send(h, WM_SHOWWINDOW, 1, 0);
          send(h, WM_WINDOWPOSCHANGING, 0, wp);
          if (!active_hwnd && !w->parent) {
              active_hwnd = h;
              send(h, WM_ACTIVATEAPP, 1, 1);
              send(h, WM_NCACTIVATE, 1, 0);
              send(h, WM_ACTIVATE, 1, 0);
              if (!focus_hwnd) { focus_hwnd = h; send(h, WM_SETFOCUS, 0, 0); }
              send(h, WM_QUERYNEWPALETTE, 0, 0);
          }
          send(h, WM_WINDOWPOSCHANGED, 0, wp);
          if (!w->sized) {                                    /* as in Windows: the 1st ShowWindow delivers WM_SIZE/WM_MOVE (the chat window positions its children here) */
              w->sized = 1;
              send(h, WM_SIZE, 0, (uint32_t)(w->cw & 0xFFFF) | ((uint32_t)w->ch << 16));
              send(h, WM_MOVE, 0, (uint32_t)(w->x & 0xFFFF) | ((uint32_t)w->y << 16));
          }
          invalidate(w, 0, 0, w->cw, w->ch, 1);
          host_sync(h);
          if (w->nohost && w->proc != NP_DIALOG) dlg_adopt(h, w->title, w->cw, w->ch);
      }
      g.esp = sp; }
    return wasvis;
}
static uint32_t destroy_window(uint32_t h) {
    Window *w = wnd(h);
    if (!w) return 0;
    dlg_window_destroying(h);
    for (int i = 0; i < MAX_WND; i++)                         /* children first (as in Win32) */
        if (wnds[i].used && wnds[i].parent == h && !is_top(&wnds[i])) destroy_window(hw(i));
    send(h, WM_DESTROY, 0, 0);
    send(h, WM_NCDESTROY, 0, 0);
    if (focus_hwnd == h) focus_hwnd = 0;
    if (active_hwnd == h) active_hwnd = wnd(w->parent) && is_top(w) ? w->parent : 0;     /* activation returns to the owner */
    if (capture_hwnd == h) capture_hwnd = 0;
    menu_window_destroyed(h);
    if (w->synced) host_window_destroy(h);
    w->used = 0;
    dlg_window_destroyed(h);
    return 1;
}
static uint32_t u_DestroyWindow(void) { return destroy_window(ARG(0)); }
static uint32_t u_SetFocus(void) {
    uint32_t old = focus_hwnd, h = ARG(0);
    if (!wnd(h) || h == old) return old;
    if (old) send(old, WM_KILLFOCUS, h, 0);
    focus_hwnd = h;
    send(h, WM_SETFOCUS, old, 0);
    return old;
}
static uint32_t u_SetActiveWindow(void) { uint32_t old = active_hwnd; if (wnd(ARG(0))) active_hwnd = ARG(0); return old; }
static uint32_t u_GetClientRect(void) {
    Window *w = wnd(ARG(0));
    uint32_t r = ARG(1);
    W32(r, 0); W32(r + 4, 0);
    W32(r + 8, w ? (uint32_t)w->cw : (uint32_t)SCREEN_W); W32(r + 12, w ? (uint32_t)w->ch : (uint32_t)SCREEN_H);
    return w != 0 || ARG(0) == DESKTOP_HWND;
}
static uint32_t u_GetWindowRect(void) {
    Window *w = wnd(ARG(0));
    uint32_t r = ARG(1);
    if (ARG(0) == DESKTOP_HWND) {        /* the virtual screen; the game centers the main window by it (WM_CREATE) */
        W32(r, 0); W32(r + 4, 0); W32(r + 8, (uint32_t)SCREEN_W); W32(r + 12, (uint32_t)SCREEN_H);
        return 1;
    }
    if (!w) return 0;
    W32(r, (uint32_t)w->x); W32(r + 4, (uint32_t)w->y); W32(r + 8, (uint32_t)(w->x + w->w)); W32(r + 12, (uint32_t)(w->y + w->h));
    return 1;
}
static void move_win(Window *w, uint32_t h, int x, int y, int cw, int ch, int size, int move) {
    int os = (w->w != cw || w->h != ch) && size;
    if (size) w->sized = 1;
    if (move) { w->x = x; w->y = y; }
    if (size) { w->w = cw; w->h = ch; set_client(w, h); if (os) { realloc_surface(w); invalidate(w, 0, 0, w->cw, w->ch, 1); } }
    if (os) send(h, WM_SIZE, 0, (uint32_t)(w->cw & 0xFFFF) | ((uint32_t)w->ch << 16));
    if (move) send(h, WM_MOVE, 0, (uint32_t)(w->x & 0xFFFF) | ((uint32_t)w->y << 16));
    if ((move || size) && w->synced) host_sync(h);
}
static uint32_t u_MoveWindow(void) {
    Window *w = wnd(ARG(0));
    if (!w) return 0;
    move_win(w, ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), 1, 1);
    return 1;
}
static uint32_t u_SetWindowPos(void) {
    uint32_t h = ARG(0), ia = ARG(1), fl = ARG(6);
    int x = (int)ARG(2), y = (int)ARG(3), cx = (int)ARG(4), cy = (int)ARG(5);
    Window *w = wnd(h), *o;
    if (!w) return 0;
    if (!(fl & 2u)) dlg_chat_center(h);                      /* the chat is a native panel: the game only moves it to center it (FUN_00503930) */
    move_win(w, h, x, y, cx, cy, !(fl & 1u), !(fl & 2u));
    if (is_top(w) && !(fl & 4u)) {                           /* z-order: HWND_TOPMOST/NOTOPMOST/TOP/BOTTOM or after another */
        o = wnd(ia);
        if (ia == 0xFFFFFFFFu) { w->topmost = 1; w->z = (z_next += 16); }
        else if (ia == 0xFFFFFFFEu) { w->topmost = 0; w->z = (z_next += 16); }
        else if (ia == 0) w->z = (z_next += 16);
        else if (ia == 1) w->z = (z_bot -= 16);
        else if (o) { w->z = o->z - 1; w->topmost = o->topmost; }
    }
    if (fl & 0x40u) w->visible = 1;
    if (fl & 0x80u) w->visible = 0;
    if (fl & 0xC0u) ctl_touch(h);
    if (w->visible || w->synced) host_sync(h);
    return 1;
}
static uint32_t u_GetWindowLongA(void) {
    Window *w = wnd(ARG(0));
    int idx = (int)ARG(1);
    if (!w) return 0;
    switch (idx) {
    case -4: return w->proc == DEFPROC ? DEFPROC : w->proc;
    case -16: return w->style; case -20: return w->exstyle; case -21: return w->userdata;
    case -12: return w->id; case -6: return w->hinst; case -8: return w->parent;
    default: return idx >= 0 && idx < 64 ? w->extra[idx / 4] : 0;
    }
}
static uint32_t u_SetWindowLongA(void) {
    Window *w = wnd(ARG(0));
    int idx = (int)ARG(1);
    uint32_t old = 0, v = ARG(2);
    if (!w) return 0;
    switch (idx) {
    case -4: old = w->proc; w->proc = v; break;
    case -16: old = w->style; w->style = v; break;
    case -20: old = w->exstyle; w->exstyle = v; break;
    case -21: old = w->userdata; w->userdata = v; break;
    case -12: old = w->id; w->id = v; break;
    default: if (idx >= 0 && idx < 64) { old = w->extra[idx / 4]; w->extra[idx / 4] = v; }
    }
    return old;
}
static uint32_t u_IsWindowVisible(void) { Window *w = wnd(ARG(0)); return w && w->visible; }
static uint32_t u_IsIconic(void) { return 0; }
static uint32_t u_IsWindowEnabled(void) { Window *w = wnd(ARG(0)); return w && w->enabled; }
static uint32_t u_EnableWindow(void) {
    Window *w = wnd(ARG(0));
    uint32_t old;
    if (!w) return 0;
    old = !w->enabled;
    if (w->enabled != ((int)ARG(1) != 0)) {
        w->enabled = (int)ARG(1) != 0;
        send(ARG(0), 0x000Au, (uint32_t)w->enabled, 0);       /* WM_ENABLE */
        ctl_touch(ARG(0));
    }
    return old;
}
static uint32_t u_SetWindowTextA(void) {
    if (!wnd(ARG(0))) return 0;
    send(ARG(0), 0x000Cu, 0, ARG(1));                            /* WM_SETTEXT: controls and windows handle it differently */
    return 1;
}
static uint32_t u_GetWindowTextA(void) {
    if (!wnd(ARG(0)) || ARG(2) == 0) return 0;
    return send(ARG(0), 0x000Du, ARG(2), ARG(1));                /* WM_GETTEXT */
}
static uint32_t u_BringWindowToTop(void) {
    Window *w = wnd(ARG(0));
    if (dlg_bring_front(ARG(0))) return 1;
    if (w && is_top(w)) { w->z = (z_next += 16); if (w->synced) host_sync(ARG(0)); }
    return 1;
}
static uint32_t u_GetDesktopWindow(void) { return DESKTOP_HWND; }

/* ---- DC and painting -------------------------------------------------------------------------- */
static uint32_t u_GetDC(void) {
    uint32_t h = ARG(0), dc;
    Window *w = wnd(h);
    if (h == 0 || h == DESKTOP_HWND) return user_screen_dc();
    if (!w) return 0;
    dc = gdi_new_dc();
    obj_get(dc, OB_DC)->u.dc.hbm = w->surface;
    obj_get(dc, OB_DC)->u.dc.hwnd = h;
    return dc;
}
static uint32_t u_ReleaseDC(void) {
    if (obj_get(ARG(1), OB_DC)) obj_free(ARG(1));
    return 1;
}
static uint32_t u_BeginPaint(void) {
    uint32_t h = ARG(0), ps = ARG(1), dc;
    Window *w = wnd(h);
    uint32_t erased = 1;
    if (!w) return 0;
    dc = gdi_new_dc();
    obj_get(dc, OB_DC)->u.dc.hbm = w->surface;
    obj_get(dc, OB_DC)->u.dc.hwnd = h;
    if (w->erase) { w->erase = 0; erased = send(h, WM_ERASEBKGND, dc, 0) != 0; }
    for (uint32_t k = 0; k < 64; k += 4) W32(ps + k, 0);
    W32(ps, dc); W32(ps + 4, erased ? 0u : 1u);
    W32(ps + 8, (uint32_t)w->rx0); W32(ps + 12, (uint32_t)w->ry0); W32(ps + 16, (uint32_t)w->rx1); W32(ps + 20, (uint32_t)w->ry1);
    w->invalid = 0;
    return dc;
}
static uint32_t u_EndPaint(void) {
    uint32_t dc = R32(ARG(1));
    { Obj *d = obj_get(dc, OB_DC); if (d && d->u.dc.hwnd) user_window_drawn(d->u.dc.hwnd, "USER32.dll!EndPaint"); }
    if (obj_get(dc, OB_DC)) obj_free(dc);
    return 1;
}
static uint32_t u_InvalidateRect(void) {
    Window *w = wnd(ARG(0));
    if (!w) return 0;
    if (ARG(1)) invalidate(w, (int)R32(ARG(1)), (int)R32(ARG(1) + 4), (int)R32(ARG(1) + 8), (int)R32(ARG(1) + 12), (int)ARG(2));
    else invalidate(w, 0, 0, w->cw, w->ch, (int)ARG(2));
    return 1;
}
static uint32_t u_RedrawWindow(void) {
    Window *w = wnd(ARG(0));
    if (w) invalidate(w, 0, 0, w->cw, w->ch, 1);
    return w != 0;
}
static uint32_t u_FillRect(void) {
    uint32_t r = ARG(1);
    int x0 = (int)R32(r), y0 = (int)R32(r + 4), x1 = (int)R32(r + 8), y1 = (int)R32(r + 12);
    Obj *d = obj_get(ARG(0), OB_DC);
    gdi_fill(ARG(0), x0, y0, x1 - x0, y1 - y0, ARG(2));
    if (d && d->u.dc.hwnd) user_window_drawn(d->u.dc.hwnd, "USER32.dll!FillRect");
    return 1;
}

/* ---- input --------------------------------------------------------------------- */
#define MK_LBUTTON 0x0001u
#define MK_RBUTTON 0x0002u
#define MK_SHIFT 0x0004u
#define MK_CONTROL 0x0008u
#define MK_MBUTTON 0x0010u
#define CS_DBLCLKS 0x0008u
#define DBLCLK_MS 500u
#define DBLCLK_PX 4

static uint8_t keys[256];                       /* bit 7 = pressed, bit 0 = toggle (Caps Lock...) */
static unsigned btn_mk;                         /* MK_LBUTTON | MK_RBUTTON | MK_MBUTTON */
static int mouse_x, mouse_y;                    /* last mouse position (virtual screen) */
static struct { uint32_t t; int x, y, valid; } lastdown[3];

static uint32_t pack_xy(int x, int y) { return (uint32_t)(x & 0xFFFF) | ((uint32_t)(y & 0xFFFF) << 16); }
static uint32_t mods_mk(unsigned mods) { return ((mods & HOST_MOD_SHIFT) ? MK_SHIFT : 0u) | ((mods & HOST_MOD_CTRL) ? MK_CONTROL : 0u); }

static uint32_t scan_of(uint32_t vk) {
    static const char row1[] = "QWERTYUIOP", row2[] = "ASDFGHJKL", row3[] = "ZXCVBNM";
    const char *q;
    if (vk >= 'A' && vk <= 'Z') {
        if ((q = strchr(row1, (int)vk))) return 0x10u + (uint32_t)(q - row1);
        if ((q = strchr(row2, (int)vk))) return 0x1Eu + (uint32_t)(q - row2);
        if ((q = strchr(row3, (int)vk))) return 0x2Cu + (uint32_t)(q - row3);
    }
    if (vk >= '1' && vk <= '9') return 2u + (vk - '1');
    if (vk == '0') return 0x0Bu;
    if (vk >= 0x70 && vk <= 0x79) return 0x3Bu + (vk - 0x70);
    switch (vk) {
    case 0x1B: return 0x01; case 0x08: return 0x0E; case 0x09: return 0x0F; case 0x0D: return 0x1C; case 0x20: return 0x39;
    case 0x7A: return 0x57; case 0x7B: return 0x58;
    case 0x26: return 0x48; case 0x25: return 0x4B; case 0x27: return 0x4D; case 0x28: return 0x50;
    case 0x24: return 0x47; case 0x23: return 0x4F; case 0x21: return 0x49; case 0x22: return 0x51;
    case 0x2D: return 0x52; case 0x2E: return 0x53; case 0x10: return 0x2A; case 0x11: return 0x1D; case 0x12: return 0x38;
    default: return 0;
    }
}
static int is_ext_key(uint32_t vk) { return (vk >= 0x21 && vk <= 0x28) || vk == 0x2D || vk == 0x2E; }

static uint32_t focus_target(void) {
    uint32_t t = focus_hwnd;
    if (!t || !wnd(t) || wnd(t)->proc == DEFPROC) t = user_main_window();
    return t;
}
static void mouse_event(uint32_t tgt, int btn, int down, int x, int y, unsigned mods) {
    static const uint32_t dn[3] = {WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN};
    static const uint32_t mk[3] = {MK_LBUTTON, MK_RBUTTON, MK_MBUTTON};
    uint32_t msg = dn[btn] + (down ? 0u : 1u);
    Window *w = wnd(tgt);
    if (down) {
        btn_mk |= mk[btn];
        if (w && (classes[w->cls].style & CS_DBLCLKS)) {
            uint32_t now = win32_now_ms();
            int dx = x - lastdown[btn].x, dy = y - lastdown[btn].y;
            if (lastdown[btn].valid && now - lastdown[btn].t <= DBLCLK_MS && dx >= -DBLCLK_PX && dx <= DBLCLK_PX &&
                dy >= -DBLCLK_PX && dy <= DBLCLK_PX) { msg = dn[btn] + 2u; lastdown[btn].valid = 0; }
            else { lastdown[btn].valid = 1; lastdown[btn].t = now; lastdown[btn].x = x; lastdown[btn].y = y; }
        }
    } else btn_mk &= ~mk[btn];
    post_pt(tgt, msg, btn_mk | mods_mk(mods), pack_xy(x, y), x, y);
}
static void key_event(const HostEvent *e) {
    uint32_t vk = (uint32_t)e->a & 0xFFu, tgt = focus_target(), msg, lp;
    if (wnd(tgt) && !wnd(tgt)->enabled) return;
    int sys = (e->mods & HOST_MOD_ALT) != 0 && vk != 0x11;
    int prev = (keys[vk] & 0x80) != 0;
    if (e->b) {
        keys[vk] |= 0x80;
        if (!prev && (vk == 0x14 || vk == 0x90 || vk == 0x91)) keys[vk] ^= 1;
    } else keys[vk] &= (uint8_t)~0x80;
    msg = e->b ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
    lp = 1u | (scan_of(vk) << 16) | (is_ext_key(vk) ? 1u << 24 : 0u) | (sys ? 1u << 29 : 0u) |
         ((e->b ? (e->c || prev) : 1) ? 1u << 30 : 0u) | (e->b ? 0u : 1u << 31);
    post_pt(tgt, msg, vk, lp, e->b ? e->ch : 0, 0);        /* pt.x carries the character to TranslateMessage */
}
/* (direct) child control of `top` at screen position (sx, sy); 0 if none. Only children of native controls. */
static uint32_t child_at(uint32_t top, int sx, int sy) {
    for (int i = MAX_WND - 1; i >= 0; i--) {
        Window *c = &wnds[i];
        int ox, oy;
        if (!c->used || c->parent != top || is_top(c) || !c->visible || ctl_kind(hw(i)) < 0) continue;
        client_origin(c, hw(i), &ox, &oy);
        ox -= 0; oy -= 0;
        if (sx >= ox && sy >= oy && sx < ox + c->w && sy < oy + c->h) return hw(i);
    }
    return 0;
}
static void pump_host(void) {
    HostEvent e;
    while (win32_input_pop(&e)) {
        uint32_t mw = user_main_window(), src, tgt;
        Window *ws, *wt;
        int ox = 0, oy = 0, sx, sy, cx, cy;
        if (!mw) continue;
        src = e.win && wnd(e.win) ? e.win : mw;
        tgt = capture_hwnd && wnd(capture_hwnd) ? capture_hwnd : src;
        switch (e.type) {
        case HOST_EV_MOUSEMOVE:
        case HOST_EV_BUTTON:
            if (e.type == HOST_EV_BUTTON && (e.a < 0 || e.a > 2)) break;
            ws = wnd(src); wt = wnd(tgt);
            if (!wt->enabled) break;                              /* disabled window (owner of a modal dialog) */
            client_origin(ws, src, &ox, &oy);
            sx = ox + e.x; sy = oy + e.y;                         /* virtual screen */
            if (!(capture_hwnd && wnd(capture_hwnd))) {               /* the child (control) under the cursor receives the mouse */
                uint32_t ch = child_at(tgt, sx, sy);
                if (ch) { tgt = ch; wt = wnd(tgt); if (!wt->enabled) break; }
            }
            client_origin(wt, tgt, &ox, &oy);
            cx = sx - ox; cy = sy - oy;                           /* client of the target window */
            mouse_x = sx; mouse_y = sy;
            if (e.type == HOST_EV_MOUSEMOVE) post_pt(tgt, WM_MOUSEMOVE, btn_mk | mods_mk(e.mods), pack_xy(cx, cy), cx, cy);
            else mouse_event(tgt, e.a, e.b, cx, cy, e.mods);
            break;
        case HOST_EV_KEY: key_event(&e); break;
        case HOST_EV_DLG: dlg_host_event(&e); break;
        case HOST_EV_CHAT: dlg_chat_command(e.a); break;
        case HOST_EV_MENU:
            if (e.b) post(mw, WM_SYSCOMMAND, (uint32_t)e.a, 0);
            else post(mw, WM_COMMAND, (uint32_t)e.a & 0xFFFFu, 0);
            break;
        case HOST_EV_CLOSE: if (wnd(src)->enabled) post(src, WM_CLOSE, 0, 0); break;
        default: break;
        }
    }
}
static uint32_t keystate(uint32_t vk, int async) {
    uint32_t r;
    vk &= 0xFFu;
    if (vk == 1) r = (btn_mk & MK_LBUTTON) ? 0x8000u : 0u;
    else if (vk == 2) r = (btn_mk & MK_RBUTTON) ? 0x8000u : 0u;
    else if (vk == 4) r = (btn_mk & MK_MBUTTON) ? 0x8000u : 0u;
    else r = (keys[vk] & 0x80) ? 0x8000u : 0u;
    if (!async && (keys[vk] & 1)) r |= 1u;
    return (r & 0x8000u) ? (r | 0xFFFF0000u) : r;                 /* signed SHORT in eax */
}
static uint32_t u_GetKeyState(void) { return keystate(ARG(0), 0); }
static uint32_t u_GetAsyncKeyState(void) { return keystate(ARG(0), 1); }
static uint32_t u_GetCursorPos(void) {
    uint32_t p = ARG(0);
    W32(p, (uint32_t)mouse_x); W32(p + 4, (uint32_t)mouse_y);
    return 1;
}
static uint32_t u_ClientToScreen(void) {
    Window *w = wnd(ARG(0));
    int ox, oy;
    uint32_t p = ARG(1);
    if (!w) return 0;
    client_origin(w, ARG(0), &ox, &oy);
    W32(p, (uint32_t)((int)R32(p) + ox)); W32(p + 4, (uint32_t)((int)R32(p + 4) + oy));
    return 1;
}
static uint32_t u_ScreenToClient(void) {
    Window *w = wnd(ARG(0));
    int ox, oy;
    uint32_t p = ARG(1);
    if (!w) return 0;
    client_origin(w, ARG(0), &ox, &oy);
    W32(p, (uint32_t)((int)R32(p) - ox)); W32(p + 4, (uint32_t)((int)R32(p + 4) - oy));
    return 1;
}
static uint32_t u_GetFocus(void) { return focus_hwnd; }
static uint32_t u_GetCapture(void) { return capture_hwnd; }
static uint32_t u_GetActiveWindow(void) { return active_hwnd; }

/* ---- messages ---------------------------------------------------------------------------------- */
static uint32_t u_SendMessageA(void) {
    uint32_t r;
    if (dlg_chat_capture(ARG(0), ARG(1), ARG(3), &r)) return r;
    return send(ARG(0), ARG(1), ARG(2), ARG(3));
}
static uint32_t u_PostMessageA(void) { if (ARG(0) && !wnd(ARG(0))) return 0; post(ARG(0), ARG(1), ARG(2), ARG(3)); return 1; }
static uint32_t u_PostQuitMessage(void) { quit_pending = 1; quit_code = ARG(0); return 0; }
static uint32_t u_DefWindowProcA(void) { return def_proc(ARG(0), ARG(1), ARG(2), ARG(3)); }
static uint32_t u_CallWindowProcA(void) { return call_proc(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)); }

static void fill_msg(uint32_t p, const Msg *m) {
    W32(p, m->hwnd); W32(p + 4, m->msg); W32(p + 8, m->wp); W32(p + 12, m->lp);
    W32(p + 16, m->time); W32(p + 20, (uint32_t)m->x); W32(p + 24, (uint32_t)m->y);
}
static int msg_match(const Msg *m, uint32_t hwnd, uint32_t lo, uint32_t hi) {
    if (hwnd && m->hwnd != hwnd) return 0;
    if ((lo || hi) && (m->msg < lo || m->msg > hi)) return 0;
    return 1;
}
static void fire_timers(void) {
    uint32_t now = win32_now_ms();
    for (int i = 0; i < MAX_TIMER; i++)
        if (timers[i].used && !timers[i].pending && (int32_t)(now - timers[i].due) >= 0) {
            timers[i].pending = 1;
            post(timers[i].hwnd, WM_TIMER, timers[i].id, timers[i].proc);
            timers[i].due = now + (timers[i].ms ? timers[i].ms : 1u);
        }
}
/* one message for the filter, in the order posted > timers > WM_PAINT > WM_QUIT; 1 = found */
static int next_msg(uint32_t hwnd, uint32_t lo, uint32_t hi, int remove, Msg *out) {
    unsigned i;
#ifdef GPM2_OS9
    os9_tick();                                                /* no threads: the host only receives events if someone pumps */
#endif
    pump_host();
    audio_pump(win32_now_ms());
    ini_flush_due(win32_now_ms());
    fire_timers();
    for (i = qhead; i != qtail; i = (i + 1u) % MAX_Q)
        if (msg_match(&queue[i], hwnd, lo, hi)) {
            *out = queue[i];
            if (remove) {
                if (out->msg == WM_TIMER)
                    for (int t = 0; t < MAX_TIMER; t++)
                        if (timers[t].used && timers[t].hwnd == out->hwnd && timers[t].id == out->wp) timers[t].pending = 0;
                for (unsigned j = i; j != qhead; j = (j + MAX_Q - 1u) % MAX_Q) queue[j] = queue[(j + MAX_Q - 1u) % MAX_Q];
                qhead = (qhead + 1u) % MAX_Q;
            }
            return 1;
        }
    if (quit_pending && (lo == 0 || lo <= WM_QUIT) && (hi == 0 || hi >= WM_QUIT)) {
        memset(out, 0, sizeof *out);
        out->msg = WM_QUIT; out->wp = quit_code;
        if (remove) quit_pending = 0;
        return 1;
    }
    if (!lo || (lo <= WM_PAINT && (hi == 0 || hi >= WM_PAINT)))
        for (int k = 0; k < MAX_WND; k++)
            if (wnds[k].used && wnds[k].visible && wnds[k].invalid && (!hwnd || hwnd == hw(k))) {
                memset(out, 0, sizeof *out);
                out->hwnd = hw(k); out->msg = WM_PAINT; out->time = win32_now_ms();
                return 1;
            }
    return 0;
}

static void note_idle(void) {
    unsigned f = host_frame_count();
    if (f != (unsigned)last_frames) { last_frames = (int)f; idle_count = 0; }
    menu_sync();
    dlg_sync();
    if (host_poll_events((unsigned)idle_count)) { win32_log("script finished (%u frames)", f); win32_exit(0); }
    if (++idle_count >= opt_idle_exit && opt_idle_exit > 0 && !host_script_active()) {
        win32_log("idle in the message loop (%d polls without a new frame, %u frames)", idle_count, f);
        win32_exit(0);
    }
    if (opt_fixed_time_set) win32_advance_ms(1);
    else win32_input_wait(1);
}

static uint32_t u_PeekMessageA(void) {
    Msg m;
    int rm = (int)(ARG(4) & 1u);
    if (!next_msg(ARG(1), ARG(2), ARG(3), rm, &m)) { note_idle(); return 0; }
    if (rm && m.msg != WM_TIMER) idle_count = 0;
    fill_msg(ARG(0), &m);
    return 1;
}
static uint32_t u_WaitMessage(void) {
    Msg m;
    for (;;) {
        if (next_msg(0, 0, 0, 0, &m)) return 1;
        note_idle();
        /* advances to the next timer (virtual time) */
        if (opt_fixed_time_set) {
            uint32_t now = win32_now_ms(), best = 0;
            int have = 0;
            for (int i = 0; i < MAX_TIMER; i++)
                if (timers[i].used && !timers[i].pending) {
                    uint32_t d = timers[i].due - now;
                    if (!have || d < best) { best = d; have = 1; }
                }
            if (have && (int32_t)best > 1) win32_advance_ms(best - 1u);
        } else {
            uint32_t now = win32_now_ms(), best = 20;
            for (int i = 0; i < MAX_TIMER; i++)
                if (timers[i].used && !timers[i].pending) {
                    uint32_t d = timers[i].due - now;
                    if ((int32_t)d < (int32_t)best) best = (int32_t)d < 0 ? 0u : d;
                }
            win32_input_wait(best);
        }
    }
}
/* WM_KEYDOWN/WM_SYSKEYDOWN -> WM_CHAR/WM_SYSCHAR with the character the host computed (message pt.x) */
static int translate_msg(const Msg *m) {
    if ((m->msg == WM_KEYDOWN || m->msg == WM_SYSKEYDOWN) && m->x) {
        post(m->hwnd, m->msg == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, (uint32_t)m->x, m->lp & 0x3FFFFFFFu);
        return 1;
    }
    return 0;
}
static uint32_t dispatch_msg(const Msg *m) {
    uint32_t h = m->hwnd;
    if (m->msg == WM_TIMER && m->lp) return guest_call(m->lp, 4, h, m->msg, m->wp, win32_now_ms());
    if (!h) return 0;
    if (m->msg == WM_MOUSEMOVE) send(h, WM_SETCURSOR, h, 1u | (WM_MOUSEMOVE << 16));      /* HTCLIENT */
    return send(h, m->msg, m->wp, m->lp);
}
static void msg_from_guest(uint32_t p, Msg *m) {
    m->hwnd = R32(p); m->msg = R32(p + 4); m->wp = R32(p + 8); m->lp = R32(p + 12);
    m->time = R32(p + 16); m->x = (int)R32(p + 20); m->y = (int)R32(p + 24);
}
static uint32_t u_TranslateMessage(void) { Msg m; msg_from_guest(ARG(0), &m); return (uint32_t)translate_msg(&m); }
static uint32_t u_DispatchMessageA(void) { Msg m; msg_from_guest(ARG(0), &m); return dispatch_msg(&m); }

/* one step of a modal dialog's message loop (dialog.c): delivers a message to any window
 * (timers, painting and events of the other windows continue) or, with nothing to do, yields time. `dlg` gets
 * the keyboard as IsDialogMessage (Tab, Enter, Esc). */
void user_modal_step(uint32_t dlg) {
    Msg m;
    if (!next_msg(0, 0, 0, 1, &m)) { note_idle(); return; }
    if (m.msg != WM_TIMER) idle_count = 0;
    if (m.msg == WM_QUIT) { quit_pending = 1; dlg_abort(dlg); return; }
    if (dlg && (m.msg == WM_KEYDOWN) && dlg_key(dlg, m.hwnd, m.wp)) return;
    translate_msg(&m);
    dispatch_msg(&m);
}

/* ---- timers --------------------------------------------------------------------------------- */
static uint32_t u_SetTimer(void) {
    uint32_t h = ARG(0), id = ARG(1);
    int i, slot = -1;
    if (h == 0 && id == 0) id = next_timer_id++;
    for (i = 0; i < MAX_TIMER; i++) if (timers[i].used && timers[i].hwnd == h && timers[i].id == id) slot = i;
    if (slot < 0) for (i = 0; i < MAX_TIMER; i++) if (!timers[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    timers[slot].used = 1; timers[slot].hwnd = h; timers[slot].id = id; timers[slot].ms = ARG(2);
    timers[slot].proc = ARG(3); timers[slot].pending = 0;
    timers[slot].due = win32_now_ms() + (ARG(2) ? ARG(2) : 1u);
    return id;
}
static uint32_t u_KillTimer(void) {
    for (int i = 0; i < MAX_TIMER; i++)
        if (timers[i].used && timers[i].hwnd == ARG(0) && timers[i].id == ARG(1)) {
            timers[i].used = 0;
            for (unsigned j = qhead; j != qtail; j = (j + 1u) % MAX_Q)       /* removes pending WM_TIMER */
                if (queue[j].msg == WM_TIMER && queue[j].hwnd == ARG(0) && queue[j].wp == ARG(1)) queue[j].msg = 0;
            return 1;
        }
    return 0;
}

/* ---- system and resources ------------------------------------------------------------------------ */
static uint32_t u_GetSystemMetrics(void) {
    switch (ARG(0)) {
    case 0: return 640; case 1: return 480;
    case 2: case 3: case 20: case 21: return 16;          /* scroll bars */
    case 4: return 19;                                     /* SM_CYCAPTION */
    case 5: case 6: return 1;                              /* SM_CXBORDER, SM_CYBORDER */
    case 7: case 8: return 3;                              /* SM_CXDLGFRAME */
    case 9: case 10: return 16;
    case 11: case 12: case 13: case 14: return 32;         /* icon and cursor */
    case 15: return 19;                                    /* SM_CYMENU */
    case 16: return 640; case 17: return 480 - 19 - 28;
    case 19: return 1;                                     /* SM_MOUSEPRESENT */
    case 32: case 33: return 4;                            /* SM_CXFRAME */
    case 28: return 112; case 29: return 27;
    default: win32_stub_note("GetSystemMetrics(?)"); return 0;
    }
}
static uint32_t u_GetSysColor(void) { return gdi_sys_color((int)ARG(0)); }
static uint32_t u_SetSysColors(void) {
    (void)ARG(0);
    win32_stub_note("USER32.dll!SetSysColors");
    return 1;
}
static uint32_t u_LoadStringA(void) {
    char buf[1024];
    int n = res_string(ARG(1), buf, sizeof buf);
    if (n < 0 || ARG(3) == 0) { if (ARG(2) && ARG(3)) W8(ARG(2), 0); return 0; }
    gstr_putn(ARG(2), ARG(3), buf);
    return (uint32_t)n < ARG(3) ? (uint32_t)n : ARG(3) - 1u;
}
static uint32_t u_LoadCursorA(void) { return 0x00050000u + (ARG(1) < 0x10000u ? ARG(1) : 0x1000u); }
static uint32_t u_LoadIconA(void) { return 0x00060000u + (ARG(1) < 0x10000u ? ARG(1) : 0x1000u); }
/* cursors: handle = 0x50000 + IDC_*; handle 0 = no cursor (SetCursor(NULL)) */
static uint32_t u_SetCursor(void) {
    static uint32_t cur = 0x00057F00u;
    uint32_t o = cur, h = ARG(0);
    cur = h;
    host_set_cursor(h >= 0x50000u && h < 0x60000u ? h - 0x50000u : 0u);
    return o;
}
static uint32_t u_DestroyCursor(void) { return 1; }
static int cursor_count;
static uint32_t u_ShowCursor(void) { cursor_count += ARG(0) ? 1 : -1; host_show_cursor(cursor_count >= 0); return (uint32_t)cursor_count; }
static uint32_t u_GetDialogBaseUnits(void) { return 0x00100008u; }
static uint32_t u_SetCapture(void) { uint32_t o = capture_hwnd; capture_hwnd = ARG(0); return o; }
static uint32_t u_ReleaseCapture(void) { capture_hwnd = 0; return 1; }
static uint32_t u_MessageBeep(void) { win32_log("MessageBeep(0x%x)", ARG(0)); host_beep(); return 1; }
static uint32_t u_MessageBoxA(void) {
    char t[512], c[256];
    int r;
    gstr_get(t, sizeof t, ARG(1)); gstr_get(c, sizeof c, ARG(2));
    win32_log("MessageBoxA(\"%s\", \"%s\", 0x%x)", c, t, ARG(3));
    menu_sync();
    r = host_message_box(c, t, ARG(3));
    return (uint32_t)r;
}
static uint32_t u_wsprintfA(void) {
    static char out[2048];
    char fmt[1024];
    int n;
    gstr_get(fmt, sizeof fmt, ARG(1));
    n = gformat(out, sizeof out, fmt, g.esp + 12u);
    gmem_put(ARG(0), out, (uint32_t)n + 1u);
    return (uint32_t)n;
}
static uint32_t u_wvsprintfA(void) {
    static char out[2048];
    char fmt[1024];
    int n;
    gstr_get(fmt, sizeof fmt, ARG(1));
    n = gformat(out, sizeof out, fmt, ARG(2));
    gmem_put(ARG(0), out, (uint32_t)n + 1u);
    return (uint32_t)n;
}
static uint32_t u_VkKeyScanA(void) { uint32_t c = ARG(0) & 0xFFu; return c >= 'a' && c <= 'z' ? c - 32u : c; }
static uint32_t u_FindWindowA(void) { return 0; }

static uint32_t u_stub0(void) { win32_stub_note(win32_cur_import()); return 0; }
/* WinHelpA(hwnd, file, command, data): the game only uses HELP_INDEX (3, Help menu) and HELP_QUIT (2, on close) */
static uint32_t u_WinHelpA(void) {
    uint32_t cmd = ARG(2);
    if (cmd == 3 || cmd == 0xB) host_help(-1);                     /* HELP_INDEX / HELP_FINDER */
    else if (cmd == 1 || cmd == 8) host_help((int)ARG(3));         /* HELP_CONTEXT / HELP_CONTEXTPOPUP */
    return 1;                                                      /* HELP_QUIT and the rest: nothing to do */
}
static uint32_t u_stub1(void) { win32_stub_note(win32_cur_import()); return 1; }

static void builtin_class(const char *name, uint32_t proc) {
    int i;
    for (i = 0; i < MAX_CLS; i++) if (!classes[i].used) break;
    snprintf(classes[i].name, sizeof classes[i].name, "%s", name);
    classes[i].proc = proc; classes[i].used = 1; classes[i].brush = 6;
}

/* ---- Internal API for dialog.c / controls.c ------------------------------------------- */
int user_wininfo(uint32_t h, UWin *o) {
    Window *w = wnd(h);
    if (!w) return 0;
    o->style = w->style; o->exstyle = w->exstyle; o->parent = w->parent; o->id = w->id; o->proc = w->proc;
    o->x = w->x; o->y = w->y; o->w = w->w; o->h = w->h; o->cw = w->cw; o->ch = w->ch;
    o->visible = w->visible; o->enabled = w->enabled; o->top = is_top(w);
    return 1;
}
const char *user_wintitle(uint32_t h) { Window *w = wnd(h); return w ? w->title : ""; }
void user_set_wintitle(uint32_t h, const char *t) { Window *w = wnd(h); if (w) snprintf(w->title, sizeof w->title, "%s", t); }
uint32_t user_send(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) { return send(h, msg, wp, lp); }
void user_destroy(uint32_t h) { destroy_window(h); }
int user_children(uint32_t parent, uint32_t *out, int max) {
    int n = 0;
    for (int i = 0; i < MAX_WND && n < max; i++)
        if (wnds[i].used && wnds[i].parent == parent && !is_top(&wnds[i])) out[n++] = hw(i);
    return n;
}
uint32_t user_find_child(uint32_t parent, uint32_t id) {
    for (int i = 0; i < MAX_WND; i++)
        if (wnds[i].used && wnds[i].parent == parent && !is_top(&wnds[i]) && wnds[i].id == id) return hw(i);
    return 0;
}
void user_set_enabled(uint32_t h, int en) { Window *w = wnd(h); if (w && w->enabled != (en != 0)) { w->enabled = en != 0; ctl_touch(h); } }
const char *user_title(uint32_t h) { Window *w = wnd(h); return w ? w->title : ""; }
void user_invalidate(uint32_t h) { Window *w = wnd(h); if (w) invalidate(w, 0, 0, w->cw, w->ch, 1); }
void user_set_visible(uint32_t h, int v) { Window *w = wnd(h); if (w) { w->visible = v != 0; ctl_touch(h); } }
void user_move(uint32_t h, int x, int y) { Window *w = wnd(h); if (w) { w->x = x; w->y = y; } }
uint32_t user_focus(void) { return focus_hwnd; }
uint32_t user_defproc(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) { return def_proc(h, msg, wp, lp); }
int user_keydown(uint32_t vk) { return (keys[vk & 0xFFu] & 0x80) != 0; }
void user_set_focus(uint32_t h) {
    uint32_t old = focus_hwnd;
    if (!wnd(h) || h == old) return;
    if (old && wnd(old)) send(old, WM_KILLFOCUS, h, 0);
    focus_hwnd = h;
    send(h, WM_SETFOCUS, old, 0);
}
void user_activate(uint32_t h) { if (wnd(h)) active_hwnd = h; }

void user32_register(void) {
    static const char *const bl[] = {"BUTTON", "EDIT", "STATIC", "LISTBOX", "COMBOBOX", "SCROLLBAR", "ComboLBox", 0};
    static const uint32_t bp[] = {NP_BASE + CK_BUTTON, NP_BASE + CK_EDIT, NP_BASE + CK_STATIC, NP_BASE + CK_LISTBOX,
                                  NP_BASE + CK_COMBO, DEFPROC, DEFPROC};
    for (int i = 0; bl[i]; i++) builtin_class(bl[i], bp[i]);
    builtin_class("#32770", NP_DIALOG);
#define R(n, f) win32_register("USER32.dll!" n, f)
    R("RegisterClassA", u_RegisterClassA); R("UnregisterClassA", u_UnregisterClassA);
    R("GetClassInfoA", u_GetClassInfoA); R("GetClassLongA", u_GetClassLongA); R("SetClassLongA", u_SetClassLongA);
    R("CreateWindowExA", u_CreateWindowExA); R("ShowWindow", u_ShowWindow); R("DestroyWindow", u_DestroyWindow);
    R("SetFocus", u_SetFocus); R("SetActiveWindow", u_SetActiveWindow);
    R("GetClientRect", u_GetClientRect); R("GetWindowRect", u_GetWindowRect);
    R("MoveWindow", u_MoveWindow); R("SetWindowPos", u_SetWindowPos);
    R("GetWindowLongA", u_GetWindowLongA); R("SetWindowLongA", u_SetWindowLongA);
    R("IsWindowVisible", u_IsWindowVisible); R("IsIconic", u_IsIconic); R("IsWindowEnabled", u_IsWindowEnabled);
    R("EnableWindow", u_EnableWindow); R("SetWindowTextA", u_SetWindowTextA); R("GetWindowTextA", u_GetWindowTextA);
    R("BringWindowToTop", u_BringWindowToTop); R("GetDesktopWindow", u_GetDesktopWindow);
    R("GetDC", u_GetDC); R("ReleaseDC", u_ReleaseDC); R("BeginPaint", u_BeginPaint); R("EndPaint", u_EndPaint);
    R("InvalidateRect", u_InvalidateRect); R("RedrawWindow", u_RedrawWindow); R("FillRect", u_FillRect);
    R("SendMessageA", u_SendMessageA); R("PostMessageA", u_PostMessageA); R("PostQuitMessage", u_PostQuitMessage);
    R("DefWindowProcA", u_DefWindowProcA); R("CallWindowProcA", u_CallWindowProcA);
    R("PeekMessageA", u_PeekMessageA); R("WaitMessage", u_WaitMessage);
    R("TranslateMessage", u_TranslateMessage); R("DispatchMessageA", u_DispatchMessageA);
    R("SetTimer", u_SetTimer); R("KillTimer", u_KillTimer);
    R("GetSystemMetrics", u_GetSystemMetrics); R("GetSysColor", u_GetSysColor); R("SetSysColors", u_SetSysColors);
    R("LoadStringA", u_LoadStringA); R("LoadCursorA", u_LoadCursorA); R("LoadIconA", u_LoadIconA);
    R("SetCursor", u_SetCursor); R("DestroyCursor", u_DestroyCursor); R("ShowCursor", u_ShowCursor);
    R("GetDialogBaseUnits", u_GetDialogBaseUnits);
    R("GetKeyState", u_GetKeyState); R("SetCapture", u_SetCapture); R("ReleaseCapture", u_ReleaseCapture);
    R("MessageBeep", u_MessageBeep); R("MessageBoxA", u_MessageBoxA);
    R("wsprintfA", u_wsprintfA); R("wvsprintfA", u_wvsprintfA); R("VkKeyScanA", u_VkKeyScanA);
    R("FindWindowA", u_FindWindowA);
    R("GetAsyncKeyState", u_GetAsyncKeyState); R("GetCursorPos", u_GetCursorPos);
    R("ClientToScreen", u_ClientToScreen); R("ScreenToClient", u_ScreenToClient);
    R("GetFocus", u_GetFocus); R("GetCapture", u_GetCapture); R("GetActiveWindow", u_GetActiveWindow);
    /* menus: menu.c. Help and hooks: stubs */
    R("WinHelpA", u_WinHelpA);
    R("SetWindowsHookA", u_stub0); R("UnhookWindowsHook", u_stub1); R("CallNextHookEx", u_stub0);
#undef R
}
