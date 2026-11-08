/* Mac OS 9 backend (CarbonLib, PowerPC) via Retro68. Classic Toolbox (WaitNextEvent, Window/Control/List/Menu
 * Manager, CopyBits, Sound Manager): no threads, no Carbon Events, no CoreFoundation. The guest runs on the main thread;
 * every wait in the core (win32_input_wait, Sleep, PeekMessage) calls os9_pump, which is the only place that handles events. The
 * host_* functions are called by the guest on the same thread and act immediately (no queue). Built in CI only; not tested on real hardware. */
#include "../../pathjoin.h"
#include <Quickdraw.h>
#include <QDOffscreen.h>
#include <Windows.h>
#include <Menus.h>
#include <MacHelp.h>
#include <Dialogs.h>
#include <Controls.h>
#include <ControlDefinitions.h>
#include <Events.h>
#include <ToolUtils.h>
#include <TextUtils.h>
#include <OSUtils.h>
#include <Lists.h>
#include <Sound.h>
#include <Files.h>
#include <Folders.h>
#include <Navigation.h>
#include <AppleEvents.h>
#include <Appearance.h>
#include <MacMemory.h>
#include <Timer.h>
#include <Threads.h>
#include <Gestalt.h>
#include <InternetConfig.h>
#include <CFString.h>
#include <strings.h>

#include "../host.h"
#include "dlg_layout_os9.h"
#include "../dlg_layout.h"
#include "../sharpscale.h"
#include "../../win32/dplay_eng.h"
#include "../../gpm2run.h"
#include "../exe_sha.h"
extern void (*dispatch_yield_hook)(void);   /* dispatch.c */

#define GAME_W 640
#define GAME_H 480
#define MAXW 32
#define MAXD 8
#define MAXC 64

extern int opt_idle_exit;
extern int opt_fixed_time_set;
extern uint32_t opt_fixed_time;

static GpmConfig gCfg;
static int gScale10 = 20;                                      /* scale x10 (10 = 1x, 15 = 1.5x, 20 = 2x ...) */
static int gMaxScale10 = 20;
static int gFit10 = 20;                                         /* Zoom to Fit: largest scale that fits the window */
static int sc(int v) { return (v * gScale10 + 5) / 10; }
/* Full screen: the frameless window covers the main screen's desktop (the menu bar stays, the game menus are in it), without changing the resolution.
 * gFullMode: 0 = Pixel Perfect (largest integer scale), 1 = Fill Screen (largest scale keeping the aspect ratio), 2 = Stretch (ignores the aspect ratio). */
static int gFull, gFullMode, gOpenFull;
static int gImgX, gImgY, gImgW = GAME_W, gImgH = GAME_H;        /* image rectangle in the main window */
static void full_rect(Rect *r) { GDHandle gd = GetMainDevice(); *r = (**gd).gdRect; r->top = (short)(r->top + GetMBarHeight()); }
static void img_update(void) {
    Rect r;
    int dw, dh;
    if (!gFull) { gImgX = gImgY = 0; gImgW = sc(GAME_W); gImgH = sc(GAME_H); return; }
    full_rect(&r);
    dw = r.right - r.left; dh = r.bottom - r.top;
    if (gFullMode == 2) { gImgW = dw; gImgH = dh; }
    else if (gFullMode == 1) {
        if (dw * GAME_H <= dh * GAME_W) { gImgW = dw; gImgH = dw * GAME_H / GAME_W; }
        else { gImgH = dh; gImgW = dh * GAME_W / GAME_H; }
    } else {
        int k = dw / GAME_W < dh / GAME_H ? dw / GAME_W : dh / GAME_H;
        if (k < 1) k = 1;
        gImgW = GAME_W * k; gImgH = GAME_H * k;
    }
    gImgX = (dw - gImgW) / 2; gImgY = (dh - gImgH) / 2;
}
static int scx(int v) { return (v * gImgW + (v >= 0 ? GAME_W / 2 : -(GAME_W / 2))) / GAME_W; }
static int scy(int v) { return (v * gImgH + (v >= 0 ? GAME_H / 2 : -(GAME_H / 2))) / GAME_H; }
static int unsc(int v, int game, int img) { return v >= 0 ? v * game / img : -((-v * game + img - 1) / img); }
static void pstr(Str255 o, const char *s) { size_t n = strlen(s); if (n > 255) n = 255; o[0] = (unsigned char)n; memcpy(o + 1, s, n); }

/* FSSpec path as "/Volume/Folder/file" (this port's host format) */
static void fss_path(const FSSpec *s, char *out, size_t cap) {
    char names[24][64];
    int n = 0;
    long dir = s->parID;
    short v = s->vRefNum;
    size_t l = 0;
    Str255 nm;
    CInfoPBRec pb;
    memcpy(names[n], s->name + 1, s->name[0]); names[n][s->name[0]] = 0; n++;
    while (n < 24) {
        memset(&pb, 0, sizeof pb);
        pb.dirInfo.ioNamePtr = nm; pb.dirInfo.ioVRefNum = v; pb.dirInfo.ioDrDirID = dir; pb.dirInfo.ioFDirIndex = -1;
        if (PBGetCatInfoSync(&pb) != noErr) break;
        memcpy(names[n], nm + 1, nm[0]); names[n][nm[0]] = 0; n++;
        if (dir == 2) break;                                    /* volume root */
        dir = pb.dirInfo.ioDrParID;
    }
    out[0] = 0;
    for (int i = n - 1; i >= 0 && l + 2 < cap; i--) l += (size_t)snprintf(out + l, cap - l, "/%s", names[i]);
}

/* ---- game windows ------------------------------------------------------------------------------------- */
typedef struct {
    int used;
    uint32_t id;
    HostWindow hw;
    WindowPtr win;
    GWorldPtr gw;
    int pw, ph;
    int buttons;
    uint8_t *frame;                                            /* copy of the 8 bpp frame + palette (non-integer scale) */
    uint8_t pal[256][4];
    int dirty;
    GWorldPtr gw2;                                             /* frame already scaled to 32 bpp */
    int w2, h2;
} W;
static W gW[MAXW];                                             /* gW[0] = main window */
static HostWindow gMainInfo;
static char gPrefGame[1024];                                    /* game folder stored in the preferences */
static int gPosValid, gPosH, gPosV, gPrefScale10;               /* position (content origin) and scale read from the preferences */
static void prefs_save(void);
static uint32_t gCurId;
static int gCurHideWant, gCurHidden;

static W *w_find(uint32_t id) { for (int i = 0; i < MAXW; i++) if (gW[i].used && gW[i].id == id) return &gW[i]; return NULL; }
static W *w_of(WindowPtr p) { for (int i = 0; i < MAXW; i++) if (gW[i].used && gW[i].win == p) return &gW[i]; return NULL; }
static void content_origin(WindowPtr wp, int *x, int *y) {
    GrafPtr save; Point p = {0, 0};
    GetPort(&save); SetPortWindowPort(wp); LocalToGlobal(&p); SetPort(save);
    *x = p.h; *y = p.v;
}
static void push_ev_close(uint32_t win) { HostEvent e; memset(&e, 0, sizeof e); e.type = HOST_EV_CLOSE; e.win = win; host_push_event(&e); }

extern int win32_slow_trace;                                    /* win32/core.c */
static int gLogOn;                                              /* log only with Option held at launch */
static void log_flush(void) { if (!gLogOn) return; fflush(stderr); FlushVol(NULL, 0); }
/* non-integer scale: sharp bilinear in software, recomputed only when the frame (or the size) changes */
static int draw_scaled(W *w, const Rect *dst) {
    int tw = dst->right - dst->left, th = dst->bottom - dst->top;
    PixMapHandle pm;
    Rect r;
    if (!w->frame) return 0;
    if (!w->gw2 || w->w2 != tw || w->h2 != th) {
        if (w->gw2) DisposeGWorld(w->gw2);
        w->gw2 = NULL;
        SetRect(&r, 0, 0, tw, th);
        if (NewGWorld(&w->gw2, 32, &r, NULL, NULL, 0) != noErr) { w->gw2 = NULL; return 0; }
        w->w2 = tw; w->h2 = th; w->dirty = 1;
    }
    pm = GetGWorldPixMap(w->gw2);
    if (!LockPixels(pm)) return 0;
    if (w->dirty) {
        int ok = sharp_scale(w->frame, w->pw, w->ph, w->pal, tw, th, (uint32_t *)GetPixBaseAddr(pm), (**pm).rowBytes & 0x3FFF);
        if (!ok) { UnlockPixels(pm); return 0; }
        w->dirty = 0;
    }
    SetRect(&r, 0, 0, tw, th);
    CopyBits(GetPortBitMapForCopyBits(w->gw2), GetPortBitMapForCopyBits(GetWindowPort(w->win)), &r, dst, srcCopy, NULL);
    UnlockPixels(pm);
    return 1;
}
static void draw_w(W *w) {
    GrafPtr save; Rect src, dst;
    if (!w->win || !w->gw || !IsWindowVisible(w->win)) return;
    GetPort(&save); SetPortWindowPort(w->win);
    SetRect(&src, 0, 0, w->pw, w->ph);
    GetPortBounds(GetWindowPort(w->win), &dst);
    if (w == &gW[0] && gFull) {                                 /* black borders around the image */
        Rect b = dst, e;
        SetRect(&dst, gImgX, gImgY, gImgX + gImgW, gImgY + gImgH);
        ForeColor(blackColor); BackColor(whiteColor);
        SetRect(&e, b.left, b.top, b.right, dst.top); if (!EmptyRect(&e)) PaintRect(&e);
        SetRect(&e, b.left, dst.bottom, b.right, b.bottom); if (!EmptyRect(&e)) PaintRect(&e);
        SetRect(&e, b.left, dst.top, dst.left, dst.bottom); if (!EmptyRect(&e)) PaintRect(&e);
        SetRect(&e, dst.right, dst.top, b.right, dst.bottom); if (!EmptyRect(&e)) PaintRect(&e);
    }
    if (((dst.right - dst.left) % w->pw != 0 || (dst.bottom - dst.top) % w->ph != 0) && draw_scaled(w, &dst)) { SetPort(save); return; }
    if (!LockPixels(GetGWorldPixMap(w->gw))) { SetPort(save); return; }
    CopyBits(GetPortBitMapForCopyBits(w->gw), GetPortBitMapForCopyBits(GetWindowPort(w->win)), &src, &dst, srcCopy, NULL);
    UnlockPixels(GetGWorldPixMap(w->gw));
    SetPort(save);
}
static void layout_w(W *w) {
    int cw = w->hw.cw > 0 ? w->hw.cw : (w->pw ? w->pw : GAME_W), ch = w->hw.ch > 0 ? w->hw.ch : (w->ph ? w->ph : GAME_H);
    if (!w->win) return;
    if (w == &gW[0] && gFull) return;                           /* full screen: the window already covers the desktop */
    if (w != &gW[0] && gW[0].win) {
        int ox, oy;
        content_origin(gW[0].win, &ox, &oy);
        MoveWindow(w->win, (short)(ox + gImgX + scx((w->hw.x + w->hw.cx) - (gMainInfo.x + gMainInfo.cx))),
                           (short)(oy + gImgY + scy((w->hw.y + w->hw.cy) - (gMainInfo.y + gMainInfo.cy))), false);
    }
    SizeWindow(w->win, (short)scx(cw), (short)scy(ch), true);
}
static void raise_dialogs(void);
static void raise_children(void) {
    for (int i = 1; i < MAXW; i++) if (gW[i].used && gW[i].win && IsWindowVisible(gW[i].win)) BringToFront(gW[i].win);
    if (gFull) raise_dialogs();                                 /* the main window covers everything: a dialog behind it would be unreachable */
}
static void relayout_all(void) { img_update(); for (int i = 0; i < MAXW; i++) if (gW[i].used) { layout_w(&gW[i]); draw_w(&gW[i]); } }

static WindowPtr w_new_win(const HostWindow *hw, int main) {
    WindowPtr win;
    Rect r;
    Str255 t;
    short proc;
    SetRect(&r, 20, 60, 20 + sc(GAME_W), 60 + sc(GAME_H));
    if (main && gFull) full_rect(&r);
    else if (main && gPosValid) {                                    /* saved position, kept inside the visible area */
        Rect sb;
        int cw = sc(GAME_W), ch = sc(GAME_H), x = gPosH, y = gPosV;
        GetRegionBounds(GetGrayRgn(), &sb);
        if (x + cw > sb.right) x = sb.right - cw;
        if (y + ch > sb.bottom) y = sb.bottom - ch;
        if (x < sb.left) x = sb.left;
        if (y < sb.top + 40) y = sb.top + 40;                   /* menu bar + title bar */
        SetRect(&r, x, y, x + cw, y + ch);
    }
    proc = main ? (gFull ? plainDBox : noGrowDocProc) : (hw && hw->caption ? (hw->sysmenu ? noGrowDocProc : movableDBoxProc) : plainDBox);
    pstr(t, hw && hw->title[0] ? hw->title : "Grand Prix Manager 2");
    fprintf(stderr, "[host] first window %dx%d scale %d/10\n", sc(GAME_W), sc(GAME_H), gScale10);
    win = NewCWindow(NULL, &r, t, false, proc, (WindowPtr)-1L, (main && !gFull) || (!main && hw && hw->caption && hw->sysmenu), 0);
    if (win && main && gFull) { RGBColor k = {0, 0, 0}; SetWindowContentColor(win, &k); }
    return win;
}
static W *w_create(uint32_t id, const HostWindow *hw, int main) {
    W *w = NULL;
    for (int i = main ? 0 : 1; i < MAXW; i++) if (!gW[i].used) { w = &gW[i]; break; }
    if (!w) return NULL;
    memset(w, 0, sizeof *w);
    w->used = 1; w->id = id;
    if (hw) w->hw = *hw;
    w->win = w_new_win(hw, main);
    if (!w->win) { w->used = 0; return NULL; }
    return w;
}
void host_window_update(const HostWindow *hw) {
    W *w;
    if (hw->main) {
        Str255 t;
        gMainInfo = *hw;
        w = &gW[0];
        if (!w->used && !w_create(hw->id, hw, 1)) return;
        if (IsWindowVisible(w->win) && w->hw.cw == hw->cw && w->hw.ch == hw->ch && w->hw.x == hw->x && w->hw.y == hw->y &&
            w->hw.cx == hw->cx && w->hw.cy == hw->cy) {           /* only the order changed (the game's BringWindowToTop): nothing to redo */
            w->hw = *hw;
            return;
        }
        w->id = hw->id; w->hw = *hw;
        if (hw->title[0]) { pstr(t, hw->title); SetWTitle(w->win, t); }
        layout_w(w);
        if (!IsWindowVisible(w->win)) { ShowWindow(w->win); SelectWindow(w->win); }
        relayout_all();
        return;
    }
    w = w_find(hw->id);
    if (!w) {
        if (!hw->visible || hw->cw <= 0 || hw->ch <= 0) return;
        w = w_create(hw->id, hw, 0);
        if (!w) return;
    }
    w->hw = *hw;
    if (!hw->visible || hw->cw <= 0 || hw->ch <= 0) { HideWindow(w->win); return; }
    layout_w(w);
    if (!IsWindowVisible(w->win)) ShowHide(w->win, true);
    BringToFront(w->win);
    draw_w(w);
}
void host_window_destroy(uint32_t id) {
    W *w = w_find(id);
    if (!w || w == &gW[0]) return;
    if (w->win) DisposeWindow(w->win);
    if (w->gw) DisposeGWorld(w->gw);
    if (w->gw2) DisposeGWorld(w->gw2);
    free(w->frame);
    memset(w, 0, sizeof *w);
}
void host_present(uint32_t win, const uint8_t *pix, int pw, int ph, const uint8_t pal[256][4], const char *who) {
    W *w = w_find(win);
    PixMapHandle pm;
    CTabHandle ct;
    Ptr base;
    int rb;
    static unsigned nframes;
    (void)who;
    nframes++;
    if (nframes == 1 || nframes <= 3 || nframes % 100 == 0) { fprintf(stderr, "[host] present #%u win=%u %dx%d\n", nframes, (unsigned)win, pw, ph); log_flush(); }
    if (!w) w = &gW[0];
    if (!w->used) return;
    if (!w->gw || w->pw != pw || w->ph != ph) {
        Rect r;
        CTabHandle c8 = GetCTable(8);
        if (w->gw) DisposeGWorld(w->gw);
        w->gw = NULL;
        SetRect(&r, 0, 0, pw, ph);
        { uint32_t t0 = host_now_ms();
          if (NewGWorld(&w->gw, 8, &r, c8, NULL, 0) != noErr) w->gw = NULL;
          if (gLogOn) fprintf(stderr, "[host] NewGWorld %dx%d: %u ms\n", pw, ph, (unsigned)(host_now_ms() - t0)); }
        if (c8) DisposeCTable(c8);
        w->pw = pw; w->ph = ph;
        free(w->frame); w->frame = (uint8_t *)malloc((size_t)pw * (size_t)ph);
        w->dirty = 1;
        if (!w->gw) return;
    }
    if (w->frame && (w->dirty || memcmp(w->frame, pix, (size_t)pw * (size_t)ph) || memcmp(w->pal, pal, 1024))) {
        memcpy(w->frame, pix, (size_t)pw * (size_t)ph); memcpy(w->pal, pal, 1024); w->dirty = 1;
    }
    pm = GetGWorldPixMap(w->gw);
    if (!LockPixels(pm)) { fprintf(stderr, "[host] LockPixels failed (pixmap purged)\n"); return; }
    ct = (**pm).pmTable;
    for (int i = 0; i < 256; i++) {
        (**ct).ctTable[i].value = (short)i;
        (**ct).ctTable[i].rgb.red = (unsigned short)(pal[i][0] * 257); (**ct).ctTable[i].rgb.green = (unsigned short)(pal[i][1] * 257);
        (**ct).ctTable[i].rgb.blue = (unsigned short)(pal[i][2] * 257);
    }
    (**ct).ctSize = 255;
    (**ct).ctSeed = GetCTSeed();
    CTabChanged(ct);
    base = GetPixBaseAddr(pm);
    rb = (**pm).rowBytes & 0x3FFF;
    if (!base || rb < pw) { UnlockPixels(pm); fprintf(stderr, "[host] invalid pixmap (base=%p rowBytes=%d pw=%d)\n", (void *)base, rb, pw); return; }
    for (int y = 0; y < ph; y++) memcpy(base + (long)y * rb, pix + (long)y * pw, (size_t)pw);
    UnlockPixels(pm);
    draw_w(w);
    os9_tick();
}
void host_computer_name(char *out, size_t cap) { snprintf(out, cap, "GPM2"); }
void host_set_title(const char *title) { Str255 t; pstr(t, title); if (gW[0].win) SetWTitle(gW[0].win, t); }
void host_set_cursor(uint32_t idc) { gCurId = idc; }
void host_show_cursor(int visible) { gCurHideWant = !visible; }
static void apply_cursor(void) {
    switch (gCurId) {
    case 0x7F01: SetThemeCursor(kThemeIBeamCursor); break;
    case 0x7F02: SetThemeCursor(kThemeWatchCursor); break;
    case 0x7F03: SetThemeCursor(kThemeCrossCursor); break;
    default: SetThemeCursor(kThemeArrowCursor);
    }
}

/* ---- menus ----------------------------------------------------------------------------------------------- */
#define VIEW_MENU 127
#define CHAT_MENU 120                                           /* grayed out until Link Play is established */
static MenuHandle gChatM;
static int gChatOn;
#define APPLE_MENU 126
#define FILE_MENU 125
#define VIEW_FMODE 124
#define VIEW_SIZE 123                                           /* submenus: outside the range of the game menus (128+) */
static uint32_t gMid[256][96];                                  /* [menuID][item] -> core menu id */
static unsigned char gMsys[256];
static MenuHandle gMH[128];
static short gMI[128];
static int gNM;
static short gNextSub = 160;
static int gViewScales[16], gNView;
static short gHelpMid, gHelpItem;                               /* our item in the system Help menu */

static void build_menu(const HostMenuItem *it, int n, int sys, MenuHandle m, short mid) {
    Str255 t;
    for (int i = 0; i < n && i < 95; i++) {
        short ix = (short)(i + 1);
        if (it[i].separator) { AppendMenu(m, "\p(-"); continue; }
        AppendMenu(m, "\px");
        pstr(t, it[i].text); SetMenuItemText(m, ix, t);
        if (it[i].nchild > 0 && gNextSub < 250 && gNM < 126) {
            short sid = gNextSub++;
            MenuHandle sm = NewMenu(sid, "\p");
            build_menu(it[i].child, it[i].nchild, sys, sm, sid);
            InsertMenu(sm, -1);
            gMH[gNM] = sm; gMI[gNM++] = sid;
            SetMenuItemHierarchicalID(m, ix, sid);
        } else { gMid[mid][ix] = it[i].id; gMsys[mid] = (unsigned char)sys; }
        if (it[i].grayed) DisableMenuItem(m, ix);
        if (it[i].checked) CheckMenuItem(m, ix, true);
    }
}
static void view_menu_checks(void) {
    MenuHandle m = GetMenuHandle(VIEW_SIZE);
    if (m) for (int i = 0; i < gNView; i++) CheckMenuItem(m, (short)(i + 1), !gFull && gViewScales[i] == gScale10);
    m = GetMenuHandle(VIEW_FMODE);
    if (m) for (int i = 0; i < 3; i++) CheckMenuItem(m, (short)(i + 1), i == gFullMode);
    m = GetMenuHandle(VIEW_MENU);
    if (m) SetMenuItemText(m, 4, gFull ? "\pExit Full Screen" : "\pEnter Full Screen");
}
static void build_apple_menu(void) {
    MenuHandle a = NewMenu(APPLE_MENU, "\p\024");
    AppendMenu(a, "\pAbout Grand Prix Manager 2\311");
    InsertMenu(a, 0);
    {
        MenuHandle f = NewMenu(FILE_MENU, "\pFile");
        AppendMenu(f, "\pPreferences\311/,");
        AppendMenu(f, "\p(-");
        AppendMenu(f, "\pQuit/Q");
        InsertMenu(f, 0);
    }
    {                                                          /* no Cmd-?: on OS 9 that shortcut belongs to Mac OS Help */
        MenuRef hm = NULL;
        if (HMGetHelpMenu(&hm, NULL) == noErr && hm) {
            AppendMenu(hm, "\pGrand Prix Manager 2 Help");
            gHelpMid = GetMenuID(hm);
            gHelpItem = (short)CountMenuItems(hm);
        }
    }
}
static void build_view_menu(void) {
    MenuHandle v = NewMenu(VIEW_MENU, "\pView"), sz = NewMenu(VIEW_SIZE, "\p"), fm = NewMenu(VIEW_FMODE, "\p");
    Str255 t;
    char b[32];
    gNView = 0;
    AppendMenu(v, "\pWindow Size");
    for (int t10 = 10; t10 <= (gMaxScale10 < 20 ? 20 : gMaxScale10) && gNView < 16; t10 += (t10 <= 15 ? 5 : 10)) {
        if (t10 == 10) snprintf(b, sizeof b, "Actual Size (1x)");
        else if (t10 % 10) snprintf(b, sizeof b, "%d.%dx", t10 / 10, t10 % 10);
        else snprintf(b, sizeof b, "%dx", t10 / 10);
        AppendMenu(sz, "\px");
        pstr(t, b); SetMenuItemText(sz, (short)(gNView + 1), t);
        if (t10 % 10 == 0 && t10 <= 40) SetItemCmd(sz, (short)(gNView + 1), (CharParameter)('0' + t10 / 10));
        gViewScales[gNView++] = t10;
    }
    AppendMenu(v, "\pZoom to Fit Screen/0");
    AppendMenu(v, "\p(-");
    AppendMenu(v, "\pEnter Full Screen/F");
    AppendMenu(v, "\pFull Screen Mode");
    AppendMenu(fm, "\pPixel Perfect");
    AppendMenu(fm, "\pFill Screen");
    AppendMenu(fm, "\pStretch");
    InsertMenu(sz, -1);
    InsertMenu(fm, -1);
    SetMenuItemHierarchicalID(v, 1, VIEW_SIZE);
    SetMenuItemHierarchicalID(v, 5, VIEW_FMODE);
    InsertMenu(v, 0);
    gMH[gNM] = v; gMI[gNM++] = VIEW_MENU;
    gMH[gNM] = sz; gMI[gNM++] = VIEW_SIZE;
    gMH[gNM] = fm; gMI[gNM++] = VIEW_FMODE;
    view_menu_checks();
    gChatM = NewMenu(CHAT_MENU, "\pChat");
    AppendMenu(gChatM, "\pShow Chat");
    AppendMenu(gChatM, "\pChat Settings\311");
    if (!gChatOn) DisableMenuItem(gChatM, 0);
    InsertMenu(gChatM, 0);
    gMH[gNM] = gChatM; gMI[gNM++] = CHAT_MENU;
}
void host_chat_menu_enable(int on) {
    gChatOn = on != 0;
    if (!gChatM) return;
    if (gChatOn) EnableMenuItem(gChatM, 0); else DisableMenuItem(gChatM, 0);
    DrawMenuBar();
}
void host_set_menu(const HostMenuItem *top, int n, int sys) {
    short base = 128;
    for (int i = gNM - 1; i >= 0; i--) { DeleteMenu(gMI[i]); DisposeMenu(gMH[i]); }
    gNM = 0; gNextSub = 160;
    memset(gMid, 0, sizeof gMid);
    for (int i = 0; i < n && i < 30; i++) {
        Str255 t;
        MenuHandle m;
        pstr(t, top[i].text);
        m = NewMenu((short)(base + i), t);
        build_menu(top[i].child, top[i].nchild, sys, m, (short)(base + i));
        InsertMenu(m, 0);
        gMH[gNM] = m; gMI[gNM++] = (short)(base + i);
    }
    dispatch_yield_hook = os9_tick;
    build_view_menu();
    DrawMenuBar();
}
/* a larger scale must not leave the window outside the desktop */
static void keep_main_on_screen(void) {
    Rect sb;
    int x, y, cw = sc(GAME_W), ch = sc(GAME_H);
    if (gFull || !gW[0].win) return;
    content_origin(gW[0].win, &x, &y);
    GetRegionBounds(GetGrayRgn(), &sb);
    if (x + cw > sb.right) x = sb.right - cw;
    if (y + ch > sb.bottom) y = sb.bottom - ch;
    if (x < sb.left) x = sb.left;
    if (y < sb.top + 40) y = sb.top + 40;
    MoveWindow(gW[0].win, (short)x, (short)y, false);
}
static void set_fullscreen(int on) {
    W *w = &gW[0];
    WindowPtr old = w->win, nw;
    if (!on == !gFull) return;
    if (old && on) prefs_save();                                /* stores the position of the normal window */
    gFull = on;
    img_update();
    if (old) {
        nw = w_new_win(&w->hw, 1);
        if (!nw) { gFull = !on; img_update(); return; }
        { Str255 tt; GetWTitle(old, tt); SetWTitle(nw, tt); }    /* the title may have come from host_set_title */
        w->win = nw;
        ShowWindow(nw);
        SelectWindow(nw);
        SetPortWindowPort(nw);                                   /* the current port may be that of the window going away */
        DisposeWindow(old);
        raise_children();
        relayout_all();
    }
    view_menu_checks();
    prefs_save();
}
static void settings_run(void);
static void do_menu_select(long r) {
    short mid = HiWord(r), item = LoWord(r);
    HostEvent h;
    if (!mid) return;
    HiliteMenu(0);
    if (gHelpItem && mid == gHelpMid) {
        if (item == gHelpItem) host_help(-1);
        return;
    }
    if (mid == APPLE_MENU) {                                     /* same command as the game's Help > About */
        if (item == 1) { memset(&h, 0, sizeof h); h.type = HOST_EV_MENU; h.a = 40029; h.b = 1; host_push_event(&h); }
        return;
    }
    if (mid == FILE_MENU) {
        if (item == 1) settings_run();
        else if (item == 3) push_ev_close(0);                    /* Quit: same WM_CLOSE path as Cmd-Q */
        return;
    }
    if (mid == CHAT_MENU) {
        memset(&h, 0, sizeof h); h.type = HOST_EV_CHAT; h.a = item - 1; host_push_event(&h);
        return;
    }
    if (mid == VIEW_MENU) {
        if (item == 2) { gScale10 = gFit10; view_menu_checks(); keep_main_on_screen(); relayout_all(); prefs_save(); }
        else if (item == 4) set_fullscreen(!gFull);
        return;
    }
    if (mid == VIEW_FMODE) {
        if (item >= 1 && item <= 3) { gFullMode = item - 1; view_menu_checks(); relayout_all(); prefs_save(); }
        return;
    }
    if (mid == VIEW_SIZE) {
        if (item >= 1 && item <= gNView) { gScale10 = gViewScales[item - 1]; view_menu_checks(); keep_main_on_screen(); relayout_all(); prefs_save(); }
        return;
    }
    if (mid < 0 || mid > 255 || item < 1 || item > 95 || !gMid[mid][item]) return;
    memset(&h, 0, sizeof h);
    h.type = HOST_EV_MENU; h.a = (int)gMid[mid][item]; h.b = gMsys[mid];
    host_push_event(&h);
}

/* ---- native dialogs (Control Manager + List Manager) ------------------------------------------------------ */
typedef struct {
    int used;
    uint32_t hwnd, id;
    int kind;
    ControlHandle ref;
    ListHandle list;
    MenuHandle menu;                                            /* HC_COMBO_DROP: popup menu */
    short menu_id;
    Rect lr;
    char **items; int nitems;
    char text[256];
    int sel;
} Ctl;
typedef struct {
    int used; uint32_t id; WindowPtr win; int modal; Ctl c[MAXC];
    int chat, hig;                                              /* hig: own layout (Link Play), normal font */
    int ox, oy, cw, ch;                                         /* margin of the template dialogs; client area */
    char tmpl[64];
} Dlg;
static Dlg gD[MAXD];

static Dlg *d_find(uint32_t id) { for (int i = 0; i < MAXD; i++) if (gD[i].used && gD[i].id == id) return &gD[i]; return NULL; }
static Dlg *d_of(WindowPtr p) { for (int i = 0; i < MAXD; i++) if (gD[i].used && gD[i].win == p) return &gD[i]; return NULL; }
static void dlg_emit(Dlg *d, Ctl *c, int code, int idx, const char *text) {
    HostEvent e;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_DLG; e.win = d->id; e.a = (int)c->id; e.b = code; e.c = idx;
    if (text) snprintf(e.text, sizeof e.text, "%s", text);
    host_push_event(&e);
}
static void raise_dialogs(void) { for (int i = 0; i < MAXD; i++) if (gD[i].used && gD[i].win) BringToFront(gD[i].win); }
void host_dlg_open(const HostDlg *h) {
    Dlg *d = NULL;
    Rect r;
    Str255 t;
    int x = 100, y = 80, cw = h->w, ch = h->h;
    ControlHandle root;
    for (int i = 0; i < MAXD; i++) if (!gD[i].used) { d = &gD[i]; break; }
    if (!d) return;
    memset(d, 0, sizeof *d);
    d->used = 1; d->id = h->id; d->modal = h->modal; d->chat = h->chat;
    d->hig = dlg_layout_has(h->name, h->chat);
    snprintf(d->tmpl, sizeof d->tmpl, "%s", h->name);
    if (h->mw && !d->hig) { cw = h->mw; ch = h->mh; d->ox = h->ox; d->oy = h->oy; }
    dlg_layout_size(h->name, &cw, &ch);
    d->cw = cw; d->ch = ch;
    if (gW[0].win) {
        int ox, oy;
        content_origin(gW[0].win, &ox, &oy);
        if (h->chat) { x = ox + gImgX + 10; y = oy + gImgY + 30; }
        else { x = ox + gImgX + (gImgW - cw) / 2; y = oy + gImgY + (gImgH - ch) / 3; }
    }
    if (x < 4) x = 4;
    if (y < 44) y = 44;
    SetRect(&r, x, y, x + cw, y + ch);
    pstr(t, h->title);
    d->win = NewCWindow(NULL, &r, t, false, h->modal ? movableDBoxProc : h->chat ? documentProc : noGrowDocProc, (WindowPtr)-1L, !h->modal, 0);
    if (!d->win) { d->used = 0; return; }
    CreateRootControl(d->win, &root);
    SetThemeWindowBackground(d->win, kThemeBrushDialogBackgroundActive, false);
    ShowWindow(d->win);
    SelectWindow(d->win);
}
static void items_free(Ctl *c) { for (int i = 0; i < c->nitems; i++) free(c->items[i]); free(c->items); c->items = NULL; c->nitems = 0; }
static int items_same(const Ctl *c, const HostCtl *h) {
    if (c->nitems != h->nitems) return 0;
    for (int i = 0; i < c->nitems; i++) if (strcmp(c->items[i], h->items[i] ? h->items[i] : "") != 0) return 0;
    return 1;
}
static void list_select(Ctl *c, int sel) {
    Cell cell = {0, 0};
    if (!c->list) return;
    while (LGetSelect(true, &cell, c->list)) { LSetSelect(false, cell, c->list); cell.v++; cell.h = 0; if (cell.v >= c->nitems) break; }
    if (sel >= 0 && sel < c->nitems) { cell.h = 0; cell.v = (short)sel; LSetSelect(true, cell, c->list); LAutoScroll(c->list); }
}
static void list_place(Ctl *c, const Rect *r) {
    Rect vw = *r;
    Point cs;
    InsetRect(&vw, 1, 1); vw.right -= 15;
    (**c->list).rView = vw;
    LSize((short)(vw.right - vw.left), (short)(vw.bottom - vw.top), c->list);
    cs.h = (short)(vw.right - vw.left); cs.v = (**c->list).cellSize.v;
    LCellSize(cs, c->list);
}
void host_dlg_ctl(uint32_t dlg, const HostCtl *hin) {
    Dlg *d = d_find(dlg);
    Ctl *c = NULL;
    HostCtl lc = *hin;
    const HostCtl *h = &lc;
    Rect r;
    Str255 t;
    GrafPtr save;
    if (!d || !d->win) return;
    if (!d->hig) { lc.x += d->ox; lc.y += d->oy; }
    dlg_layout_ctl(d->tmpl, d->chat, d->cw, d->ch, &lc);
    if (d->hig && lc.kind == HC_EDIT) { lc.x += 3; lc.y += 3; lc.w -= 6; lc.h -= 6; }   /* the field draws its frame and focus ring outside the frame */
    if (!d->hig && (lc.kind == HC_CHECK || lc.kind == HC_RADIO)) {                      /* MS Sans Serif layout: the text must not be clipped */
        int need = (int)strlen(lc.text) * 6 + 24;
        if (need > lc.w) lc.w = need;
    }
    for (int i = 0; i < MAXC; i++) if (d->c[i].used && d->c[i].hwnd == h->hwnd) { c = &d->c[i]; break; }
    GetPort(&save); SetPortWindowPort(d->win);
    if (lc.visible && lc.x + lc.w + HOST_DLG_MARGIN > d->cw) {   /* a control that grew does not leave the window */
        d->cw = lc.x + lc.w + HOST_DLG_MARGIN;
        SizeWindow(d->win, (short)d->cw, (short)d->ch, true);
    }
    SetRect(&r, h->x, h->y, h->x + h->w, h->y + h->h);
    if (h->kind == HC_COMBO_LIST || h->kind == HC_COMBO_DROP) r.bottom = r.top + 20;        /* the template height includes the open list */
    if (!c) {
        short proc = -1;
        for (int i = 0; i < MAXC; i++) if (!d->c[i].used) { c = &d->c[i]; break; }
        if (!c) { SetPort(save); return; }
        memset(c, 0, sizeof *c);
        c->used = 1; c->hwnd = h->hwnd; c->id = h->id; c->kind = h->kind; c->sel = -1; c->lr = r;
        pstr(t, h->text);
        switch (h->kind) {
        case HC_PUSH: case HC_DEFPUSH: proc = kControlPushButtonProc; break;
        case HC_CHECK: proc = kControlCheckBoxProc; break;
        case HC_RADIO: proc = kControlRadioButtonProc; break;
        case HC_GROUP: proc = kControlGroupBoxTextTitleProc; break;
        case HC_EDIT: proc = h->password ? kControlEditTextPasswordProc : kControlEditTextProc; break;
        case HC_STATIC: proc = kControlStaticTextProc; break;
        case HC_COMBO_LIST: case HC_COMBO_DROP: {                /* CBS_DROPDOWNLIST and CBS_DROPDOWN become a popup */
            static short next_menu = 1000;
            c->menu_id = next_menu++;
            if (next_menu > 2000) next_menu = 1000;
            c->menu = NewMenu(c->menu_id, "\p");
            if (c->menu) { InsertMenu(c->menu, -1); proc = kControlPopupButtonProc; t[0] = 0; }
            break;
        }
        case HC_LIST: {
            Rect vw = r, db = {0, 0, 0, 1};
            Point cs = {0, 0};
            InsetRect(&vw, 1, 1); vw.right -= 15;
            c->list = LNew(&vw, &db, cs, 0, d->win, true, false, false, true);
            if (c->list) (**c->list).selFlags = h->multisel ? 0 : lOnlyOne;
            break;
        }
        default: break;
        }
        if (proc >= 0) {
            c->ref = c->menu ? NewControl(d->win, &r, t, true, 0, c->menu_id, 0, proc, 0)   /* popup: min = menu, max = title width */
                                              : NewControl(d->win, &r, t, true, 0, 0, 1, proc, 0);
            if (c->ref && d->hig && h->kind == HC_STATIC && h->align == 2) {
                ControlFontStyleRec fs;
                memset(&fs, 0, sizeof fs);
                fs.flags = kControlUseJustMask; fs.just = teFlushRight;
                SetControlFontStyle(c->ref, &fs);
            }
            if (c->ref && !d->hig && h->kind != HC_PUSH && h->kind != HC_DEFPUSH) {   /* small font: the layout is MS Sans Serif 8 */
                ControlFontStyleRec fs;
                memset(&fs, 0, sizeof fs);
                fs.flags = kControlUseThemeFontIDMask; fs.font = kThemeSmallSystemFont;
                SetControlFontStyle(c->ref, &fs);
            }
            if (c->ref && !d->hig) {                            /* the frame is opaque and the template creates it after the controls: whatever is inside gets embedded in it */
                for (int i = 0; i < MAXC; i++) {
                    Ctl *o = &d->c[i];
                    if (!o->used || o == c || !o->ref) continue;
                    if (h->kind == HC_GROUP && o->lr.left >= r.left && o->lr.top >= r.top && o->lr.right <= r.right && o->lr.bottom <= r.bottom) EmbedControl(o->ref, c->ref);
                    else if (h->kind != HC_GROUP && o->kind == HC_GROUP && r.left >= o->lr.left && r.top >= o->lr.top && r.right <= o->lr.right && r.bottom <= o->lr.bottom) { EmbedControl(c->ref, o->ref); break; }
                }
            }
            if (c->ref && h->kind == HC_DEFPUSH) { Boolean on = true; SetControlData(c->ref, kControlEntireControl, kControlPushButtonDefaultTag, sizeof on, &on); }
            if (c->ref && h->kind == HC_EDIT) { ControlHandle cur; if (GetKeyboardFocus(d->win, &cur) != noErr || !cur) SetKeyboardFocus(d->win, c->ref, kControlFocusNextPart); }
        }
        InvalWindowRect(d->win, &r);                            /* lists only draw on update */
    }
    if (c->list && !EqualRect(&c->lr, &r)) { list_place(c, &r); c->lr = r; InvalWindowRect(d->win, &r); }
    if (c->ref) {
        if (!EqualRect(&c->lr, &r)) { MoveControl(c->ref, r.left, r.top); SizeControl(c->ref, (short)(r.right - r.left), (short)(r.bottom - r.top)); c->lr = r; }
        if (h->visible) ShowControl(c->ref); else HideControl(c->ref);
        HiliteControl(c->ref, h->enabled ? 0 : 255);
    }
    switch (h->kind) {
    case HC_PUSH: case HC_DEFPUSH: case HC_CHECK: case HC_RADIO: case HC_GROUP:
        if (c->ref) {
            if (strcmp(c->text, h->text) != 0) { pstr(t, h->text); SetControlTitle(c->ref, t); snprintf(c->text, sizeof c->text, "%s", h->text); }
            if (h->kind == HC_CHECK || h->kind == HC_RADIO) { if (GetControlValue(c->ref) != (h->checked ? 1 : 0)) SetControlValue(c->ref, h->checked ? 1 : 0); }
        }
        break;
    case HC_STATIC:
        if (c->ref && strcmp(c->text, h->text) != 0) {
            SetControlData(c->ref, kControlEntireControl, kControlStaticTextTextTag, (Size)strlen(h->text), (Ptr)h->text);
            snprintf(c->text, sizeof c->text, "%s", h->text);
            Draw1Control(c->ref);
        }
        break;
    case HC_EDIT:
        if (c->ref && strcmp(c->text, h->text) != 0) {              /* only what the core changed (what the user typed is already there) */
            SetControlData(c->ref, kControlEntireControl, kControlEditTextTextTag, (Size)strlen(h->text), (Ptr)h->text);
            snprintf(c->text, sizeof c->text, "%s", h->text);
            Draw1Control(c->ref);
        }
        break;
    case HC_COMBO_LIST: case HC_COMBO_DROP:
        if (c->ref && c->menu) {
            Str255 it;
            if (!items_same(c, h)) {
                while (CountMenuItems(c->menu) > 0) DeleteMenuItem(c->menu, 1);
                items_free(c);
                c->items = (char **)calloc((size_t)(h->nitems > 0 ? h->nitems : 1), sizeof(char *));
                c->nitems = h->nitems > 0 ? h->nitems : 0;
                for (int i = 0; i < c->nitems; i++) {
                    c->items[i] = strdup(h->items[i] ? h->items[i] : "");
                    AppendMenu(c->menu, "\px");                  /* SetMenuItemText: the name does not go through the metacharacters */
                    pstr(it, c->items[i]);
                    SetMenuItemText(c->menu, (short)(i + 1), it);
                }
                SetControlMaximum(c->ref, c->nitems);
                c->sel = -2;
            }
            if (h->sel != c->sel) { c->sel = h->sel; SetControlValue(c->ref, h->sel >= 0 ? h->sel + 1 : 0); }
        }
        break;
    case HC_LIST:
        if (c->list) {
            if (!items_same(c, h)) {
                if ((**c->list).dataBounds.bottom > 0) LDelRow(0, 0, c->list);
                items_free(c);
                c->items = (char **)calloc((size_t)(h->nitems > 0 ? h->nitems : 1), sizeof(char *));
                c->nitems = h->nitems > 0 ? h->nitems : 0;
                if (c->nitems) LAddRow(c->nitems, 0, c->list);
                for (int i = 0; i < c->nitems; i++) {
                    Cell cell; cell.h = 0; cell.v = (short)i;
                    c->items[i] = strdup(h->items[i] ? h->items[i] : "");
                    LSetCell(c->items[i], (short)strlen(c->items[i]), cell, c->list);
                }
                c->sel = -2;
            }
            if (h->sel != c->sel) { c->sel = h->sel; list_select(c, h->sel); }
            LSetDrawingMode(h->visible != 0, c->list);
        }
        break;
    default: break;
    }
    SetPort(save);
}
void host_dlg_close(uint32_t id) {
    Dlg *d = d_find(id);
    if (!d) return;
    for (int i = 0; i < MAXC; i++) if (d->c[i].used) { if (d->c[i].list) LDispose(d->c[i].list); if (d->c[i].menu) { DeleteMenu(d->c[i].menu_id); DisposeMenu(d->c[i].menu); } items_free(&d->c[i]); }
    if (d->win) DisposeWindow(d->win);
    memset(d, 0, sizeof *d);
}
/* These change the window order: on the main thread, the only one that handles events */
static void on_main(void (*fn)(void *), void *arg);
static void front_call(void *p) { Dlg *d = d_find(*(uint32_t *)p); if (d && d->win) SelectWindow(d->win); }
/* Send Window To Back: only the chat goes to the back (the system activates the window left in front). Selecting the game window
 * and bringing the auxiliary ones on top, as before, left the game unresponsive to clicks on OS 9. */
static void back_call(void *p) { Dlg *d = d_find(*(uint32_t *)p); if (d && d->win) SendBehind(d->win, NULL); }
static void center_call(void *p) {
    Dlg *d = d_find(*(uint32_t *)p);
    int ox, oy;
    if (!d || !d->win || !gW[0].win) return;
    content_origin(gW[0].win, &ox, &oy);
    MoveWindow(d->win, (short)(ox + gImgX + (gImgW - d->cw) / 2), (short)(oy + gImgY + (gImgH - d->ch) / 2), false);
}
static void beep_call(void *p) { (void)p; SysBeep(1); }
void host_dlg_front(uint32_t dlg) { on_main(front_call, &dlg); }
void host_dlg_back(uint32_t dlg) { on_main(back_call, &dlg); }
void host_dlg_center(uint32_t dlg) { on_main(center_call, &dlg); }
void host_beep(void) { on_main(beep_call, NULL); }
/* chat resized: the grid is the same as host_dlg_ctl's, with the new size */
static void chat_relayout(Dlg *d, int w, int ht) {
    GrafPtr save;
    Rect all;
    GetPort(&save); SetPortWindowPort(d->win);
    d->cw = w; d->ch = ht;
    for (int i = 0; i < MAXC; i++) {
        Ctl *c = &d->c[i];
        HostCtl lc;
        Rect r;
        if (!c->used) continue;
        memset(&lc, 0, sizeof lc);
        lc.kind = c->kind;
        dlg_layout_ctl(d->tmpl, 1, w, ht, &lc);
        if (lc.kind == HC_EDIT) { lc.x += 3; lc.y += 3; lc.w -= 6; lc.h -= 6; }
        SetRect(&r, lc.x, lc.y, lc.x + lc.w, lc.y + lc.h);
        if (c->list) list_place(c, &r);
        if (c->ref) { MoveControl(c->ref, r.left, r.top); SizeControl(c->ref, (short)(r.right - r.left), (short)(r.bottom - r.top)); }
        c->lr = r;
    }
    GetWindowPortBounds(d->win, &all);
    ClipRect(&all);                                              /* the port clip kept the old size: whatever grew did not draw */
    EraseRect(&all);
    InvalWindowRect(d->win, &all);
    SetPort(save);
}
static void dlg_draw(Dlg *d) {
    RgnHandle rg = NewRgn();
    if (d->chat) { Rect pb; GetWindowPortBounds(d->win, &pb); ClipRect(&pb); }
    DrawControls(d->win);
    if (d->chat) {                                               /* only the resize box, without the scroll bar lines */
        Rect all, gr;
        GetWindowPortBounds(d->win, &all);
        gr = all; gr.left = gr.right - 15; gr.top = gr.bottom - 15;
        ClipRect(&gr);
        DrawGrowIcon(d->win);
        ClipRect(&all);
    }
    GetPortVisibleRegion(GetWindowPort(d->win), rg);
    for (int i = 0; i < MAXC; i++)
        if (d->c[i].used && d->c[i].list) { Rect f = d->c[i].lr; FrameRect(&f); LUpdate(rg, d->c[i].list); }
    DisposeRgn(rg);
}
static void dlg_click(Dlg *d, EventRecord *e) {
    Point lp = e->where;
    ControlHandle hit = NULL;
    GrafPtr save;
    GetPort(&save); SetPortWindowPort(d->win); GlobalToLocal(&lp);
    for (int i = 0; i < MAXC; i++) {
        Ctl *c = &d->c[i];
        if (c->used && c->list && PtInRect(lp, &c->lr)) {
            Boolean dbl = LClick(lp, e->modifiers, c->list);
            Cell cell = {0, 0};
            int sel = -1;
            if (LGetSelect(true, &cell, c->list)) sel = cell.v;
            if (sel != c->sel) { c->sel = sel; dlg_emit(d, c, DLG_SEL, sel, NULL); }
            if (dbl && sel >= 0) dlg_emit(d, c, DLG_DBLCLK, sel, NULL);
            SetPort(save);
            return;
        }
    }
    if (FindControl(lp, d->win, &hit) && hit) {
        for (int i = 0; i < MAXC; i++) {
            Ctl *c = &d->c[i];
            if (!c->used || c->ref != hit) continue;
            if (c->kind == HC_EDIT) { SetKeyboardFocus(d->win, hit, kControlFocusNextPart); TrackControl(hit, lp, NULL); }
            else if (c->menu) {
                if (TrackControl(hit, lp, (ControlActionUPP)-1L)) {
                    int sel = (int)GetControlValue(hit) - 1;
                    if (sel != c->sel) { c->sel = sel; dlg_emit(d, c, DLG_SEL, sel, NULL); }
                }
            }
            else if (TrackControl(hit, lp, NULL)) dlg_emit(d, c, DLG_CLICK, 0, NULL);
            break;
        }
    }
    SetPort(save);
}
static void dlg_key(Dlg *d, EventRecord *e) {
    int ch = (int)(e->message & charCodeMask);
    ControlHandle f = NULL;
    if (ch == 13 || ch == 3 || ch == 27) {
        for (int i = 0; i < MAXC; i++)
            if (d->c[i].used && ((ch == 27 && d->c[i].kind == HC_PUSH && d->c[i].id == 2) || (ch != 27 && d->c[i].kind == HC_DEFPUSH))) { dlg_emit(d, &d->c[i], DLG_CLICK, 0, NULL); return; }
    }
    if (GetKeyboardFocus(d->win, &f) == noErr && f) HandleControlKey(f, (SInt16)((e->message & keyCodeMask) >> 8), (SInt16)ch, e->modifiers);
}
static void poll_edits(void) {
    for (int i = 0; i < MAXD; i++) {
        if (!gD[i].used) continue;
        for (int k = 0; k < MAXC; k++) {
            Ctl *c = &gD[i].c[k];
            char buf[256];
            Size n = 0;
            if (!c->used || c->kind != HC_EDIT || !c->ref) continue;
            if (GetControlData(c->ref, kControlEntireControl, kControlEditTextTextTag, sizeof buf - 1, buf, &n) != noErr) continue;
            buf[n < 0 ? 0 : (n > 255 ? 255 : n)] = 0;
            if (strcmp(buf, c->text) != 0) { snprintf(c->text, sizeof c->text, "%s", buf); dlg_emit(&gD[i], c, DLG_TEXT, 0, buf); }
        }
    }
}

/* ---- MessageBox, network, files --------------------------------------------------------------------------- */
/* Fallback when StandardAlert fails: modal dialog built in memory (DITL), with ModalDialog (pumps events). */
static int fallback_alert(const char *title, const char *text, const char **names, int n, int cancel) {
    unsigned char buf[1024];
    size_t o = 0, tl = strlen(text), nb = (size_t)n;
    int lines = (int)(tl / 52) + 1, h = 16 * lines + 60, i, w = 380;
    Rect r;
    Handle ditl;
    DialogPtr dlg;
    SInt16 hit = 1;
    Str255 ti;
    if (tl > 600) tl = 600;
    buf[o++] = 0; buf[o++] = (unsigned char)(n);                   /* items - 1 = n (buttons + text - 1) */
    for (i = 0; i < n; i++) {                                      /* buttons: 1..n, right to left */
        size_t l = strlen(names[i]);
        int right = w - 12 - i * 82, left = right - 74, top = h - 32;
        memset(buf + o, 0, 4); o += 4;
        buf[o++] = (unsigned char)(top >> 8); buf[o++] = (unsigned char)top; buf[o++] = (unsigned char)(left >> 8); buf[o++] = (unsigned char)left;
        buf[o++] = (unsigned char)((top + 20) >> 8); buf[o++] = (unsigned char)(top + 20); buf[o++] = (unsigned char)(right >> 8); buf[o++] = (unsigned char)right;
        buf[o++] = 4; buf[o++] = (unsigned char)l; memcpy(buf + o, names[i], l); o += l; if (l & 1) buf[o++] = 0;
    }
    memset(buf + o, 0, 4); o += 4;                                 /* text: item n+1 */
    buf[o++] = 0; buf[o++] = 12; buf[o++] = 0; buf[o++] = 12;
    buf[o++] = (unsigned char)((12 + 16 * lines) >> 8); buf[o++] = (unsigned char)(12 + 16 * lines); buf[o++] = (unsigned char)((w - 12) >> 8); buf[o++] = (unsigned char)(w - 12);
    buf[o++] = 8 | 128; buf[o++] = (unsigned char)tl; memcpy(buf + o, text, tl); o += tl; if (tl & 1) buf[o++] = 0;
    ditl = NewHandle((Size)o);
    if (!ditl) return 0;
    BlockMoveData(buf, *ditl, (Size)o);
    pstr(ti, title);
    SetRect(&r, 0, 0, w, h); OffsetRect(&r, 80, 80);
    dlg = NewDialog(NULL, &r, ti, true, 1 /* dBoxProc */, (WindowPtr)-1, false, 0, ditl);
    if (!dlg) { DisposeHandle(ditl); return 0; }
    SetDialogDefaultItem(dlg, 1);
    if (cancel > 0) SetDialogCancelItem(dlg, cancel);
    SelectWindow(GetDialogWindow(dlg));
    ShowWindow(GetDialogWindow(dlg));
    do { ModalDialog(NULL, &hit); } while (hit < 1 || hit > (SInt16)nb);
    DisposeDialog(dlg);
    return hit;
}
static int message_box_impl(const char *title, const char *text, uint32_t flags) {
    static const struct { const char *name; int id; } sets[6][3] = {
        {{"OK", 1}, {0, 0}, {0, 0}}, {{"OK", 1}, {"Cancel", 2}, {0, 0}}, {{"Abort", 3}, {"Retry", 4}, {"Ignore", 5}},
        {{"Yes", 6}, {"No", 7}, {"Cancel", 2}}, {{"Yes", 6}, {"No", 7}, {0, 0}}, {{"Retry", 4}, {"Cancel", 2}, {0, 0}}};
    unsigned ty = flags & 0xFu, def = (flags >> 8) & 0xFu;
    AlertType at = (flags & 0xF0u) == 0x10u ? kAlertStopAlert : (flags & 0xF0u) == 0x30u ? kAlertCautionAlert : kAlertNoteAlert;
    Str255 ti, tx, b[3];
    const char *nm[3];
    AlertStdAlertParamRec pr;
    SInt16 hit = 0;
    OSErr err;
    int n = 0, order[3] = {0, 1, 2}, k, cancel = 0;
    if (ty > 5) ty = 0;
    while (n < 3 && sets[ty][n].name) n++;
    if (def >= (unsigned)n) def = 0;
    order[0] = (int)def; order[def] = 0;
    for (int i = 0; i < n; i++) { nm[i] = sets[ty][order[i]].name; pstr(b[i], nm[i]); }
    for (int i = 0; i < n; i++) if (sets[ty][order[i]].id == 2 || sets[ty][order[i]].id == 7) cancel = i + 1;
    pstr(ti, title); pstr(tx, text);
    memset(&pr, 0, sizeof pr);
    pr.movable = true; pr.defaultText = b[0]; pr.cancelText = n > 1 ? b[1] : NULL; pr.otherText = n > 2 ? b[2] : NULL;
    pr.defaultButton = kAlertStdAlertOKButton;
    pr.cancelButton = n > 1 ? kAlertStdAlertCancelButton : 0;
    pr.position = kWindowDefaultPosition;
    err = StandardAlert(at, ti, tx, &pr, &hit);
    if (err != noErr) {
        fprintf(stderr, "[host] StandardAlert failed (err %d, item %d); using the built-in dialog\n", (int)err, (int)hit);
        hit = (SInt16)fallback_alert(title, text, nm, n, cancel);
    }
    k = hit - 1;                                               /* 1 default, 2 cancel, 3 other */
    if (k < 0 || k >= n) k = 0;
    return sets[ty][order[k]].id;
}
/* Link Play: asks for the host address ("ip" or "ip:port"; empty = LAN discovery only) in a dialog built in memory */
static int net_ask_impl(char *out, int cap) {
    static char last[64];
    unsigned char buf[512];
    const char *lab = "Host address (IP or IP:port). Leave empty to search the local network only.", *bn[2] = {"OK", "Cancel"};
    size_t o = 0, ll = strlen(lab);
    int w = 380, h = 130, i;
    Rect r;
    Handle ditl;
    DialogPtr dlg;
    SInt16 hit = 0, type;
    Handle ih;
    Rect ir;
    Str255 ti, tx;
    buf[o++] = 0; buf[o++] = 3;                                    /* 4 items */
    for (i = 0; i < 2; i++) {                                      /* 1 = OK, 2 = Cancel */
        size_t l = strlen(bn[i]);
        int right = w - 12 - i * 82, left = right - 74, top = h - 32;
        memset(buf + o, 0, 4); o += 4;
        buf[o++] = (unsigned char)(top >> 8); buf[o++] = (unsigned char)top; buf[o++] = (unsigned char)(left >> 8); buf[o++] = (unsigned char)left;
        buf[o++] = (unsigned char)((top + 20) >> 8); buf[o++] = (unsigned char)(top + 20); buf[o++] = (unsigned char)(right >> 8); buf[o++] = (unsigned char)right;
        buf[o++] = 4; buf[o++] = (unsigned char)l; memcpy(buf + o, bn[i], l); o += l; if (l & 1) buf[o++] = 0;
    }
    memset(buf + o, 0, 4); o += 4;                                 /* 3 = text */
    buf[o++] = 0; buf[o++] = 12; buf[o++] = 0; buf[o++] = 12;
    buf[o++] = 0; buf[o++] = 44; buf[o++] = (unsigned char)((w - 12) >> 8); buf[o++] = (unsigned char)(w - 12);
    buf[o++] = 8 | 128; buf[o++] = (unsigned char)ll; memcpy(buf + o, lab, ll); o += ll; if (ll & 1) buf[o++] = 0;
    memset(buf + o, 0, 4); o += 4;                                 /* 4 = edit field */
    buf[o++] = 0; buf[o++] = 56; buf[o++] = 0; buf[o++] = 12;
    buf[o++] = 0; buf[o++] = 76; buf[o++] = (unsigned char)((w - 12) >> 8); buf[o++] = (unsigned char)(w - 12);
    buf[o++] = 16; buf[o++] = 0;
    ditl = NewHandle((Size)o);
    if (!ditl) return 0;
    BlockMoveData(buf, *ditl, (Size)o);
    pstr(ti, "Link Play");
    SetRect(&r, 0, 0, w, h); OffsetRect(&r, 80, 80);
    dlg = NewDialog(NULL, &r, ti, true, 1 /* dBoxProc */, (WindowPtr)-1, false, 0, ditl);
    if (!dlg) { DisposeHandle(ditl); return 0; }
    SetDialogDefaultItem(dlg, 1);
    SetDialogCancelItem(dlg, 2);
    GetDialogItem(dlg, 4, &type, &ih, &ir);
    pstr(tx, last); SetDialogItemText(ih, tx);
    SelectDialogItemText(dlg, 4, 0, 255);
    SelectWindow(GetDialogWindow(dlg));
    ShowWindow(GetDialogWindow(dlg));
    do { ModalDialog(NULL, &hit); } while (hit != 1 && hit != 2);
    if (hit == 1) {
        GetDialogItem(dlg, 4, &type, &ih, &ir);
        GetDialogItemText(ih, tx);
        i = tx[0] < sizeof last - 1 ? tx[0] : (int)sizeof last - 1;
        memcpy(last, tx + 1, (size_t)i); last[i] = 0;
        snprintf(out, (size_t)cap, "%s", last);
    }
    DisposeDialog(dlg);
    return hit == 1;
}
/* Help: GPMHELP.HLP becomes HTML in <wdir>/Help (hlp.c) and opens in the default browser via Internet Config */
void host_help(int context) {
    char idx[1200], url[3000];
    size_t n = 0;
    ICInstance ic;
    long s, e;
    if (!gCfg.game || !gCfg.wdir || !hlp_prepare(gCfg.game, gCfg.wdir, idx, sizeof idx)) {
        host_message_box("Help", "GPMHELP.HLP was not found in the game folder.", 0x30);
        return;
    }
    n += (size_t)snprintf(url, sizeof url, "file://");
    for (const char *p = idx; *p && n + 4 < sizeof url; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || strchr("/._-", c)) url[n++] = (char)c;
        else n += (size_t)snprintf(url + n, sizeof url - n, "%%%02X", c);
    }
    url[n] = 0;
    if (context >= 0) n += (size_t)snprintf(url + n, sizeof url - n, "#ctx%d", context);
    s = 0; e = (long)n;
    if (ICStart(&ic, 'GPM2') == noErr) { ICLaunchURL(ic, "\p", url, (long)n, &s, &e); ICStop(ic); }
}
/* Navigation Services (API 1.x, in CarbonLib 1.0+): mode 0 open, 1 save, 2 choose folder; returns the chosen FSSpec */
static char gNavPat[128];                                       /* chosen filter pattern ("*.gps;*.gcf") */
static int pat_match(const char *name) {
    char pat[128], *tok, *sv = NULL;
    size_t nl = strlen(name);
    if (!gNavPat[0]) return 1;
    snprintf(pat, sizeof pat, "%s", gNavPat);
    for (tok = strtok(pat, ";"); tok; tok = strtok(NULL, ";")) {
        const char *ext = strrchr(tok, '.');
        size_t el;
        (void)sv;
        if (!ext) continue;
        if (!strcmp(ext, ".*")) return 1;
        el = strlen(ext);
        if (nl >= el && !strncasecmp(name + nl - el, ext, el)) return 1;
    }
    return 0;
}
static pascal Boolean nav_filter(AEDescPtr item, void *info, void *ud, NavFilterModes mode) {
    NavFileOrFolderInfo *fi = (NavFileOrFolderInfo *)info;
    FSSpec sp;
    char nm[300];
    (void)ud; (void)mode;
    if (!fi || fi->isFolder || item->descriptorType != typeFSS) return true;
    if (AEGetDescData(item, &sp, sizeof sp) != noErr) return true;
    memcpy(nm, sp.name + 1, sp.name[0]); nm[sp.name[0]] = 0;
    return pat_match(nm) != 0;
}
static int nav_pick(int mode, const char *msg, const char *name, FSSpec *out, const char *pat, const char *dir) {
    NavDialogOptions o;
    NavReplyRecord rep;
    OSErr err;
    int ok = 0;
    AEDesc loc;
    AEDesc *locp = NULL;
    NavObjectFilterUPP fu = NULL;
    if (NavGetDefaultDialogOptions(&o) != noErr) return 0;
    o.dialogOptionFlags &= ~kNavAllowMultipleFiles;
    o.preferenceKey = 0;
    pstr(o.message, msg); pstr(o.savedFileName, name);
    if (dir && dir[0]) {                                        /* initial folder: "/Vol/Folder" -> "Vol:Folder:" */
        char m[PATH_MAX];
        Str255 ps;
        FSSpec fs;
        size_t n = 0;
        for (const char *q = dir + (*dir == '/'); *q && n + 2 < sizeof m; q++) m[n++] = *q == '/' ? ':' : *q;
        if (n && m[n - 1] != ':') m[n++] = ':';
        m[n] = 0;
        pstr(ps, m);
        if (FSMakeFSSpec(0, 0, ps, &fs) == noErr && AECreateDesc(typeFSS, &fs, sizeof fs, &loc) == noErr) locp = &loc;
    }
    gNavPat[0] = 0;
    if (pat) snprintf(gNavPat, sizeof gNavPat, "%s", pat);
    if (mode == 0 && pat) fu = NewNavObjectFilterUPP(nav_filter);
    if (mode == 1) err = NavPutFile(locp, &rep, &o, NULL, '????', 'GPM2', NULL);
    else if (mode == 2) err = NavChooseFolder(locp, &rep, &o, NULL, NULL, NULL);
    else err = NavGetFile(locp, &rep, &o, NULL, NULL, fu, NULL, NULL);
    if (locp) AEDisposeDesc(locp);
    if (err == noErr && rep.validRecord) {
        AEKeyword kw; DescType ty; Size sz;
        if (AEGetNthPtr(&rep.selection, 1, typeFSS, &kw, &ty, out, sizeof *out, &sz) == noErr) ok = 1;
    }
    if (err == noErr) NavDisposeReply(&rep);
    return ok;
}
static int file_dialog_impl(const HostFileReq *rq, char *out, size_t cap) {
    FSSpec sp;
    if (!nav_pick(rq->save ? 1 : 0, rq->title, rq->name, &sp, rq->fsel >= 0 && rq->fsel < rq->nfilt ? rq->fpat[rq->fsel] : NULL, rq->dir)) return 0;
    fss_path(&sp, out, cap);
    if (rq->save && rq->defext[0] && !strchr(strrchr(out, '/') ? strrchr(out, '/') : out, '.')) { size_t l = strlen(out); snprintf(out + l, cap - l, ".%s", rq->defext); }
    return 1;
}

/* ---- audio: Sound Manager, buffers refilled by the pump (the mixer runs on the main thread) ------------- */
#define AFR 2048
#define NB 4
static SndChannelPtr gChan;
static int16_t gAbuf[NB][AFR * 2];
static ExtSoundHeader gAhdr[NB];
static volatile int gAdone[NB];
static SndCallBackUPP gAcb;
static pascal void snd_cb(SndChannelPtr ch, SndCommand *cmd) { (void)ch; if (cmd->param2 >= 0 && cmd->param2 < NB) gAdone[cmd->param2] = 1; }
static void audio_queue(int i) {
    SndCommand c;
    ExtSoundHeader *h = &gAhdr[i];
    audio_render(gAbuf[i], AFR);
    memset(h, 0, sizeof *h);
    h->samplePtr = (Ptr)gAbuf[i]; h->numChannels = 2; h->sampleRate = rate44khz; h->encode = extSH; h->baseFrequency = 60;
    h->numFrames = AFR; h->AIFFSampleRate.exp = 0x400E; h->AIFFSampleRate.man[0] = (short)0xAC44; h->sampleSize = 16;
    c.cmd = bufferCmd; c.param1 = 0; c.param2 = (long)h; SndDoCommand(gChan, &c, true);
    c.cmd = callBackCmd; c.param1 = 0; c.param2 = i; SndDoCommand(gChan, &c, true);
}
int host_audio_open(void) {
    gAcb = NewSndCallBackUPP(snd_cb);
    if (SndNewChannel(&gChan, sampledSynth, initStereo, gAcb) != noErr) { gChan = NULL; fprintf(stderr, "[audio] no sound channel\n"); return 0; }
    for (int i = 0; i < NB; i++) audio_queue(i);
    fprintf(stderr, "[audio] output started (44100 Hz, 16-bit, stereo)\n");
    return 1;
}
static void audio_service(void) { audio_stream_service(); if (gChan) for (int i = 0; i < NB; i++) if (gAdone[i]) { gAdone[i] = 0; audio_queue(i); } }

/* ---- time, input and the pump -------------------------------------------------------------------------- */
uint32_t host_now_ms(void) {
    UnsignedWide uw;
    Microseconds(&uw);
    return (uint32_t)(((double)uw.hi * 4294967296.0 + (double)uw.lo) / 1000.0);
}
void host_sleep_ms(uint32_t ms) { os9_pump(ms); }
void host_init(const char *out) { (void)out; }
unsigned host_frame_count(void) { return 0; }
const char *host_last_frame_path(void) { return ""; }
int host_poll_events(unsigned idle) { (void)idle; return 0; }
int host_script_active(void) { return 0; }
void host_set_script(const char *p) { (void)p; }

static int vk_from_keycode(unsigned kc) {
    static const unsigned char t[128] = {
        /* 00 */ 'A','S','D','F','H','G','Z','X','C','V',0xDC,'B','Q','W','E','R',
        /* 10 */ 'Y','T','1','2','3','4','6','5',0xBB,'9','7',0xBD,'8','0',0xDD,'O',
        /* 20 */ 'U',0xDB,'I','P',0x0D,'L','J',0xDE,'K',0xBA,0xDC,0xBC,0xBF,'N','M',0xBE,
        /* 30 */ 0x09,0x20,0xC0,0x08,0,0x1B,0,0,0,0x14,0,0,0,0,0,0,
        /* 40 */ 0,0x6E,0,0x6A,0,0x6B,0,0x90,0,0,0,0x6F,0x0D,0,0x6D,0,
        /* 50 */ 0,0xBB,0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0,0x68,0x69,0,0,0,
        /* 60 */ 0x74,0x75,0x76,0x72,0x77,0x78,0,0x7A,0,0,0,0,0,0x79,0,0x7B,
        /* 70 */ 0,0x2D,0x24,0x21,0x2E,0x73,0x23,0x71,0x22,0x70,0x25,0x27,0x28,0x26,0,0};
    return kc < 128 ? t[kc] : 0;
}
static unsigned mods_of(unsigned m) {
    return ((m & shiftKey) ? HOST_MOD_SHIFT : 0u) | ((m & controlKey) ? HOST_MOD_CTRL : 0u) | ((m & optionKey) ? HOST_MOD_ALT : 0u);
}
static void game_mouse(W *w, Point where, int type, int btn, int down, unsigned mods) {
    HostEvent h;
    int ox, oy;
    content_origin(w->win, &ox, &oy);
    memset(&h, 0, sizeof h);
    h.type = type; h.win = w == &gW[0] ? 0 : w->id; h.a = btn; h.b = down; h.mods = mods;
    if (w == &gW[0]) { ox += gImgX; oy += gImgY; }
    h.x = unsc(where.h - ox, GAME_W, gImgW); h.y = unsc(where.v - oy, GAME_H, gImgH);
    if (w == &gW[0] && gFull && type == HOST_EV_MOUSEMOVE && !w->buttons && (h.x < 0 || h.y < 0 || h.x >= GAME_W || h.y >= GAME_H)) return;     /* black borders */
    host_push_event(&h);
}
static void do_mouse_down(EventRecord *e) {
    WindowPtr wp = NULL;
    short part = FindWindow(e->where, &wp);
    Dlg *d;
    W *w;
    switch (part) {
    case inMenuBar: do_menu_select(MenuSelect(e->where)); break;
    case inDrag: {
        Rect dr;
        GetRegionBounds(GetGrayRgn(), &dr);
        DragWindow(wp, e->where, &dr);
        if (wp == gW[0].win) { relayout_all(); raise_children(); prefs_save(); }
        break;
    }
    case inGrow:
        if ((d = d_of(wp)) != NULL && d->chat) {
            Rect lim;
            long sz;
            if (wp != FrontWindow()) { SelectWindow(wp); break; }
            SetRect(&lim, 220, 120, 2000, 2000);
            sz = GrowWindow(wp, e->where, &lim);
            if (sz) { SizeWindow(wp, LoWord(sz), HiWord(sz), true); chat_relayout(d, LoWord(sz), HiWord(sz)); }
        }
        break;
    case inGoAway:
        if (TrackGoAway(wp, e->where)) {
            if ((w = w_of(wp)) != NULL) push_ev_close(w == &gW[0] ? 0 : w->id);
            else if ((d = d_of(wp)) != NULL) {
                int sent = 0;
                for (int i = 0; i < MAXC && !sent; i++) if (d->c[i].used && d->c[i].id == 2) { dlg_emit(d, &d->c[i], DLG_CLICK, 0, NULL); sent = 1; }
                if (!sent) push_ev_close(d->id);
            }
        }
        break;
    case inContent:
        if ((d = d_of(wp)) != NULL) { if (wp != FrontWindow()) SelectWindow(wp); else dlg_click(d, e); break; }
        if ((w = w_of(wp)) != NULL) {
            int b = (e->modifiers & controlKey) ? 1 : 0;
            if (w == &gW[0] && wp != FrontWindow()) { SelectWindow(wp); raise_children(); }
            if (w == &gW[0] && gFull) {                         /* a click on the black borders does not go to the game */
                int ox, oy, gx, gy;
                content_origin(wp, &ox, &oy);
                gx = e->where.h - ox - gImgX; gy = e->where.v - oy - gImgY;
                if (gx < 0 || gy < 0 || gx >= gImgW || gy >= gImgH) break;
            }
            w->buttons |= 1 << b;
            game_mouse(w, e->where, HOST_EV_BUTTON, b, 1, mods_of(e->modifiers));
        }
        break;
    default: break;
    }
}
static void do_mouse_up(EventRecord *e) {
    for (int i = 0; i < MAXW; i++)
        if (gW[i].used && gW[i].buttons)
            for (int b = 0; b < 3; b++)
                if (gW[i].buttons & (1 << b)) { gW[i].buttons &= ~(1 << b); game_mouse(&gW[i], e->where, HOST_EV_BUTTON, b, 0, mods_of(e->modifiers)); }
}
static void do_key(EventRecord *e, int down, int rep) {
    int ch = (int)(e->message & charCodeMask);
    unsigned kc = (unsigned)((e->message & keyCodeMask) >> 8);
    WindowPtr fw = FrontWindow();
    Dlg *d = fw ? d_of(fw) : NULL;
    HostEvent h;
    if (down && (e->modifiers & cmdKey) && (ch == 'q' || ch == 'Q')) { push_ev_close(0); return; }
    if (down && (e->modifiers & cmdKey)) { do_menu_select(MenuKey((char)ch)); return; }
    if (d) { if (down) dlg_key(d, e); return; }
    memset(&h, 0, sizeof h);
    h.type = HOST_EV_KEY; h.a = (int)vk_from_keycode(kc); h.mods = mods_of(e->modifiers);
    if (!h.a) return;
    h.b = down; h.c = rep;
    if (down) {
        if (ch == 0x7F) ch = 8; else if (ch == 3) ch = 13;
        if (ch >= 0x20 || ch == 8 || ch == 9 || ch == 13 || ch == 27) h.ch = ((ch >= 0x1C && ch <= 0x1F) || ch >= 0x80) ? 0 : ch;
    }
    host_push_event(&h);
}
static void do_update(WindowPtr wp) {
    W *w = w_of(wp);
    Dlg *d = d_of(wp);
    GrafPtr save;
    GetPort(&save); SetPortWindowPort(wp);
    BeginUpdate(wp);
    if (w) draw_w(w);
    else if (d) dlg_draw(d);
    EndUpdate(wp);
    SetPort(save);
}
static void do_activate(WindowPtr wp, int active) {
    Dlg *d = d_of(wp);
    ControlHandle root;
    if (!d) return;
    if (GetRootControl(wp, &root) == noErr) { if (active) ActivateControl(root); else DeactivateControl(root); }
    for (int i = 0; i < MAXC; i++) if (d->c[i].used && d->c[i].list) LActivate(active, d->c[i].list);
}
static pascal OSErr ae_quit(const AppleEvent *ev, AppleEvent *reply, long ref) { (void)ev; (void)reply; (void)ref; push_ev_close(0); return noErr; }
static pascal OSErr ae_ignore(const AppleEvent *ev, AppleEvent *reply, long ref) { (void)ev; (void)reply; (void)ref; return noErr; }

static void poll_mouse_and_keys(void) {
    static Point last; static uint32_t lastw = 0xFFFFFFFEu; static unsigned lastm;
    Point g;
    WindowPtr wp = NULL;
    W *t = NULL;
    int over = 0;
    KeyMap km;
    GetGlobalMouse(&g);
    for (int i = 0; i < MAXW; i++) if (gW[i].used && gW[i].buttons) t = &gW[i];
    if (FindWindow(g, &wp) == inContent && wp) { W *w = w_of(wp); if (w) { over = 1; if (!t) t = w; } }
    if (t) {
        uint32_t id = t == &gW[0] ? 0 : t->id;
        if (g.h != last.h || g.v != last.v || id != lastw) { last = g; lastw = id; game_mouse(t, g, HOST_EV_MOUSEMOVE, 0, 0, 0); }
    }
    if (over) apply_cursor();
    if (gCurHideWant && over && !gCurHidden) { HideCursor(); gCurHidden = 1; }
    else if ((!gCurHideWant || !over) && gCurHidden) { ShowCursor(); gCurHidden = 0; }
    GetKeys(km);                                                /* Shift, Control, Option, CapsLock (modifiers do not generate events) */
    {
        static const struct { unsigned k, vk; } m[] = {{0x38, 0x10}, {0x3B, 0x11}, {0x3A, 0x12}, {0x39, 0x14}};
        unsigned now = 0;
        for (int i = 0; i < 4; i++) if (((unsigned char *)km)[m[i].k >> 3] >> (m[i].k & 7) & 1) now |= 1u << i;
        for (int i = 0; i < 4; i++)
            if (((now ^ lastm) >> i) & 1) {
                HostEvent h;
                memset(&h, 0, sizeof h);
                h.type = HOST_EV_KEY; h.a = (int)m[i].vk; h.b = m[i].vk == 0x14 ? 1 : (int)((now >> i) & 1);
                host_push_event(&h);
            }
        lastm = now;
    }
}
static void handle_event(EventRecord *e) {
    switch (e->what) {
    case mouseDown: do_mouse_down(e); break;
    case mouseUp: do_mouse_up(e); break;
    case keyDown: do_key(e, 1, 0); break;
    case autoKey: do_key(e, 1, 1); break;
    case keyUp: do_key(e, 0, 0); break;
    case updateEvt: do_update((WindowPtr)e->message); break;
    case activateEvt: do_activate((WindowPtr)e->message, (e->modifiers & activeFlag) != 0); break;
    case kHighLevelEvent: AEProcessAppleEvent(e); break;
    default: break;
    }
}
/* The guest runs on a cooperative Thread Manager thread with a large stack; only the main thread calls WaitNextEvent. */
static ThreadID gGuestThread, gMainThread;
static int gInGuest;
/* Modal native UI (alerts, Navigation Services, IP prompt) only runs on the main thread; the guest yields until the result. */
typedef struct { void (*fn)(void *); void *arg; volatile int done; } MainCall;
static MainCall *gCall;
static void on_main(void (*fn)(void *), void *arg) {
    MainCall mc;
    if (!gInGuest) { fn(arg); return; }
    mc.fn = fn; mc.arg = arg; mc.done = 0;
    gCall = &mc;
    while (!mc.done) YieldToThread(gMainThread);
}
typedef struct { const char *title, *text; uint32_t flags; int r; } MbArg;
static void mb_call(void *p) { MbArg *a = (MbArg *)p; a->r = message_box_impl(a->title, a->text, a->flags); }
int host_message_box(const char *title, const char *text, uint32_t flags) { MbArg a = {title, text, flags, 0}; on_main(mb_call, &a); return a.r; }
typedef struct { char *out; int cap, r; } NetArg;
static void net_call(void *p) { NetArg *a = (NetArg *)p; a->r = net_ask_impl(a->out, a->cap); }
int host_net_ask_address(char *out, int cap) { NetArg a = {out, cap, 0}; on_main(net_call, &a); return a.r; }
typedef struct { const HostFileReq *rq; char *out; size_t cap; int r; } FdArg;
static void fd_call(void *p) { FdArg *a = (FdArg *)p; a->r = file_dialog_impl(a->rq, a->out, a->cap); }
int host_file_dialog(const HostFileReq *rq, char *out, size_t cap) { FdArg a = {rq, out, cap, 0}; on_main(fd_call, &a); return a.r; }
static void pump_main(unsigned ms) {
    EventRecord e;
    int n = 0;
    WindowPtr fw;
    audio_service();
    dpe_service_all();
    if (WaitNextEvent(everyEvent, &e, (UInt32)(ms * 60u / 1000u), NULL)) handle_event(&e);
    while (n++ < 8 && WaitNextEvent(everyEvent, &e, 0, NULL)) handle_event(&e);
    fw = FrontWindow();
    if (fw && d_of(fw)) IdleControls(fw);
    poll_mouse_and_keys();
    poll_edits();
}
void os9_pump(unsigned ms) {
    if (!gInGuest) { pump_main(ms); return; }
    {
        uint32_t t0 = host_now_ms();
        do YieldToThread(gMainThread); while (host_now_ms() - t0 < ms);
    }
}
void os9_tick(void) {
    static uint32_t last, lastflush, prev;
    uint32_t t = host_now_ms();
    if (gLogOn && prev && t - prev > 250u) fprintf(stderr, "[host] stall: %u ms without yielding (t=%u)\n", (unsigned)(t - prev), (unsigned)t);
    prev = t;
    if (t - lastflush >= 1000u) { lastflush = t; log_flush(); }
    if (t - last < 16u) return;
    last = t;
    os9_pump(0);
}
static void *guest_entry(void *p) {
    void *(*fn)(void *) = (void *(*)(void *))p;
    gInGuest = 1;
    fn(NULL);
    gInGuest = 0;
    return NULL;
}
/* Called by gpm2_start: never returns when the Thread Manager exists (the event loop stays here). */
int os9_run_guest(void *(*fn)(void *), unsigned stack_mb) {
    long attr = 0;
    OSErr err;
    if (stack_mb == 0) stack_mb = 16;
    if (Gestalt(gestaltThreadMgrAttr, &attr) == noErr && (attr & (1L << gestaltThreadMgrPresent))) {
        err = GetCurrentThread(&gMainThread);
        if (err == noErr) err = NewThread(kCooperativeThread, NewThreadEntryUPP((ThreadEntryProcPtr)guest_entry), (void *)fn,
                                          (Size)stack_mb << 20, kCreateIfNeeded, NULL, &gGuestThread);
        if (err == noErr) {
            fprintf(stderr, "[host] guest on a cooperative thread, stack %u MB\n", stack_mb);
            for (;;) {
                pump_main(0);
                if (gCall) { MainCall *c = gCall; gCall = NULL; c->fn(c->arg); c->done = 1; }      /* modal UI requested by the guest */
                YieldToThread(gGuestThread);
            }
        }
        fprintf(stderr, "[host] NewThread failed (%d); guest on the main thread\n", (int)err);
    } else fprintf(stderr, "[host] Thread Manager absent; guest on the main thread\n");
    fn(NULL);
    return 0;
}

/* ---- game folder and 1.02B exe ----------------------------------------------------------------------------- */
static int exe_in_folder(const char *dir, char *out, size_t cap) {
    DIR *d = opendir(dir);
    struct dirent *e;
    int ok = 0;
    if (!d) return 0;
    while (!ok && (e = readdir(d))) if (!strcasecmp(e->d_name, "gpm2.exe")) { path_join(out, cap, dir, e->d_name); ok = 1; }
    closedir(d);
    return ok;
}
static int is_game_folder(const char *dir) { char e[1200]; return exe_in_folder(dir, e, sizeof e) && is_exe_102b(e); }
static void alert_need_102b(const char *dir) {
    char e[1200];
    host_message_box("This app requires Grand Prix Manager 2 version 1.02B",
        exe_in_folder(dir, e, sizeof e) ? "GPM2.EXE in the chosen folder is not version 1.02B. Install the Grand Prix Manager 2 1.02B patch, then choose the game folder again."
                                        : "GPM2.EXE was not found in the chosen folder. Choose the folder of Grand Prix Manager 2 version 1.02B.", 0x10);
}
static int prefs_path(char *out, size_t cap) {
    short v; long dir;
    FSSpec sp;
    if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder, &v, &dir) != noErr) return 0;
    FSMakeFSSpec(v, dir, "\pGPM2 Prefs", &sp);                  /* fnfErr is expected: the spec stays valid */
    fss_path(&sp, out, cap);
    return 1;
}
static void prefs_save(void) {
    char pp[1100];
    FILE *f;
    int x, y;
    if (!gPrefGame[0] || !prefs_path(pp, sizeof pp)) return;
    if (gW[0].win && !gFull) { content_origin(gW[0].win, &x, &y); gPosValid = 1; gPosH = x; gPosV = y; }
    if ((f = fopen(pp, "w")) != NULL) {
        fprintf(f, "%s\n", gPrefGame);
        if (gPosValid) fprintf(f, "pos %d %d\n", gPosH, gPosV);
        fprintf(f, "scale %d\nfullmode %d\nopenfull %d\n", gScale10, gFullMode, gOpenFull);
        fclose(f);
    }
}
static int resolve_files(void) {
    static char game[1024], exe[1200], wdir[1100], pp[1100];
    FILE *f;
    game[0] = 0;
    if (prefs_path(pp, sizeof pp) && (f = fopen(pp, "r")) != NULL) {
        char ln[1100];
        if (fgets(game, sizeof game, f)) game[strcspn(game, "\r\n")] = 0;
        while (fgets(ln, sizeof ln, f)) {
            int a, b;
            if (sscanf(ln, "pos %d %d", &a, &b) == 2) { gPosValid = 1; gPosH = a; gPosV = b; }
            else if (sscanf(ln, "scale %d", &a) == 1) gPrefScale10 = a;
            else if (sscanf(ln, "fullmode %d", &a) == 1) gFullMode = a < 0 || a > 2 ? 0 : a;
            else if (sscanf(ln, "openfull %d", &a) == 1) gOpenFull = a != 0;
        }
        fclose(f);
    }
    if (!game[0] || !is_game_folder(game)) {
        FSSpec sp;
        if (!nav_pick(2, "Choose the folder that contains GPM2.EXE (Grand Prix Manager 2 version 1.02B).", "", &sp, NULL, NULL)) return 0;
        fss_path(&sp, game, sizeof game);
        if (!is_game_folder(game)) { alert_need_102b(game); return 0; }
    }
    strncpy(gPrefGame, game, sizeof gPrefGame - 1);
    if (gPrefScale10 >= 10 && gPrefScale10 <= (gFit10 < 20 ? 20 : gFit10)) gScale10 = gPrefScale10;
    prefs_save();
    gCfg.game = game;
    if (!exe_in_folder(game, exe, sizeof exe)) { alert_need_102b(game); return 0; }
    gCfg.exe = exe;
    path_strip_slash(game);
    path_join(wdir, sizeof wdir, game, "GPM2 Saves");
    mkdir(wdir, 0755);
    gCfg.wdir = wdir;
    return 1;
}

/* File > Preferences: modal, like the game's dialogs on this system; changes apply when OK is clicked */
static ControlHandle st_new(WindowPtr win, int l, int t, int r, int b, const char *text, int right) {
    Rect rc;
    ControlHandle c;
    SetRect(&rc, l, t, r, b);
    c = NewControl(win, &rc, "\p", true, 0, 0, 0, kControlStaticTextProc, 0);
    if (c) {
        SetControlData(c, kControlEntireControl, kControlStaticTextTextTag, (Size)strlen(text), (Ptr)text);
        if (right) { ControlFontStyleRec fs; memset(&fs, 0, sizeof fs); fs.flags = kControlUseJustMask; fs.just = teFlushRight; SetControlFontStyle(c, &fs); }
    }
    return c;
}
static void settings_run(void) {
    enum { DW = 450, DH = 204, MS = 121, MF = 122 };
    GrafPtr save;
    Rect r, sr;
    WindowPtr win;
    ControlHandle root, ok, cancel, ps, pf, co, ft, ch;
    MenuHandle ms = NewMenu(MS, "\p"), mf = NewMenu(MF, "\p");
    Str255 t;
    char folder[1024];
    int cur = 0, done = 0, accept = 0;
    Boolean on = true;
    GetPort(&save);
    full_rect(&sr);
    SetRect(&r, 0, 0, DW, DH);
    OffsetRect(&r, sr.left + (sr.right - sr.left - DW) / 2, sr.top + (sr.bottom - sr.top - DH) / 3);
    win = NewCWindow(NULL, &r, "\pPreferences", false, movableDBoxProc, (WindowPtr)-1L, false, 0);
    if (!win || !ms || !mf) { if (win) DisposeWindow(win); if (ms) DisposeMenu(ms); if (mf) DisposeMenu(mf); return; }
    CreateRootControl(win, &root);
    SetThemeWindowBackground(win, kThemeBrushDialogBackgroundActive, false);
    SetPortWindowPort(win);
    InsertMenu(ms, -1);
    InsertMenu(mf, -1);
    for (int i = 0; i < gNView; i++) {
        GetMenuItemText(GetMenuHandle(VIEW_SIZE), (short)(i + 1), t);
        AppendMenu(ms, "\px"); SetMenuItemText(ms, (short)(i + 1), t);
        if (gViewScales[i] == gScale10) cur = i + 1;
    }
    AppendMenu(mf, "\pPixel Perfect"); AppendMenu(mf, "\pFill Screen"); AppendMenu(mf, "\pStretch");
    snprintf(folder, sizeof folder, "%s", gPrefGame);
    st_new(win, 16, 20, 146, 36, "Window scale:", 1);
    SetRect(&r, 156, 16, 356, 36);
    ps = NewControl(win, &r, "\p", true, 0, MS, 0, kControlPopupButtonProc, 0);
    SetControlMaximum(ps, gNView); SetControlValue(ps, cur);
    st_new(win, 16, 52, 146, 68, "Full screen mode:", 1);
    SetRect(&r, 156, 48, 356, 68);
    pf = NewControl(win, &r, "\p", true, 0, MF, 0, kControlPopupButtonProc, 0);
    SetControlMaximum(pf, 3); SetControlValue(pf, gFullMode + 1);
    SetRect(&r, 156, 80, 426, 98);
    co = NewControl(win, &r, "\pOpen in full screen", true, gOpenFull, 0, 1, kControlCheckBoxProc, 0);
    st_new(win, 16, 112, 146, 128, "Game folder:", 1);
    ft = st_new(win, 156, 112, 340, 160, folder, 0);
    SetRect(&r, 350, 110, 434, 130);
    ch = NewControl(win, &r, "\pChange\311", true, 0, 0, 1, kControlPushButtonProc, 0);
    SetRect(&r, DW - 86, DH - 36, DW - 16, DH - 16);
    ok = NewControl(win, &r, "\pOK", true, 0, 0, 1, kControlPushButtonProc, 0);
    SetControlData(ok, kControlEntireControl, kControlPushButtonDefaultTag, sizeof on, &on);
    SetRect(&r, DW - 168, DH - 36, DW - 98, DH - 16);
    cancel = NewControl(win, &r, "\pCancel", true, 0, 0, 1, kControlPushButtonProc, 0);
    ShowWindow(win);
    SelectWindow(win);
    while (!done) {
        EventRecord e;
        WindowPtr wp = NULL;
        audio_service();
        if (!WaitNextEvent(everyEvent, &e, 6, NULL)) continue;
        switch (e.what) {
        case updateEvt:
            if ((WindowPtr)e.message == win) { BeginUpdate(win); DrawControls(win); EndUpdate(win); }
            else handle_event(&e);
            SetPortWindowPort(win);
            break;
        case keyDown: case autoKey: {
            int k = (int)(e.message & charCodeMask);
            if (k == 13 || k == 3) { done = 1; accept = 1; }
            else if (k == 27 || ((e.modifiers & cmdKey) && k == '.')) done = 1;
            break;
        }
        case mouseDown: {
            short part = FindWindow(e.where, &wp);
            if (wp == win && part == inDrag) { Rect dr; GetRegionBounds(GetGrayRgn(), &dr); DragWindow(win, e.where, &dr); }
            else if (wp == win && part == inContent) {
                Point lp = e.where;
                ControlHandle hit = NULL;
                GlobalToLocal(&lp);
                if (FindControl(lp, win, &hit) && hit) {
                    if (hit == ps || hit == pf) TrackControl(hit, lp, (ControlActionUPP)-1L);
                    else if (TrackControl(hit, lp, NULL)) {
                        if (hit == ok) { done = 1; accept = 1; }
                        else if (hit == cancel) done = 1;
                        else if (hit == co) SetControlValue(co, !GetControlValue(co));
                        else if (hit == ch) {
                            FSSpec sp;
                            char g[1024];
                            if (nav_pick(2, "Choose the folder that contains GPM2.EXE (Grand Prix Manager 2 version 1.02B).", "", &sp, NULL, NULL)) {
                                fss_path(&sp, g, sizeof g);
                                if (!is_game_folder(g)) alert_need_102b(g);
                                else {
                                    snprintf(folder, sizeof folder, "%s", g);
                                    SetControlData(ft, kControlEntireControl, kControlStaticTextTextTag, (Size)strlen(folder), (Ptr)folder);
                                    Draw1Control(ft);
                                }
                            }
                            SetPortWindowPort(win);
                        }
                    }
                }
            } else SysBeep(1);
            break;
        }
        default: handle_event(&e); SetPortWindowPort(win); break;
        }
    }
    {
        int vs = GetControlValue(ps), vf = GetControlValue(pf), vo = GetControlValue(co);
        SetPort(save);
        DisposeWindow(win);
        DeleteMenu(MS); DeleteMenu(MF);
        DisposeMenu(ms); DisposeMenu(mf);
        if (accept) {
            int changed = strcmp(folder, gPrefGame) != 0;
            if (vs >= 1 && vs <= gNView) gScale10 = gViewScales[vs - 1];
            if (vf >= 1 && vf <= 3) gFullMode = vf - 1;
            gOpenFull = vo != 0;
            if (changed) snprintf(gPrefGame, sizeof gPrefGame, "%s", folder);
            view_menu_checks();
            keep_main_on_screen();
            relayout_all();
            prefs_save();
            if (changed) host_message_box("Restart required", "The new game folder will be used the next time Grand Prix Manager 2 starts.", 0x40);
        }
    }
}

int main(void) {
    Rect sb;
    char lp[1100], mp[1100];
    int w, h, m1, m2;
#if !TARGET_API_MAC_CARBON
    SetApplLimit(GetApplLimit() - 2L * 1024L * 1024L); MaxApplZone(); MoreMasters(); MoreMasters();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL);
#endif
    InitCursor();
    FlushEvents(everyEvent, 0);
    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, NewAEEventHandlerUPP(ae_quit), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenApplication, NewAEEventHandlerUPP(ae_ignore), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments, NewAEEventHandlerUPP(ae_ignore), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEPrintDocuments, NewAEEventHandlerUPP(ae_ignore), 0, false);
    memset(&gCfg, 0, sizeof gCfg);
    opt_idle_exit = 0;
    {
        KeyMap km;
        GetKeys(km);
        gLogOn = (((unsigned char *)km)[0x3A >> 3] >> (0x3A & 7)) & 1; win32_slow_trace = gLogOn;     /* Option */
    }
    if (gLogOn && prefs_path(lp, sizeof lp)) {                           /* log next to the preferences: "GPM2 Prefs Log" */
        size_t i = 0, o = 0;
        strncat(lp, " Log", sizeof lp - strlen(lp) - 1);
        for (; lp[i] && o + 1 < sizeof mp; i++) { if (i == 0 && lp[0] == '/') continue; if (lp[i] == '/' && lp[i + 1] == '/') continue; mp[o++] = lp[i] == '/' ? ':' : lp[i]; }
        mp[o] = 0;
        freopen(mp, "w", stderr);
        setvbuf(stderr, NULL, _IONBF, 0);
    }
    fprintf(stderr, "[host] Mac OS 9: free stack %ld bytes, largest block %ld bytes\n", (long)StackSpace(), (long)MaxBlock());
    GetRegionBounds(GetGrayRgn(), &sb);
    w = sb.right - sb.left; h = sb.bottom - sb.top;
    m1 = (w - 20) * 10 / GAME_W; m2 = (h - 60) * 10 / GAME_H;
    gMaxScale10 = (m1 < m2 ? m1 : m2) / 5 * 5;
    if (gMaxScale10 < 10) gMaxScale10 = 10;
    gFit10 = m1 < m2 ? m1 : m2;
    if (gFit10 < 10) gFit10 = 10;
    gScale10 = gMaxScale10 >= 20 ? 20 : 10;
    build_apple_menu();
    build_view_menu();
    DrawMenuBar();
    if (MaxBlock() < 70L * 1024L * 1024L) {
        host_message_box("Not enough memory", "Grand Prix Manager 2 needs about 100 MB. Quit other applications or raise the memory of this application (Get Info) and open it again.", 0x10);
        return 1;
    }
    if (!resolve_files()) return 0;
    gFull = gOpenFull;
    img_update();
    view_menu_checks();
    fprintf(stderr, "[host] game: %s\n[host] exe: %s\n[host] writable: %s\n", gCfg.game, gCfg.exe, gCfg.wdir);
    gCfg.out = "";
    gCfg.stack_mb = 16;
    {
        pthread_t th;
        if (gpm2_start(&gCfg, &th)) { host_message_box("Could not start the game", "Open the app again holding Option to write the GPM2 Prefs Log file in the Preferences folder, then check it.", 0x10); return 2; }
    }
    return 0;
}
