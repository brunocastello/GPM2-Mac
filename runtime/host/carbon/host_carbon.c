/* Carbon backend for Mac OS X 10.4 Tiger / 10.5 Leopard, PowerPC. Implements host.h using only APIs that
 * exist on 10.4: Carbon (windows, menus, events, HIView for the dialogs), CoreGraphics (blit), Navigation
 * Services (Open/Save), CFUserNotification (MessageBox and IP prompt) and CoreAudio/AudioUnit via the Component
 * Manager. Carbon is not thread-safe: the guest thread only queues commands (cmd_push) that the main-thread timer
 * runs; MessageBox and IP block only the guest thread (CFUserNotification is thread-safe). */
#include "../../pathjoin.h"
#include "../hostname_posix.h"
#include "dlg_layout_tiger.h"
#include "../dlg_layout.h"
#include <Carbon/Carbon.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../host.h"
#include "../../gpm2run.h"
#include "../../machine.h"

#define GAME_W 640
#define GAME_H 480
#define MAXW 32
#define MAXD 8
#define MAXC 64

extern int opt_idle_exit;
extern int opt_fixed_time_set;
extern uint32_t opt_fixed_time;

static GpmConfig gCfg;
static pthread_t gThread;
static float gScale = 1;                                       /* 1 .. gMaxScale; free (window resize) */
static float gMaxScale = 2;
static int sc(int v) { return (int)floorf((float)v * gScale + 0.5f); }
/* full screen: the game image (gImgX/Y/W/H, in screen pixels) and the per-axis scale; in a normal window gSx = gSy = gScale. gFullMode: 0 Pixel Perfect, 1 Fill Screen, 2 Stretch */
static int gFull, gFullMode, gOpenFull;
static int gImgX, gImgY, gImgW = GAME_W, gImgH = GAME_H;
static float gSx = 1, gSy = 1;
static float gFitScale = 1;                                     /* largest scale that fits the usable screen area (Zoom to Fit Screen) */
static int scx(int v) { return (int)floorf((float)v * gSx + 0.5f); }
static int scy(int v) { return (int)floorf((float)v * gSy + 0.5f); }
static void img_update(void) {
    CGRect b;
    int dw, dh, W, H;
    float s, sw, sh;
    if (!gFull) {
        gSx = gSy = gScale; gImgX = gImgY = 0; gImgW = sc(GAME_W); gImgH = sc(GAME_H);
        return;
    }
    b = CGDisplayBounds(CGMainDisplayID());
    dw = (int)b.size.width; dh = (int)b.size.height;
    sw = (float)dw / GAME_W; sh = (float)dh / GAME_H;
    s = sw < sh ? sw : sh;
    if (gFullMode == 2) { W = dw; H = dh; }
    else {
        if (gFullMode == 0) s = floorf(s);
        if (s < 1) s = 1;
        W = (int)floorf((float)GAME_W * s + 0.5f); H = (int)floorf((float)GAME_H * s + 0.5f);
    }
    gSx = (float)W / GAME_W; gSy = (float)H / GAME_H;
    gImgW = W; gImgH = H;
    gImgX = (dw - W) / 2; gImgY = (dh - H) / 2;
}

static CFStringRef cfs(const char *s) { return CFStringCreateWithCString(NULL, s ? s : "", kCFStringEncodingWindowsLatin1); }
static void cfs_get(CFStringRef s, char *out, size_t cap) {
    out[0] = 0;
    if (s) CFStringGetCString(s, out, (CFIndex)cap, kCFStringEncodingWindowsLatin1);
}

/* ---- command queue guest -> main thread ---------------------------------------------------------- */
enum { C_WIN = 1, C_WIN_DESTROY, C_PRESENT, C_TITLE, C_CURSOR, C_SHOWCUR, C_MENU, C_DLG_OPEN, C_DLG_CTL, C_DLG_CLOSE, C_DLG_FRONT, C_DLG_BACK, C_DLG_CENTER, C_BEEP, C_CHAT_MENU, C_CALL };
typedef struct Cmd { int kind; uint32_t id; void *p; struct Cmd *next; } Cmd;
typedef struct { uint32_t win; int w, h; uint8_t *pix; uint8_t pal[256][4]; } Frame;
typedef struct { uint32_t dlg; HostCtl c; char **items; } CtlCmd;
typedef struct { HostMenuItem *top; int n, sys; } MenuCmd;
typedef struct { void (*fn)(void *); void *arg; volatile int done; } MainCall;
static pthread_t gMainThread;
static Cmd *qhead, *qtail;
static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;

static void cmd_push(int kind, uint32_t id, void *p) {
    Cmd *c = (Cmd *)calloc(1, sizeof *c);
    c->kind = kind; c->id = id; c->p = p;
    pthread_mutex_lock(&qmu);
    if (kind == C_PRESENT) {                                   /* only the newest frame of each window matters */
        for (Cmd *o = qhead; o; o = o->next)
            if (o->kind == C_PRESENT && o->id == id) {
                Frame *f = (Frame *)o->p; free(f->pix); free(f);
                o->p = p; free(c); pthread_mutex_unlock(&qmu); return;
            }
    }
    if (qtail) qtail->next = c; else qhead = c;
    qtail = c;
    pthread_mutex_unlock(&qmu);
}

/* All native modal UI (alerts, Navigation Services, IP prompt) runs on the main thread; the guest thread waits for the result. */
static void on_main(void (*fn)(void *), void *arg) {
    MainCall mc;
    if (pthread_equal(pthread_self(), gMainThread)) { fn(arg); return; }
    mc.fn = fn; mc.arg = arg; mc.done = 0;
    cmd_push(C_CALL, 0, &mc);
    while (!mc.done) usleep(2000);
}

/* ---- game windows -------------------------------------------------------------------------------- */
typedef struct {
    int used;
    uint32_t id;
    HostWindow hw;
    WindowRef ref;
    uint8_t *pix; int pw, ph; uint8_t pal[256][4];
    int buttons;
} W;
static W gW[MAXW];                                             /* gW[0] = main window */
static HostWindow gMainInfo;

static W *w_find(uint32_t id) {
    for (int i = 0; i < MAXW; i++) if (gW[i].used && gW[i].id == id) return &gW[i];
    return NULL;
}
static W *w_of_ref(WindowRef r) {
    for (int i = 0; i < MAXW; i++) if (gW[i].used && gW[i].ref == r) return &gW[i];
    return NULL;
}
static void save_pos(int x, int y) {
    CFNumberRef nx = CFNumberCreate(NULL, kCFNumberIntType, &x), ny = CFNumberCreate(NULL, kCFNumberIntType, &y);
    CFPreferencesSetAppValue(CFSTR("WinX"), nx, kCFPreferencesCurrentApplication);
    CFPreferencesSetAppValue(CFSTR("WinY"), ny, kCFPreferencesCurrentApplication);
    CFRelease(nx); CFRelease(ny);
}
static int load_int(CFStringRef key, int *out) {
    CFPropertyListRef v = CFPreferencesCopyAppValue(key, kCFPreferencesCurrentApplication);
    int ok = v && CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, out);
    if (v) CFRelease(v);
    return ok;
}
static void pref_int_set(CFStringRef key, int v) {
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CFPreferencesSetAppValue(key, n, kCFPreferencesCurrentApplication);
    CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
    CFRelease(n);
}
static void scale_save(void) {
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloatType, &gScale);
    CFPreferencesSetAppValue(CFSTR("ViewScale"), n, kCFPreferencesCurrentApplication);
    CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
    CFRelease(n);
}
static void draw_w(W *w) {
    CGrafPtr port; CGContextRef ctx; CGColorSpaceRef cs; CGDataProviderRef prov; CGImageRef img;
    size_t n; uint8_t *rgb;
    if (!w->ref || !w->pix || !IsWindowVisible(w->ref)) return;
    n = (size_t)w->pw * (size_t)w->ph;
    rgb = (uint8_t *)malloc(n * 4);
    for (size_t i = 0; i < n; i++) {
        const uint8_t *c = w->pal[w->pix[i]];
        rgb[4 * i] = 0; rgb[4 * i + 1] = c[0]; rgb[4 * i + 2] = c[1]; rgb[4 * i + 3] = c[2];     /* X,R,G,B (ARGB big-endian) */
    }
    cs = CGColorSpaceCreateDeviceRGB();
    prov = CGDataProviderCreateWithData(NULL, rgb, n * 4, NULL);
    img = CGImageCreate((size_t)w->pw, (size_t)w->ph, 8, 32, (size_t)w->pw * 4, cs, kCGImageAlphaNoneSkipFirst, prov, NULL, false, kCGRenderingIntentDefault);
    port = GetWindowPort(w->ref);
    if (img && QDBeginCGContext(port, &ctx) == noErr) {
        Rect b;
        int bw, bh, dx = 0, dy = 0, dw, dh;
        GetPortBounds(port, &b);
        bw = b.right - b.left; bh = b.bottom - b.top;
        dw = bw; dh = bh;
        if (w == &gW[0] && gFull) {                             /* full screen: black background and the image centered in the computed rectangle */
            CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
            CGContextFillRect(ctx, CGRectMake(0, 0, (float)bw, (float)bh));
            dx = gImgX; dy = gImgY; dw = gImgW; dh = gImgH;
        }
        CGContextSetInterpolationQuality(ctx, (dw % w->pw == 0 && dh % w->ph == 0) ? kCGInterpolationNone : kCGInterpolationHigh);   /* integer multiple: nearest neighbor; otherwise smoothed */
        CGContextDrawImage(ctx, CGRectMake((float)dx, (float)(bh - dy - dh), (float)dw, (float)dh), img);
        CGContextSynchronize(ctx);
        QDEndCGContext(port, &ctx);
        QDFlushPortBuffer(port, NULL);
    }
    if (img) CGImageRelease(img);
    CGDataProviderRelease(prov); CGColorSpaceRelease(cs);
    free(rgb);
}
static void content_origin(W *w, int *x, int *y) {
    Rect r;
    GetWindowBounds(w->ref, kWindowContentRgn, &r);
    *x = r.left; *y = r.top;
}
static void layout_w(W *w) {
    int cw = w->hw.cw > 0 ? w->hw.cw : (w->pw ? w->pw : GAME_W), ch = w->hw.ch > 0 ? w->hw.ch : (w->ph ? w->ph : GAME_H);
    Rect r;
    if (!w->ref) return;
    if (w == &gW[0]) {
        if (gFull) return;                                      /* the window covers the screen; the image is positioned in draw_w */
        GetWindowBounds(w->ref, kWindowContentRgn, &r);
        SetRect(&r, r.left, r.top, r.left + sc(cw), r.top + sc(ch));
        SetWindowBounds(w->ref, kWindowContentRgn, &r);
    } else if (gW[0].ref) {
        int ox, oy, x, y;
        content_origin(&gW[0], &ox, &oy);
        x = ox + gImgX + scx((w->hw.x + w->hw.cx) - (gMainInfo.x + gMainInfo.cx));
        y = oy + gImgY + scy((w->hw.y + w->hw.cy) - (gMainInfo.y + gMainInfo.cy));
        SetRect(&r, x, y, x + scx(cw), y + scy(ch));
        SetWindowBounds(w->ref, kWindowContentRgn, &r);
    }
}
static void relayout_all(void) {
    img_update();
    for (int i = 0; i < MAXW; i++) if (gW[i].used) { layout_w(&gW[i]); draw_w(&gW[i]); }
}

static void push_ev_close(uint32_t win) {
    HostEvent e;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_CLOSE; e.win = win;
    host_push_event(&e);
}

static OSStatus win_handler(EventHandlerCallRef next, EventRef ev, void *ud) {
    W *w = (W *)ud;
    (void)next;
    switch (GetEventKind(ev)) {
    case kEventWindowUpdate:
        BeginUpdate(w->ref);
        draw_w(w);
        EndUpdate(w->ref);
        return noErr;
    case kEventWindowBoundsChanging:
    case kEventWindowBoundsChanged: {
        UInt32 at = 0;
        Rect nb, st, ct;
        if (w != &gW[0] || !w->ref) break;
        GetEventParameter(ev, kEventParamAttributes, typeUInt32, NULL, sizeof at, NULL, &at);
        if (GetEventKind(ev) == kEventWindowBoundsChanged && (at & kWindowBoundsChangeUserDrag) && (at & kWindowBoundsChangeOriginChanged)) {
            Rect cb;                                            /* saves the position (content origin) in the preferences */
            GetWindowBounds(w->ref, kWindowContentRgn, &cb);
            save_pos(cb.left, cb.top);
        }
        if (!(at & kWindowBoundsChangeUserDrag) || !(at & kWindowBoundsChangeSizeChanged)) break;
        if (GetEventParameter(ev, kEventParamCurrentBounds, typeQDRectangle, NULL, sizeof nb, NULL, &nb) != noErr) break;
        GetWindowBounds(w->ref, kWindowStructureRgn, &st);
        GetWindowBounds(w->ref, kWindowContentRgn, &ct);
        if (GetEventKind(ev) == kEventWindowBoundsChanging) {   /* 4:3 aspect ratio locked (the content, not the frame) */
            int dx = (st.right - st.left) - (ct.right - ct.left), dy = (st.bottom - st.top) - (ct.bottom - ct.top);
            int cw = (nb.right - nb.left) - dx;
            if (cw < 320) cw = 320;
            nb.right = (short)(nb.left + dx + cw);
            nb.bottom = (short)(nb.top + dy + (cw * GAME_H + GAME_W / 2) / GAME_W);
            SetEventParameter(ev, kEventParamCurrentBounds, typeQDRectangle, sizeof nb, &nb);
        } else {                                                /* the scale follows the width; the other windows follow */
            gScale = (float)(ct.right - ct.left) / (float)GAME_W;
            scale_save();
            img_update();
            for (int i = 1; i < MAXW; i++) if (gW[i].used) { layout_w(&gW[i]); draw_w(&gW[i]); }
            draw_w(w);
        }
        return noErr;
    }
    case kEventWindowClose:
        push_ev_close(w == &gW[0] ? 0 : w->id);
        return noErr;
    }
    return eventNotHandledErr;
}
/* creates the window's WindowRef and only sets w->ref on success (set_fullscreen swaps the main window and needs the old one intact) */
static int w_make_ref(W *w, const HostWindow *hw, int main) {
    static const EventTypeSpec ts[] = {{kEventClassWindow, kEventWindowUpdate}, {kEventClassWindow, kEventWindowClose},
                                       {kEventClassWindow, kEventWindowBoundsChanging}, {kEventClassWindow, kEventWindowBoundsChanged}};
    Rect r;
    WindowAttributes at = kWindowStandardHandlerAttribute | kWindowCloseBoxAttribute;
    WindowClass cl;
    WindowRef ref = NULL;
    CFStringRef t;
    int full = main && gFull;
    if (full) { cl = kPlainWindowClass; at = kWindowStandardHandlerAttribute; }
    else if (main) { cl = kDocumentWindowClass; at |= kWindowCollapseBoxAttribute | kWindowResizableAttribute | kWindowLiveResizeAttribute; }
    else {
        cl = hw && hw->caption ? kFloatingWindowClass : kPlainWindowClass;
        at = kWindowStandardHandlerAttribute | kWindowNoActivatesAttribute | (hw && hw->caption && hw->sysmenu ? kWindowCloseBoxAttribute : 0);
    }
    if (full) {
        CGRect b = CGDisplayBounds(CGMainDisplayID());
        SetRect(&r, (short)b.origin.x, (short)b.origin.y, (short)(b.origin.x + b.size.width), (short)(b.origin.y + b.size.height));
    } else SetRect(&r, 40, 60, 40 + sc(GAME_W), 60 + sc(GAME_H));
    if (CreateNewWindow(cl, at, &r, &ref) != noErr) return 0;
    InstallWindowEventHandler(ref, NewEventHandlerUPP(win_handler), 4, ts, w, NULL);
    if (full) { RGBColor black = {0, 0, 0}; SetWindowContentColor(ref, &black); }
    else if (main) { HISize mn = {320, 240}; SetWindowResizeLimits(ref, &mn, NULL); }   /* minimum 320x240, aspect ratio locked in win_handler */
    t = cfs(hw && hw->title[0] ? hw->title : "Grand Prix Manager 2");
    SetWindowTitleWithCFString(ref, t);
    CFRelease(t);
    if (main && !full) {
        int x, y;
        if (load_int(CFSTR("WinX"), &x) && load_int(CFSTR("WinY"), &y)) {      /* saved position, kept on the visible screen */
            MoveWindow(ref, (short)x, (short)y, false);
            ConstrainWindowToScreen(ref, kWindowStructureRgn, kWindowConstrainStandardOptions, NULL, NULL);
        } else RepositionWindow(ref, NULL, kWindowCascadeOnMainScreen);
    }
    w->ref = ref;
    return 1;
}
static W *w_create(uint32_t id, const HostWindow *hw, int main) {
    W *w = NULL;
    for (int i = main ? 0 : 1; i < MAXW; i++) if (!gW[i].used) { w = &gW[i]; break; }
    if (!w) return NULL;
    memset(w, 0, sizeof *w);
    w->used = 1; w->id = id;
    if (hw) w->hw = *hw;
    if (!w_make_ref(w, hw, main)) { w->used = 0; return NULL; }
    return w;
}

static void do_win(const HostWindow *hw) {
    W *w = NULL;
    if (hw->main) {
        CFStringRef t = cfs(hw->title);
        gMainInfo = *hw;
        w = &gW[0];
        w->id = hw->id; w->hw = *hw;
        if (w->ref && hw->title[0]) SetWindowTitleWithCFString(w->ref, t);
        CFRelease(t);
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
    if (!hw->visible || hw->cw <= 0 || hw->ch <= 0) { HideWindow(w->ref); return; }
    layout_w(w);
    if (!IsWindowVisible(w->ref)) ShowWindow(w->ref);
    draw_w(w);
}
static void do_win_destroy(uint32_t id) {
    W *w = w_find(id);
    if (!w || w == &gW[0]) return;
    if (w->ref) DisposeWindow(w->ref);
    free(w->pix);
    memset(w, 0, sizeof *w);
}
static void do_present(Frame *f) {
    W *w = w_find(f->win);
    if (!w) w = &gW[0];
    free(w->pix);
    w->pix = f->pix; w->pw = f->w; w->ph = f->h;
    memcpy(w->pal, f->pal, sizeof w->pal);
    free(f);
    draw_w(w);
}

/* ---- menus ----------------------------------------------------------------------------------------- */
#define CMD_BASE 0x47000000u                                    /* 'G' + index in the table */
#define CMD_SCALE  0x53430000u                                  /* 'SC' + scale x10 (10 = 1x, 15 = 1.5x, 20 = 2x ...) */
typedef struct { uint32_t id; int sys; } McEnt;
static McEnt gMc[2048];
static int gNMc;
static MenuID gMenuIds[64];
static int gNMenus;
static int gNextMenuId = 1000;

static MenuRef build_menu(const HostMenuItem *it, int sys, const char *title) {
    MenuRef m;
    CFStringRef t;
    if (gNextMenuId > 30000) gNextMenuId = 1000;
    CreateNewMenu((MenuID)gNextMenuId++, 0, &m);
    t = cfs(title);
    SetMenuTitleWithCFString(m, t);
    CFRelease(t);
    for (int i = 0; i < it->nchild; i++) {
        const HostMenuItem *c = &it->child[i];
        MenuItemIndex idx;
        if (c->separator) { AppendMenuItemTextWithCFString(m, CFSTR(""), kMenuItemAttrSeparator, 0, &idx); continue; }
        t = cfs(c->text);
        if (c->nchild > 0) {
            MenuRef sub = build_menu(c, sys, c->text);
            AppendMenuItemTextWithCFString(m, t, 0, 0, &idx);
            SetMenuItemHierarchicalMenu(m, idx, sub);
        } else {
            uint32_t cid = 0;
            if (gNMc < 2048) { gMc[gNMc].id = c->id; gMc[gNMc].sys = sys; cid = CMD_BASE + (uint32_t)gNMc++; }
            AppendMenuItemTextWithCFString(m, t, c->grayed ? kMenuItemAttrDisabled : 0, cid, &idx);
            if (c->checked) CheckMenuItem(m, idx, true);
        }
        CFRelease(t);
    }
    return m;
}
static void free_menu(HostMenuItem *it, int n) {
    for (int i = 0; i < n; i++) free_menu(it[i].child, it[i].nchild);
    free(it);
}
#define CMD_FULL   0x46530000u                                  /* 'FS' + 0: Enter/Exit Full Screen */
#define CMD_FIT    0x46530001u                                  /* Zoom to Fit Screen */
#define CMD_CHAT   0x46530020u                                  /* menu Chat: +0 Show Chat, +1 Chat Settings */
static MenuRef gChatM;                                          /* gray until Link Play is established */
static int gChatOn;
#define CMD_FMODE  0x46530010u                                  /* Full Screen Mode: +0 Pixel Perfect, +1 Fill Screen, +2 Stretch */
static MenuRef gViewM, gWszM, gFmodeM;                          /* View is recreated on every do_menu */
static MenuItemIndex gFullIdx;
static MenuID gHelpMid;                                         /* system Help menu: the game menus go before it */
static int gWszT10[16], gNWsz;                                  /* scales x10 offered (Window Size menu and Preferences window) */
static void wsz_fill(void) {
    gNWsz = 0;
    for (int t10 = 10; t10 <= (gMaxScale < 2 ? 20 : (int)(gMaxScale * 10 + 0.001f)) && gNWsz < 16; t10 += (t10 <= 15 ? 5 : 10)) gWszT10[gNWsz++] = t10;   /* 1x, 1.5x and 2x always; larger ones only if they fit */
}
static void wsz_title(int t10, char *b, size_t n) {
    if (t10 == 10) snprintf(b, n, "Actual Size (1x)");
    else if (t10 % 10) snprintf(b, n, "%d.%dx", t10 / 10, t10 % 10);
    else snprintf(b, n, "%dx", t10 / 10);
}
static void view_refresh(void) {
    if (gWszM) for (int i = 0; i < gNWsz; i++) CheckMenuItem(gWszM, (MenuItemIndex)(i + 1), !gFull && fabsf(gScale - (float)gWszT10[i] / 10.0f) < 0.01f);
    if (gFmodeM) for (int i = 0; i < 3; i++) CheckMenuItem(gFmodeM, (MenuItemIndex)(i + 1), gFullMode == i);
    if (gViewM && gFullIdx) SetMenuItemTextWithCFString(gViewM, gFullIdx, gFull ? CFSTR("Exit Full Screen") : CFSTR("Enter Full Screen"));
}
static void do_menu(MenuCmd *mc) {
    MenuRef m;
    for (int i = 0; i < gNMenus; i++) DeleteMenu(gMenuIds[i]);
    gNMenus = 0; gNMc = 0;
    for (int i = 0; i < mc->n && gNMenus < 62; i++) {
        m = build_menu(&mc->top[i], mc->sys, mc->top[i].text);
        InsertMenu(m, gHelpMid);
        gMenuIds[gNMenus++] = GetMenuID(m);
    }
    CreateNewMenu((MenuID)gNextMenuId++, 0, &m);
    SetMenuTitleWithCFString(m, CFSTR("View"));
    gViewM = m;
    {   /* View > Window Size: 1x, 1.5x, 2x, 3x ... those that fit on the screen (same as the modern app) */
        MenuItemIndex si;
        CreateNewMenu((MenuID)gNextMenuId++, 0, &gWszM);
        SetMenuTitleWithCFString(gWszM, CFSTR("Window Size"));
        wsz_fill();
        for (int i = 0; i < gNWsz; i++) {
            int t10 = gWszT10[i];
            char b[32];
            CFStringRef ts;
            MenuItemIndex ix;
            wsz_title(t10, b, sizeof b);
            ts = cfs(b);
            AppendMenuItemTextWithCFString(gWszM, ts, 0, CMD_SCALE + (uint32_t)t10, &ix);
            CFRelease(ts);
            if (t10 % 10 == 0 && t10 <= 40) SetMenuItemCommandKey(gWszM, ix, false, (UniChar)('0' + t10 / 10));
        }
        AppendMenuItemTextWithCFString(m, CFSTR("Window Size"), 0, 0, &si);
        SetMenuItemHierarchicalMenu(m, si, gWszM);
    }
    {
        static const char *const fmn[3] = {"Pixel Perfect", "Fill Screen", "Stretch"};
        MenuItemIndex zi, fi;
        AppendMenuItemTextWithCFString(m, CFSTR("Zoom to Fit Screen"), 0, CMD_FIT, &zi);
        SetMenuItemCommandKey(m, zi, false, '0');
        AppendMenuItemTextWithCFString(m, CFSTR(""), kMenuItemAttrSeparator, 0, NULL);
        AppendMenuItemTextWithCFString(m, CFSTR("Enter Full Screen"), 0, CMD_FULL, &gFullIdx);
        SetMenuItemCommandKey(m, gFullIdx, false, 'F');
        SetMenuItemModifiers(m, gFullIdx, kMenuControlModifier);
        CreateNewMenu((MenuID)gNextMenuId++, 0, &gFmodeM);
        SetMenuTitleWithCFString(gFmodeM, CFSTR("Full Screen Mode"));
        for (int i = 0; i < 3; i++) {
            CFStringRef ts = cfs(fmn[i]);
            AppendMenuItemTextWithCFString(gFmodeM, ts, 0, CMD_FMODE + (uint32_t)i, NULL);
            CFRelease(ts);
        }
        AppendMenuItemTextWithCFString(m, CFSTR("Full Screen Mode"), 0, 0, &fi);
        SetMenuItemHierarchicalMenu(m, fi, gFmodeM);
    }
    view_refresh();
    InsertMenu(m, gHelpMid);
    gMenuIds[gNMenus++] = GetMenuID(m);
    CreateNewMenu((MenuID)gNextMenuId++, 0, &gChatM);
    SetMenuTitleWithCFString(gChatM, CFSTR("Chat"));
    AppendMenuItemTextWithCFString(gChatM, CFSTR("Show Chat"), 0, CMD_CHAT, NULL);
    {
        CFStringRef ts = CFStringCreateWithCString(NULL, "Chat Settings\xE2\x80\xA6", kCFStringEncodingUTF8);
        AppendMenuItemTextWithCFString(gChatM, ts, 0, CMD_CHAT + 1, NULL);
        CFRelease(ts);
    }
    if (!gChatOn) DisableMenuItem(gChatM, 0);
    InsertMenu(gChatM, gHelpMid);
    gMenuIds[gNMenus++] = GetMenuID(gChatM);
    DrawMenuBar();
    free_menu(mc->top, mc->n);
    free(mc);
}

/* ---- native dialogs (HIView) --------------------------------------------------------------------- */
typedef struct {
    int used;
    uint32_t hwnd, id;
    int kind;
    ControlRef ref;
    char **items; int nitems;
    char text[256];                                            /* last EDIT text exchanged with the core */
    int sel;
    int colw;
    MenuRef menu;
    ControlRef frame;                                          /* list: own border, which does not vanish with an inactive window */
} Ctl;
typedef struct {
    int used;
    uint32_t id;
    WindowRef ref;
    int modal;
    Ctl c[MAXC];
    int chat;
    int cw, ch;
    int ox, oy;                /* offset to the margin (template dialogs) */
    int hig;                   /* own layout (Link Play): font and normal-size controls */
    char tmpl[64];
} Dlg;
static Dlg gD[MAXD];

static Dlg *d_find(uint32_t id) {
    for (int i = 0; i < MAXD; i++) if (gD[i].used && gD[i].id == id) return &gD[i];
    return NULL;
}
static Dlg *d_of_ref(WindowRef r) {
    for (int i = 0; i < MAXD; i++) if (gD[i].used && gD[i].ref == r) return &gD[i];
    return NULL;
}
static void dlg_emit(Dlg *d, Ctl *c, int code, int idx, const char *text) {
    HostEvent e;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_DLG; e.win = d->id; e.a = (int)c->id; e.b = code; e.c = idx;
    if (text) snprintf(e.text, sizeof e.text, "%s", text);
    host_push_event(&e);
}
/* DataBrowser without its own frame (it vanishes with an inactive window); this panel behind the list draws the border */
static OSStatus list_frame_draw(EventHandlerCallRef next, EventRef ev, void *ud) {
    CGContextRef g = NULL;
    HIRect b;
    (void)next; (void)ud;
    if (GetEventParameter(ev, kEventParamCGContextRef, typeCGContextRef, NULL, sizeof g, NULL, &g) != noErr || !g) return eventNotHandledErr;
    HIViewGetBounds((HIViewRef)ud, &b);
    CGContextSetRGBStrokeColor(g, 0.56f, 0.56f, 0.56f, 1.0f);
    CGContextSetLineWidth(g, 1.0f);
    CGContextStrokeRect(g, CGRectInset(b, 0.5f, 0.5f));
    return noErr;
}
static void list_frame_add(Dlg *d, Ctl *c) {
    static const EventTypeSpec dr[] = {{kEventClassControl, kEventControlDraw}};
    Rect r;
    Boolean no = false;
    SetRect(&r, 0, 0, 10, 10);
    SetControlData(c->ref, kControlEntireControl, 'brdr', sizeof no, &no);   /* kControlDataBrowserIncludesFrameAndFocusTag */
    if (CreateUserPaneControl(d->ref, &r, 0, &c->frame) != noErr) { c->frame = NULL; return; }
    InstallControlEventHandler(c->frame, NewEventHandlerUPP(list_frame_draw), 1, dr, c->frame, NULL);
    HIViewSetZOrder(c->frame, kHIViewZOrderBelow, c->ref);
}
static void list_frame_place(Ctl *c, const HIRect *list) {
    if (c->frame) { HIRect f = CGRectInset(*list, -1.0f, -1.0f); HIViewSetFrame(c->frame, &f); }
}

/* resized chat: the grid is the same as in do_dlg_ctl, with the new size */
static void chat_relayout(Dlg *d) {
    Rect cr;
    GetWindowBounds(d->ref, kWindowContentRgn, &cr);
    if (cr.right - cr.left == d->cw && cr.bottom - cr.top == d->ch) return;
    d->cw = cr.right - cr.left; d->ch = cr.bottom - cr.top;
    for (int i = 0; i < MAXC; i++) {
        Ctl *c = &d->c[i];
        HostCtl h;
        HIRect fr;
        if (!c->used || !c->ref) continue;
        memset(&h, 0, sizeof h);
        h.kind = c->kind;
        dlg_layout_ctl(d->tmpl, 1, d->cw, d->ch, &h);
        if (h.kind == HC_EDIT) { h.x += 3; h.y += 3; h.w -= 6; h.h -= 6; }
        if (h.kind == HC_LIST) { h.x += 1; h.y += 1; h.w -= 2; h.h -= 2; }
        fr = CGRectMake((float)h.x, (float)h.y, (float)h.w, (float)h.h);
        HIViewSetFrame(c->ref, &fr);
        list_frame_place(c, &fr);
        if (h.kind == HC_LIST) {
            c->colw = h.w;
            SetDataBrowserTableViewNamedColumnWidth(c->ref, 'TEXT', (UInt16)(h.w > 16 ? h.w - 16 : h.w));
        }
    }
}
static OSStatus dlg_win_handler(EventHandlerCallRef next, EventRef ev, void *ud) {
    Dlg *d = (Dlg *)ud;
    (void)next;
    if (GetEventKind(ev) == kEventWindowBoundsChanged) {
        if (d->chat) chat_relayout(d);
        return eventNotHandledErr;
    }
    if (GetEventKind(ev) == kEventWindowClose) {
        for (int i = 0; i < MAXC; i++) if (d->c[i].used && d->c[i].id == 2) { dlg_emit(d, &d->c[i], DLG_CLICK, 0, NULL); return noErr; }
        push_ev_close(d->id);
        return noErr;
    }
    return eventNotHandledErr;
}
static OSStatus ctl_hit(EventHandlerCallRef next, EventRef ev, void *ud) {
    Ctl *c = (Ctl *)ud;
    Dlg *d;
    (void)next; (void)ev;
    if (!c->ref) return eventNotHandledErr;
    d = d_of_ref(GetControlOwner(c->ref));
    if (!d) return eventNotHandledErr;
    if (c->menu) dlg_emit(d, c, DLG_SEL, (int)GetControl32BitValue(c->ref) - 1, NULL);
    else dlg_emit(d, c, DLG_CLICK, 0, NULL);
    return eventNotHandledErr;                                 /* the control still needs to handle the click */
}
static OSStatus list_item_data(ControlRef br, DataBrowserItemID item, DataBrowserPropertyID prop, DataBrowserItemDataRef data, Boolean set) {
    Dlg *d = d_of_ref(GetControlOwner(br));
    (void)set;
    if (prop != 'TEXT') return errDataBrowserPropertyNotSupported;   /* answering noErr to everything left the row layout wrong */
    if (!d) return noErr;
    for (int i = 0; i < MAXC; i++)
        if (d->c[i].used && d->c[i].ref == br && item >= 1 && (int)item <= d->c[i].nitems) {
            CFStringRef s = cfs(d->c[i].items[item - 1]);
            SetDataBrowserItemDataText(data, s);
            CFRelease(s);
        }
    return noErr;
}
static void list_notify(ControlRef br, DataBrowserItemID item, DataBrowserItemNotification msg) {
    Dlg *d = d_of_ref(GetControlOwner(br));
    if (!d) return;
    for (int i = 0; i < MAXC; i++)
        if (d->c[i].used && d->c[i].ref == br) {
            if (msg == kDataBrowserItemSelected && (int)item - 1 != d->c[i].sel) { d->c[i].sel = (int)item - 1; dlg_emit(d, &d->c[i], DLG_SEL, (int)item - 1, NULL); }
            else if (msg == kDataBrowserItemDoubleClicked) dlg_emit(d, &d->c[i], DLG_DBLCLK, (int)item - 1, NULL);
        }
}
static void do_dlg_open(HostDlg *h) {
    static const EventTypeSpec ts[] = {{kEventClassWindow, kEventWindowClose}, {kEventClassWindow, kEventWindowBoundsChanged}};
    Dlg *d = NULL;
    Rect r;
    CFStringRef t;
    for (int i = 0; i < MAXD; i++) if (!gD[i].used) { d = &gD[i]; break; }
    if (!d) { free(h); return; }
    memset(d, 0, sizeof *d);
    if (h->mw && !dlg_layout_has(h->name, h->chat)) { h->w = h->mw; h->h = h->mh; d->ox = h->ox; d->oy = h->oy; }
    dlg_layout_size(h->name, &h->w, &h->h);
    d->used = 1; d->id = h->id; d->modal = h->modal; d->chat = h->chat; d->cw = h->w; d->ch = h->h;
    d->hig = dlg_layout_has(h->name, h->chat);
    snprintf(d->tmpl, sizeof d->tmpl, "%s", h->name);
    SetRect(&r, 100, 100, 100 + h->w, 100 + h->h);
    if (CreateNewWindow(h->modal ? kMovableModalWindowClass : kDocumentWindowClass,
                        kWindowCompositingAttribute | kWindowStandardHandlerAttribute | (h->modal ? 0 : kWindowCloseBoxAttribute) | (h->chat ? kWindowResizableAttribute | kWindowLiveResizeAttribute : 0), &r, &d->ref) != noErr) {
        d->used = 0; free(h); return;
    }
    InstallWindowEventHandler(d->ref, NewEventHandlerUPP(dlg_win_handler), 2, ts, d, NULL);
    if (h->chat) { HISize mn = CGSizeMake(220, 120); SetWindowResizeLimits(d->ref, &mn, NULL); }
    t = cfs(h->title);
    SetWindowTitleWithCFString(d->ref, t);
    CFRelease(t);
    if (h->chat && gW[0].ref) {
        Rect b;
        GetWindowBounds(gW[0].ref, kWindowContentRgn, &b);
        MoveWindow(d->ref, b.left + 10, b.top + 30, false);
    } else if (gW[0].ref) RepositionWindow(d->ref, gW[0].ref, kWindowCenterOnParentWindow); else RepositionWindow(d->ref, NULL, kWindowCenterOnMainScreen);
    ShowWindow(d->ref);
    SelectWindow(d->ref);
    free(h);
}
static int items_same(const Ctl *c, const CtlCmd *cc) {
    if (c->nitems != cc->c.nitems) return 0;
    for (int i = 0; i < c->nitems; i++) if (strcmp(c->items[i], cc->items[i]) != 0) return 0;
    return 1;
}
static void items_take(Ctl *c, CtlCmd *cc) {
    for (int i = 0; i < c->nitems; i++) free(c->items[i]);
    free(c->items);
    c->items = cc->items; c->nitems = cc->c.nitems; cc->items = NULL;
}
static void do_dlg_ctl(CtlCmd *cc) {
    static const EventTypeSpec hit[] = {{kEventClassControl, kEventControlHit}};
    Dlg *d = d_find(cc->dlg);
    HostCtl *h = &cc->c;
    Ctl *c = NULL;
    Rect r;
    CFStringRef t;
    if (!d) goto done;
    if (!d->hig) { h->x += d->ox; h->y += d->oy; }
    dlg_layout_ctl(d->tmpl, d->chat, d->cw, d->ch, h);
    if (d->hig && h->kind == HC_EDIT) { h->x += 3; h->y += 3; h->w -= 6; h->h -= 6; }   /* the Carbon field draws the frame and focus outside the frame */
    if (d->hig && h->kind == HC_LIST) { h->x += 1; h->y += 1; h->w -= 2; h->h -= 2; }   /* the list_frame_add border sits outside */
    for (int i = 0; i < MAXC; i++) if (d->c[i].used && d->c[i].hwnd == h->hwnd) { c = &d->c[i]; break; }
    if (!c) {
        for (int i = 0; i < MAXC; i++) if (!d->c[i].used) { c = &d->c[i]; break; }
        if (!c) goto done;
        memset(c, 0, sizeof *c);
        c->used = 1; c->hwnd = h->hwnd; c->id = h->id; c->kind = h->kind; c->sel = -1;
        SetRect(&r, h->x, h->y, h->x + h->w, h->y + h->h);
        t = cfs(h->text);
        switch (h->kind) {
        case HC_PUSH: case HC_DEFPUSH: CreatePushButtonControl(d->ref, &r, t, &c->ref); break;
        case HC_CHECK: CreateCheckBoxControl(d->ref, &r, t, 0, true, &c->ref); break;
        case HC_RADIO: CreateRadioButtonControl(d->ref, &r, t, 0, true, &c->ref); break;
        case HC_GROUP:
            CreateGroupBoxControl(d->ref, &r, t, true, &c->ref);
            if (c->ref) HIViewSetZOrder(c->ref, kHIViewZOrderBelow, NULL);   /* in the template the frame comes after the controls: it must not cover them or steal clicks */
            break;
        case HC_EDIT:
            CreateEditUnicodeTextControl(d->ref, &r, t, h->password != 0, NULL, &c->ref);
            if (d->chat && c->ref) SetKeyboardFocus(d->ref, c->ref, kControlFocusNextPart);
            break;
        case HC_STATIC: CreateStaticTextControl(d->ref, &r, t, NULL, &c->ref); break;
        case HC_LIST: {
            DataBrowserCallbacks cb;
            DataBrowserListViewColumnDesc col;
            CreateDataBrowserControl(d->ref, &r, kDataBrowserListView, &c->ref);
            memset(&cb, 0, sizeof cb);
            cb.version = kDataBrowserLatestCallbacks;
            InitDataBrowserCallbacks(&cb);
            cb.u.v1.itemDataCallback = NewDataBrowserItemDataUPP(list_item_data);
            cb.u.v1.itemNotificationCallback = NewDataBrowserItemNotificationUPP(list_notify);
            SetDataBrowserCallbacks(c->ref, &cb);
            memset(&col, 0, sizeof col);
            col.propertyDesc.propertyID = 'TEXT'; col.propertyDesc.propertyType = kDataBrowserTextType;
            col.propertyDesc.propertyFlags = kDataBrowserDoNotTruncateText | (d->chat ? 0 : kDataBrowserListViewSelectionColumn);   /* without the selection column a click does not select the row */
            col.headerBtnDesc.version = kDataBrowserListViewLatestHeaderDesc;
            col.headerBtnDesc.minimumWidth = 10; col.headerBtnDesc.maximumWidth = 2000;
            col.headerBtnDesc.btnFontStyle.just = teFlushDefault;
            AddDataBrowserListViewColumn(c->ref, &col, 0);
            SetDataBrowserListViewHeaderBtnHeight(c->ref, 0);
            SetDataBrowserHasScrollBars(c->ref, false, true);
            if (d->hig) SetDataBrowserTableViewHiliteStyle(c->ref, kDataBrowserTableViewFillHilite);   /* selection on the whole row, not just the text */
            SetDataBrowserSelectionFlags(c->ref, h->multisel ? kDataBrowserCmdTogglesSelection : kDataBrowserSelectOnlyOne);
            break;
        }
        case HC_COMBO_LIST: case HC_COMBO_DROP: {                /* CBS_DROPDOWNLIST and CBS_DROPDOWN become popups */
            MenuRef m;
            CreateNewMenu((MenuID)gNextMenuId++, 0, &m);
            c->menu = m;
            InsertMenu(m, kInsertHierarchicalMenu);              /* the control finds the menu by ID: outside the menu list there is no popup */
            if (CreatePopupButtonControl(d->ref, &r, t, GetMenuID(m), false, 0, 0, 0, &c->ref) != noErr) c->ref = NULL;
            break;
        }
        default: break;
        }
        CFRelease(t);
        if (c->ref) {
            InstallControlEventHandler(c->ref, NewEventHandlerUPP(ctl_hit), 1, hit, c, NULL);
            {                                                   /* small font, as in the modern app: the layout is MS Sans Serif 8 */
                ControlFontStyleRec fs;
                memset(&fs, 0, sizeof fs);
                fs.flags = kControlUseThemeFontIDMask; fs.font = d->hig ? kThemeSystemFont : kThemeSmallSystemFont;
                if (d->hig && h->kind == HC_STATIC && h->align == 2) { fs.flags |= kControlUseJustMask; fs.just = teFlushRight; }
                SetControlFontStyle(c->ref, &fs);
                if (h->kind == HC_LIST) SetDataBrowserTableViewRowHeight(c->ref, d->hig ? 17 : 14);   /* after the font: without this the row is too tall and ends up out of place */
                if (!d->hig && (h->kind == HC_PUSH || h->kind == HC_DEFPUSH || h->kind == HC_CHECK || h->kind == HC_RADIO || h->kind == HC_COMBO_LIST || h->kind == HC_COMBO_DROP)) {
                    ControlSize cs = kControlSizeSmall;
                    SetControlData(c->ref, kControlEntireControl, kControlSizeTag, sizeof cs, &cs);
                }
            }
            if (d->hig && h->kind == HC_LIST) list_frame_add(d, c);
            if (h->kind == HC_DEFPUSH) SetWindowDefaultButton(d->ref, c->ref);
            if (h->kind == HC_PUSH && h->id == 2) SetWindowCancelButton(d->ref, c->ref);
        }
    }
    if (!c->ref) goto done;
    {
        int fh = (h->kind == HC_COMBO_LIST || h->kind == HC_COMBO_DROP) && h->h > 20 ? 20 : h->h;   /* the template height includes the open list */
        int fw = h->w;
        HIRect fr, old;
        if (h->kind == HC_PUSH || h->kind == HC_DEFPUSH || h->kind == HC_CHECK || h->kind == HC_RADIO || (h->kind == HC_STATIC && h->h <= 20 && h->align == 0)) {   /* the layout is MS Sans Serif: the native text must not be clipped */
            Point sz = {0, 0};
            SInt16 base = 0;
            int need;
            t = cfs(h->text);
            GetThemeTextDimensions(t, d->hig ? kThemeSystemFont : kThemeSmallSystemFont, kThemeStateActive, false, &sz, &base);
            CFRelease(t);
            need = sz.h + (h->kind == HC_STATIC ? 4 : 24);
            if (need > fw) fw = need;
        }
        fr = CGRectMake((float)h->x, (float)h->y, (float)fw, (float)fh);
        HIViewGetFrame(c->ref, &old);
        if (!CGRectEqualToRect(old, fr)) HIViewSetFrame(c->ref, &fr);
        list_frame_place(c, &fr);
        if (h->visible) {                                      /* a control that grew does not leave the window; margin at the edges */
            Rect cr;
            int cw, ch, nw, nh;
            GetWindowBounds(d->ref, kWindowContentRgn, &cr);
            cw = cr.right - cr.left; ch = cr.bottom - cr.top;
            nw = h->x + fw + HOST_DLG_MARGIN > cw ? h->x + fw + HOST_DLG_MARGIN : cw;
            nh = h->y + fh + HOST_DLG_MARGIN > ch ? h->y + fh + HOST_DLG_MARGIN : ch;
            if (nw != cw || nh != ch) SizeWindow(d->ref, (short)nw, (short)nh, true);
        }
        if (h->kind == HC_LIST && h->w != c->colw) {           /* the chat creates the list at 0x0 and only then positions it (WM_SIZE): a zero-width column shows nothing */
            c->colw = h->w;
            SetDataBrowserTableViewNamedColumnWidth(c->ref, 'TEXT', (UInt16)(h->w > 16 ? h->w - 16 : h->w));
            SetDataBrowserScrollPosition(c->ref, 0, 0);
        }
    }
    HIViewSetVisible(c->ref, h->visible != 0);
    if (c->frame) HIViewSetVisible(c->frame, h->visible != 0);
    if (h->enabled || (d->chat && h->kind == HC_LIST)) EnableControl(c->ref); else DisableControl(c->ref);   /* a disabled DataBrowser appears blank; the modal already blocks the chat */
    switch (h->kind) {
    case HC_PUSH: case HC_DEFPUSH: case HC_CHECK: case HC_RADIO: case HC_GROUP:
        t = cfs(h->text); SetControlTitleWithCFString(c->ref, t); CFRelease(t);
        if (h->kind == HC_CHECK || h->kind == HC_RADIO) SetControl32BitValue(c->ref, h->checked ? 1 : 0);
        break;
    case HC_STATIC:
        t = cfs(h->text); SetControlData(c->ref, kControlEntireControl, kControlStaticTextCFStringTag, sizeof t, &t); CFRelease(t);
        break;
    case HC_EDIT:
        if (strcmp(c->text, h->text) != 0) {                   /* only what the core changed (what the user typed is already there) */
            t = cfs(h->text);
            SetControlData(c->ref, kControlEntireControl, kControlEditTextCFStringTag, sizeof t, &t);
            CFRelease(t);
            snprintf(c->text, sizeof c->text, "%s", h->text);
        }
        break;
    case HC_LIST:
        if (!items_same(c, cc)) {
            RemoveDataBrowserItems(c->ref, kDataBrowserNoItem, 0, NULL, 0);
            items_take(c, cc);
            if (c->nitems) {
                DataBrowserItemID *ids = (DataBrowserItemID *)malloc(sizeof(DataBrowserItemID) * (size_t)c->nitems);
                for (int i = 0; i < c->nitems; i++) ids[i] = (DataBrowserItemID)(i + 1);
                AddDataBrowserItems(c->ref, kDataBrowserNoItem, (UInt32)c->nitems, ids, kDataBrowserItemNoProperty);
                free(ids);
                SetDataBrowserScrollPosition(c->ref, 0, 0);
            }
            c->sel = -2;
        }
        if (h->sel != c->sel) {
            DataBrowserItemID one = (DataBrowserItemID)(h->sel + 1);
            c->sel = h->sel;
            if (h->sel >= 0) {
                if (!d->chat) SetDataBrowserSelectedItems(c->ref, 1, &one, kDataBrowserItemsAssign);   /* in the chat the game selects the last row only to scroll to it */
                RevealDataBrowserItem(c->ref, one, 'TEXT', kDataBrowserRevealOnly);
            }
            else SetDataBrowserSelectedItems(c->ref, 0, NULL, kDataBrowserItemsRemove);
        }
        break;
    case HC_COMBO_LIST: case HC_COMBO_DROP:
        if (!items_same(c, cc)) {
            DeleteMenuItems(c->menu, 1, CountMenuItems(c->menu));
            items_take(c, cc);
            for (int i = 0; i < c->nitems; i++) { t = cfs(c->items[i]); AppendMenuItemTextWithCFString(c->menu, t, 0, 0, NULL); CFRelease(t); }
            SetControl32BitMaximum(c->ref, c->nitems);
        }
        SetControl32BitValue(c->ref, h->sel + 1);
        break;
    default: break;
    }
done:
    if (cc->items) { for (int i = 0; i < h->nitems; i++) free(cc->items[i]); free(cc->items); }
    free(cc);
}
static void do_dlg_close(uint32_t id) {
    Dlg *d = d_find(id);
    if (!d) return;
    for (int i = 0; i < MAXC; i++) if (d->c[i].used) { for (int k = 0; k < d->c[i].nitems; k++) free(d->c[i].items[k]); free(d->c[i].items); }
    if (d->ref) DisposeWindow(d->ref);
    for (int i = 0; i < MAXC; i++) if (d->c[i].used && d->c[i].menu) { DeleteMenu(GetMenuID(d->c[i].menu)); DisposeMenu(d->c[i].menu); }   /* after the window: the popup still uses the menu */
    memset(d, 0, sizeof *d);
}
/* the typed text goes back to the core on every tick (no per-control text events) */
static void poll_edits(void) {
    for (int i = 0; i < MAXD; i++) {
        if (!gD[i].used) continue;
        for (int k = 0; k < MAXC; k++) {
            Ctl *c = &gD[i].c[k];
            CFStringRef s = NULL;
            char buf[256];
            if (!c->used || c->kind != HC_EDIT || !c->ref) continue;
            if (GetControlData(c->ref, kControlEntireControl, kControlEditTextCFStringTag, sizeof s, &s, NULL) != noErr || !s) continue;
            cfs_get(s, buf, sizeof buf);
            CFRelease(s);
            if (strcmp(buf, c->text) != 0) { snprintf(c->text, sizeof c->text, "%s", buf); dlg_emit(&gD[i], c, DLG_TEXT, 0, buf); }
        }
    }
}

/* ---- host.h API: they only enqueue --------------------------------------------------------------- */
void host_window_update(const HostWindow *w) { HostWindow *c = (HostWindow *)malloc(sizeof *c); *c = *w; cmd_push(C_WIN, 0, c); }
void host_window_destroy(uint32_t id) { cmd_push(C_WIN_DESTROY, id, NULL); }
void host_present(uint32_t win, const uint8_t *pix, int w, int h, const uint8_t pal[256][4], const char *who) {
    Frame *f = (Frame *)malloc(sizeof *f);
    (void)who;
    f->win = win; f->w = w; f->h = h;
    f->pix = (uint8_t *)malloc((size_t)w * (size_t)h);
    memcpy(f->pix, pix, (size_t)w * (size_t)h);
    memcpy(f->pal, pal, sizeof f->pal);
    cmd_push(C_PRESENT, win, f);
}
void host_computer_name(char *out, size_t cap) { hostname_short(out, cap); }
void host_set_title(const char *t) { cmd_push(C_TITLE, 0, strdup(t)); }
void host_set_cursor(uint32_t idc) { cmd_push(C_CURSOR, idc, NULL); }
void host_show_cursor(int v) { cmd_push(C_SHOWCUR, (uint32_t)v, NULL); }
static HostMenuItem *copy_items(const HostMenuItem *s, int n) {
    HostMenuItem *d = (HostMenuItem *)calloc((size_t)(n ? n : 1), sizeof *d);
    for (int i = 0; i < n; i++) { d[i] = s[i]; d[i].child = copy_items(s[i].child, s[i].nchild); }
    return d;
}
void host_set_menu(const HostMenuItem *top, int n, int sys) {
    MenuCmd *m = (MenuCmd *)malloc(sizeof *m);
    m->top = copy_items(top, n); m->n = n; m->sys = sys;
    cmd_push(C_MENU, 0, m);
}
void host_dlg_open(const HostDlg *d) { HostDlg *c = (HostDlg *)malloc(sizeof *c); *c = *d; cmd_push(C_DLG_OPEN, 0, c); }
void host_dlg_ctl(uint32_t dlg, const HostCtl *c) {
    CtlCmd *cc = (CtlCmd *)calloc(1, sizeof *cc);
    cc->dlg = dlg; cc->c = *c;
    if (c->nitems > 0 && c->items) {
        cc->items = (char **)calloc((size_t)c->nitems, sizeof(char *));
        for (int i = 0; i < c->nitems; i++) cc->items[i] = strdup(c->items[i] ? c->items[i] : "");
    } else cc->c.nitems = 0;
    cc->c.items = NULL;
    cmd_push(C_DLG_CTL, 0, cc);
}
void host_dlg_close(uint32_t dlg) { cmd_push(C_DLG_CLOSE, dlg, NULL); }
void host_dlg_front(uint32_t dlg) { cmd_push(C_DLG_FRONT, dlg, NULL); }
void host_dlg_back(uint32_t dlg) { cmd_push(C_DLG_BACK, dlg, NULL); }
void host_dlg_center(uint32_t dlg) { cmd_push(C_DLG_CENTER, dlg, NULL); }
void host_beep(void) { cmd_push(C_BEEP, 0, NULL); }
void host_chat_menu_enable(int on) { cmd_push(C_CHAT_MENU, (uint32_t)on, NULL); }
extern int win32_slow_trace;                                    /* win32/core.c */
/* Help: GPMHELP.HLP becomes HTML in the write folder and opens in the default browser (Tiger's Help Viewer showed blank loose pages) */
typedef struct { const char *idx; int context; } HelpArg;
static void help_call(void *p) {
    HelpArg *a = (HelpArg *)p;
    /* "file:///...", without the "localhost" that Tiger's CFURL generates */
    CFStringRef raw = CFStringCreateWithCString(NULL, a->idx, kCFStringEncodingUTF8);
    CFStringRef esc = raw ? CFURLCreateStringByAddingPercentEscapes(NULL, raw, NULL, CFSTR("#?"), kCFStringEncodingUTF8) : NULL;
    CFStringRef full = !esc ? NULL : a->context >= 0 ? CFStringCreateWithFormat(NULL, NULL, CFSTR("file://%@#ctx%d"), esc, a->context)
                                                     : CFStringCreateWithFormat(NULL, NULL, CFSTR("file://%@"), esc);
    CFURLRef u = full ? CFURLCreateWithString(NULL, full, NULL) : NULL;
    if (u) { LSOpenCFURLRef(u, NULL); CFRelease(u); }
    if (full) CFRelease(full);
    if (esc) CFRelease(esc);
    if (raw) CFRelease(raw);
}
void host_help(int context) {
    char idx[1200];
    HelpArg a;
    if (!gCfg.game || !gCfg.wdir || !hlp_prepare(gCfg.game, gCfg.wdir, idx, sizeof idx)) {
        host_message_box("Help", "GPMHELP.HLP was not found in the game folder.", 0x30);
        return;
    }
    a.idx = idx; a.context = context;
    on_main(help_call, &a);
}

/* MessageBox and IP: CFUserNotification (exists on Tiger), blocks only the guest thread */
static int message_box_impl(const char *title, const char *text, uint32_t flags) {
    static const struct { const char *name; int id; } sets[6][3] = {
        {{"OK", 1}, {0, 0}, {0, 0}}, {{"OK", 1}, {"Cancel", 2}, {0, 0}}, {{"Abort", 3}, {"Retry", 4}, {"Ignore", 5}},
        {{"Yes", 6}, {"No", 7}, {"Cancel", 2}}, {{"Yes", 6}, {"No", 7}, {0, 0}}, {{"Retry", 4}, {"Cancel", 2}, {0, 0}}};
    unsigned ty = flags & 0xFu, def = (flags >> 8) & 0xFu;
    SInt32 icon = (flags & 0xF0u) == 0x10u ? kCFUserNotificationStopAlertLevel : (flags & 0xF0u) == 0x30u ? kCFUserNotificationCautionAlertLevel : kCFUserNotificationNoteAlertLevel;
    CFStringRef b[3] = {NULL, NULL, NULL}, h, m;
    CFOptionFlags resp = 0;
    int n = 0, order[3] = {0, 1, 2}, k;
    if (ty > 5) ty = 0;
    while (n < 3 && sets[ty][n].name) n++;
    if (def >= (unsigned)n) def = 0;
    order[0] = (int)def; order[def] = 0;                       /* the default button goes first */
    for (int i = 0; i < n; i++) b[i] = cfs(sets[ty][order[i]].name);
    h = cfs(title); m = cfs(text);
    CFUserNotificationDisplayAlert(0, (CFOptionFlags)icon, NULL, NULL, NULL, h, m, b[0], b[1], b[2], &resp);
    CFRelease(h); CFRelease(m);
    for (int i = 0; i < n; i++) CFRelease(b[i]);
    k = (int)(resp & 3u);                                      /* 0 default, 1 alternate, 2 other */
    if (k >= n) k = 0;
    return sets[ty][order[k]].id;
}
static int net_ask_impl(char *out, int cap) {
    const char *env = getenv("GPM2_NET_HOST");
    CFMutableDictionaryRef d;
    CFUserNotificationRef n;
    CFOptionFlags resp = 0;
    SInt32 err = 0;
    const void *titles[1];
    CFArrayRef arr;
    int ok = 0;
    if (env) { snprintf(out, (size_t)cap, "%s", env); return 1; }
    d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    titles[0] = CFSTR("IP address:");
    arr = CFArrayCreate(NULL, titles, 1, &kCFTypeArrayCallBacks);
    CFDictionarySetValue(d, kCFUserNotificationAlertHeaderKey, CFSTR("TCP/IP Connection"));
    CFDictionarySetValue(d, kCFUserNotificationAlertMessageKey, CFSTR("Enter the IP address of the host (leave empty to search the local network)."));
    CFDictionarySetValue(d, kCFUserNotificationTextFieldTitlesKey, arr);
    CFDictionarySetValue(d, kCFUserNotificationDefaultButtonTitleKey, CFSTR("OK"));
    CFDictionarySetValue(d, kCFUserNotificationAlternateButtonTitleKey, CFSTR("Cancel"));
    n = CFUserNotificationCreate(NULL, 0, 0, &err, d);
    out[0] = 0;
    if (n && CFUserNotificationReceiveResponse(n, 0, &resp) == 0 && (resp & 3u) == kCFUserNotificationDefaultResponse) {
        cfs_get(CFUserNotificationGetResponseValue(n, kCFUserNotificationTextFieldValuesKey, 0), out, (size_t)cap);
        ok = 1;
    }
    if (n) CFRelease(n);
    CFRelease(arr); CFRelease(d);
    return ok;
}

/* filter by extension (pattern of the chosen filter: "*.gps;*.gcf") */
static char gNavPat[128];
static int pat_match(const char *name) {
    char pat[128], *tok;
    size_t nl = strlen(name);
    if (!gNavPat[0]) return 1;
    snprintf(pat, sizeof pat, "%s", gNavPat);
    for (tok = strtok(pat, ";"); tok; tok = strtok(NULL, ";")) {
        const char *ext = strrchr(tok, '.');
        size_t el;
        if (!ext) continue;
        if (!strcmp(ext, ".*")) return 1;
        el = strlen(ext);
        if (nl >= el && !strncasecmp(name + nl - el, ext, el)) return 1;
    }
    return 0;
}
static pascal Boolean nav_filter(AEDesc *item, void *info, void *ud, NavFilterModes mode) {
    NavFileOrFolderInfo *fi = (NavFileOrFolderInfo *)info;
    FSRef fr; HFSUniStr255 un; char nm[300];
    (void)ud; (void)mode;
    if (item->descriptorType != typeFSRef || !fi || fi->isFolder) return true;
    if (AEGetDescData(item, &fr, sizeof fr) != noErr || FSGetCatalogInfo(&fr, kFSCatInfoNone, NULL, &un, NULL, NULL) != noErr) return true;
    { CFStringRef cs = CFStringCreateWithCharacters(NULL, un.unicode, (CFIndex)un.length); cfs_get(cs, nm, sizeof nm); CFRelease(cs); }
    return pat_match(nm) != 0;
}
static void nav_set_dir(NavDialogRef dlg, const char *dir) {
    FSRef fr; AEDesc d;
    if (!dir || !dir[0] || FSPathMakeRef((const UInt8 *)dir, &fr, NULL) != noErr) return;
    if (AECreateDesc(typeFSRef, &fr, sizeof fr, &d) != noErr) return;
    NavCustomControl(dlg, kNavCtlSetLocation, &d);
    AEDisposeDesc(&d);
}

/* Open/Save: Navigation Services (the Carbon equivalent of NSOpenPanel). Runs modal on the calling thread (the guest one). */
static int file_dialog_impl(const HostFileReq *rq, char *out, size_t cap) {
    NavDialogCreationOptions o;
    NavDialogRef dlg = NULL;
    NavReplyRecord rep;
    int ok = 0;
    CFStringRef t;
    if (NavGetDefaultDialogCreationOptions(&o) != noErr) return 0;
    t = cfs(rq->title);
    o.windowTitle = t;
    o.modality = kWindowModalityAppModal;
    o.optionFlags &= ~kNavAllowMultipleFiles;
    if (rq->save) {
        o.saveFileName = cfs(rq->name);
        if (NavCreatePutFileDialog(&o, 0, 0, NULL, NULL, &dlg) != noErr) dlg = NULL;
        CFRelease(o.saveFileName);
    } else {
        NavObjectFilterUPP fu = NewNavObjectFilterUPP(nav_filter);
        gNavPat[0] = 0;
        if (rq->fsel >= 0 && rq->fsel < rq->nfilt) snprintf(gNavPat, sizeof gNavPat, "%s", rq->fpat[rq->fsel]);
        if (NavCreateGetFileDialog(&o, NULL, NULL, NULL, fu, NULL, &dlg) != noErr) dlg = NULL;
    }
    CFRelease(t);
    if (!dlg) return 0;
    nav_set_dir(dlg, rq->dir);
    if (NavDialogRun(dlg) == noErr && NavDialogGetReply(dlg, &rep) == noErr) {
        if (rep.validRecord) {
            AEKeyword kw; DescType ty; Size sz; FSRef fr; char path[1024];
            if (AEGetNthPtr(&rep.selection, 1, typeFSRef, &kw, &ty, &fr, sizeof fr, &sz) == noErr && FSRefMakePath(&fr, (UInt8 *)path, sizeof path) == noErr) {
                if (rq->save) {
                    char nm[260];
                    cfs_get(rep.saveFileName, nm, sizeof nm);
                    if (rq->defext[0] && !strchr(nm, '.')) { size_t l = strlen(nm); snprintf(nm + l, sizeof nm - l, ".%s", rq->defext); }
                    path_join(out, cap, path, nm);
                } else snprintf(out, cap, "%s", path);
                ok = 1;
            }
        }
        NavDisposeReply(&rep);
    }
    NavDialogDispose(dlg);
    return ok;
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

/* ---- audio: AudioUnit via the Component Manager (the AudioComponent* APIs only exist from 10.6) ------- */
static AudioUnit gUnit;
static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)ref; (void)flags; (void)ts; (void)bus;
    if (io->mNumberBuffers < 1) return noErr;
    audio_render((int16_t *)io->mBuffers[0].mData, (int)frames);
    return noErr;
}
int host_audio_open(void) {
    ComponentDescription cd;
    Component comp;
    AudioStreamBasicDescription f;
    AURenderCallbackStruct cb;
    cd.componentType = kAudioUnitType_Output; cd.componentSubType = kAudioUnitSubType_DefaultOutput;
    cd.componentManufacturer = kAudioUnitManufacturer_Apple; cd.componentFlags = 0; cd.componentFlagsMask = 0;
    comp = FindNextComponent(NULL, &cd);
    if (!comp || OpenAComponent(comp, &gUnit) != noErr) { fprintf(stderr, "[audio] no audio output\n"); return 0; }
    memset(&f, 0, sizeof f);
    f.mSampleRate = 44100; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;   /* the mixer delivers host int16 */
    f.mChannelsPerFrame = 2; f.mBitsPerChannel = 16; f.mBytesPerFrame = 4; f.mFramesPerPacket = 1; f.mBytesPerPacket = 4;
    cb.inputProc = render_cb; cb.inputProcRefCon = NULL;
    if (AudioUnitSetProperty(gUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr ||
        AudioUnitSetProperty(gUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr ||
        AudioUnitInitialize(gUnit) != noErr || AudioOutputUnitStart(gUnit) != noErr) {
        fprintf(stderr, "[audio] failed to start the AudioUnit\n");
        return 0;
    }
    fprintf(stderr, "[audio] output started (44100 Hz, 16-bit, stereo)\n");
    return 1;
}

/* ---- time and lifecycle -------------------------------------------------------------------------------- */
uint32_t host_now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (uint32_t)((mach_absolute_time() * tb.numer / tb.denom) / 1000000ull);
}
void host_sleep_ms(uint32_t ms) { usleep(ms * 1000u); }
void host_init(const char *out) { (void)out; }
unsigned host_frame_count(void) { return 0; }
const char *host_last_frame_path(void) { return ""; }
int host_poll_events(unsigned idle) { (void)idle; return 0; }
int host_script_active(void) { return 0; }
void host_set_script(const char *p) { (void)p; }

/* ---- input ----------------------------------------------------------------------------------------- */
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
static unsigned mods_of(UInt32 m) {
    return ((m & shiftKey) ? HOST_MOD_SHIFT : 0u) | ((m & controlKey) ? HOST_MOD_CTRL : 0u) | ((m & optionKey) ? HOST_MOD_ALT : 0u);
}
static WindowRef gPrefWin;                                      /* Preferences window (created in show_prefs) */
static void prefs_refresh(void);
/* the full-screen main window covers everything: children, dialogs and Preferences must stay in front of it */
static void raise_over_main(void) {
    for (int i = 1; i < MAXW; i++) if (gW[i].used && gW[i].ref && IsWindowVisible(gW[i].ref)) BringToFront(gW[i].ref);
    for (int i = 0; i < MAXD; i++) if (gD[i].used && gD[i].ref) BringToFront(gD[i].ref);
    if (gPrefWin && IsWindowVisible(gPrefWin)) BringToFront(gPrefWin);
}
static void set_fullscreen(int on) {
    W *m = &gW[0];
    WindowRef old = m->ref;
    on = on != 0;
    if (on == gFull || !old) return;
    gFull = on;
    img_update();
    if (!w_make_ref(m, &m->hw, 1)) { gFull = !on; img_update(); return; }
    {                                                          /* the title may have come from host_set_title, which does not go through hw */
        CFStringRef tt = NULL;
        if (CopyWindowTitleAsCFString(old, &tt) == noErr && tt) { SetWindowTitleWithCFString(m->ref, tt); CFRelease(tt); }
    }
    SetSystemUIMode(on ? kUIModeAllHidden : kUIModeNormal, on ? kUIOptionAutoShowMenuBar : 0);
    ShowWindow(m->ref);
    SelectWindow(m->ref);
    DisposeWindow(old);
    raise_over_main();
    relayout_all();
    if (!on) ConstrainWindowToScreen(m->ref, kWindowStructureRgn, kWindowConstrainStandardOptions, NULL, NULL);
    view_refresh();
}
static void set_full_mode(int mode, int persist) {
    if (mode < 0 || mode > 2) return;
    gFullMode = mode;
    if (persist) pref_int_set(CFSTR("FullScreenMode"), mode);
    relayout_all();
    view_refresh();
    prefs_refresh();
}
static void scale_apply(float s) {
    gScale = s;
    scale_save();
    relayout_all();
    if (!gFull && gW[0].ref) ConstrainWindowToScreen(gW[0].ref, kWindowStructureRgn, kWindowConstrainStandardOptions, NULL, NULL);
    view_refresh();
    prefs_refresh();
}
static OSStatus mouse_handler(EventHandlerCallRef next, EventRef ev, void *ud) {
    WindowRef wr = NULL;
    HIPoint p;
    UInt32 kind = GetEventKind(ev), mods = 0;
    EventMouseButton btn = kEventMouseButtonPrimary;
    HostEvent h;
    W *w;
    int ox, oy;
    float fx, fy;
    (void)next; (void)ud;
    if (GetEventParameter(ev, kEventParamWindowRef, typeWindowRef, NULL, sizeof wr, NULL, &wr) != noErr || !wr) return eventNotHandledErr;
    w = w_of_ref(wr);
    if (!w) return eventNotHandledErr;
    if (GetEventParameter(ev, kEventParamMouseLocation, typeHIPoint, NULL, sizeof p, NULL, &p) != noErr) return eventNotHandledErr;
    content_origin(w, &ox, &oy);
    fx = p.x - (float)ox; fy = p.y - (float)oy;
    if (w == &gW[0]) { fx -= (float)gImgX; fy -= (float)gImgY; }
    if (w == &gW[0] && gFull && (fx < 0 || fy < 0 || fx >= (float)gImgW || fy >= (float)gImgH)) {   /* outside the image (black bars): no event */
        if (kind == kEventMouseDown) return noErr;
        if ((kind == kEventMouseMoved || kind == kEventMouseDragged) && !w->buttons) return noErr;
    }
    if (kind == kEventMouseDown) {
        WindowPartCode part = 0;
        GetEventParameter(ev, kEventParamWindowPartCode, typeWindowPartCode, NULL, sizeof part, NULL, &part);
        if (part != inContent) return eventNotHandledErr;      /* title bar, close box: handled by the system */
        if (w == &gW[0]) {
            if (!IsWindowActive(wr)) SelectWindow(wr);
            if (gFull) raise_over_main();                      /* otherwise a dialog ends up behind the full screen */
        }
    }
    GetEventParameter(ev, kEventParamKeyModifiers, typeUInt32, NULL, sizeof mods, NULL, &mods);
    GetEventParameter(ev, kEventParamMouseButton, typeMouseButton, NULL, sizeof btn, NULL, &btn);
    memset(&h, 0, sizeof h);
    h.win = w == &gW[0] ? 0 : w->id;
    h.x = (int)floorf(fx / gSx); h.y = (int)floorf(fy / gSy);
    h.mods = mods_of(mods);
    if (kind == kEventMouseMoved || kind == kEventMouseDragged) {
        h.type = HOST_EV_MOUSEMOVE;
    } else {
        int b = btn == kEventMouseButtonSecondary ? 1 : btn == kEventMouseButtonTertiary ? 2 : 0;
        if (b == 0 && (mods & controlKey)) b = 1;               /* Ctrl+click = right button */
        if (kind == kEventMouseDown) { w->buttons |= 1 << b; h.b = 1; }
        else { if (!(w->buttons & (1 << b))) return eventNotHandledErr; w->buttons &= ~(1 << b); h.b = 0; }
        h.type = HOST_EV_BUTTON; h.a = b;
    }
    host_push_event(&h);
    return noErr;
}
static UInt32 gLastMods;
static OSStatus key_handler(EventHandlerCallRef next, EventRef ev, void *ud) {
    UInt32 kind = GetEventKind(ev), kc = 0, mods = 0;
    unsigned char ch = 0;
    HostEvent h;
    (void)next; (void)ud;
    if (d_of_ref(GetUserFocusWindow())) return eventNotHandledErr;   /* keys in a native dialog belong to its controls, not to the game */
    GetEventParameter(ev, kEventParamKeyModifiers, typeUInt32, NULL, sizeof mods, NULL, &mods);
    memset(&h, 0, sizeof h);
    h.mods = mods_of(mods);
    if (kind == kEventRawKeyModifiersChanged) {
        static const struct { UInt32 f; int vk; } m[] = {{shiftKey, 0x10}, {controlKey, 0x11}, {optionKey, 0x12}, {alphaLock, 0x14}};
        UInt32 diff = mods ^ gLastMods;
        gLastMods = mods;
        for (int i = 0; i < 4; i++)
            if (diff & m[i].f) { h.type = HOST_EV_KEY; h.a = m[i].vk; h.b = m[i].vk == 0x14 ? 1 : (mods & m[i].f) != 0; host_push_event(&h); }
        return eventNotHandledErr;
    }
    if (mods & cmdKey) return eventNotHandledErr;              /* app menu shortcuts */
    GetEventParameter(ev, kEventParamKeyCode, typeUInt32, NULL, sizeof kc, NULL, &kc);
    GetEventParameter(ev, kEventParamKeyMacCharCodes, typeChar, NULL, sizeof ch, NULL, &ch);
    h.type = HOST_EV_KEY; h.a = vk_from_keycode(kc);
    if (!h.a) return eventNotHandledErr;
    h.b = kind != kEventRawKeyUp; h.c = kind == kEventRawKeyRepeat;
    if (h.b) {
        if (ch == 0x7F) ch = 8; else if (ch == 3) ch = 13;
        if (ch >= 0x20 || ch == 8 || ch == 9 || ch == 13 || ch == 27) h.ch = (ch >= 0x1C && ch <= 0x1F) || ch >= 0x80 ? 0 : ch;
    }
    host_push_event(&h);
    return noErr;
}
static void show_prefs(void);
static OSStatus cmd_handler(EventHandlerCallRef next, EventRef ev, void *ud) {
    HICommand c;
    (void)next; (void)ud;
    if (GetEventParameter(ev, kEventParamDirectObject, typeHICommand, NULL, sizeof c, NULL, &c) != noErr) return eventNotHandledErr;
    if (c.commandID == kHICommandQuit) { push_ev_close(0); return noErr; }
    if (c.commandID == kHICommandAppHelp) { host_help(-1); return noErr; }   /* Help menu > Grand Prix Manager 2 Help: same path as the game's Help */
    if (c.commandID == kHICommandAbout) {                        /* same command as the game's Help > About */
        HostEvent h;
        memset(&h, 0, sizeof h);
        h.type = HOST_EV_MENU; h.a = 40029; h.b = 1;
        host_push_event(&h);
        return noErr;
    }
    if (c.commandID >= CMD_SCALE + 10 && c.commandID <= CMD_SCALE + 200) { scale_apply((float)(c.commandID - CMD_SCALE) / 10.0f); return noErr; }
    if (c.commandID == CMD_FULL) { set_fullscreen(!gFull); return noErr; }
    if (c.commandID == CMD_FIT) { scale_apply(gFitScale); return noErr; }
    if (c.commandID >= CMD_FMODE && c.commandID <= CMD_FMODE + 2) { set_full_mode((int)(c.commandID - CMD_FMODE), 1); return noErr; }
    if (c.commandID == kHICommandPreferences) { show_prefs(); return noErr; }
    if (c.commandID == CMD_CHAT || c.commandID == CMD_CHAT + 1) {
        HostEvent h;
        memset(&h, 0, sizeof h);
        h.type = HOST_EV_CHAT; h.a = (int)(c.commandID - CMD_CHAT);
        host_push_event(&h);
        return noErr;
    }
    if (c.commandID >= CMD_BASE && c.commandID < CMD_BASE + 2048u && (int)(c.commandID - CMD_BASE) < gNMc) {
        HostEvent h;
        memset(&h, 0, sizeof h);
        h.type = HOST_EV_MENU; h.a = (int)gMc[c.commandID - CMD_BASE].id; h.b = gMc[c.commandID - CMD_BASE].sys;
        host_push_event(&h);
        return noErr;
    }
    return eventNotHandledErr;
}

static void apply_cursor(uint32_t idc) {
    switch (idc) {
    case 0x7F01: SetThemeCursor(kThemeIBeamCursor); break;
    case 0x7F02: SetThemeCursor(kThemeWatchCursor); break;
    case 0x7F03: SetThemeCursor(kThemeCrossCursor); break;
    default: SetThemeCursor(kThemeArrowCursor);
    }
}
static int gCurHidden;
static void run_cmds(void) {
    for (;;) {
        Cmd *c;
        pthread_mutex_lock(&qmu);
        c = qhead;
        if (c) { qhead = c->next; if (!qhead) qtail = NULL; }
        pthread_mutex_unlock(&qmu);
        if (!c) break;
        switch (c->kind) {
        case C_WIN: do_win((HostWindow *)c->p); free(c->p); break;
        case C_WIN_DESTROY: do_win_destroy(c->id); break;
        case C_PRESENT: do_present((Frame *)c->p); break;
        case C_TITLE: { CFStringRef t = cfs((char *)c->p); if (gW[0].ref) SetWindowTitleWithCFString(gW[0].ref, t); CFRelease(t); free(c->p); break; }
        case C_CURSOR: apply_cursor(c->id); break;
        case C_SHOWCUR: if (c->id && gCurHidden) { ShowCursor(); gCurHidden = 0; } else if (!c->id && !gCurHidden) { HideCursor(); gCurHidden = 1; } break;
        case C_MENU: do_menu((MenuCmd *)c->p); break;
        case C_DLG_OPEN: do_dlg_open((HostDlg *)c->p); break;
        case C_DLG_CTL: do_dlg_ctl((CtlCmd *)c->p); break;
        case C_DLG_CLOSE: do_dlg_close(c->id); break;
        case C_DLG_FRONT: { Dlg *fd = d_find(c->id); if (fd && fd->ref) SelectWindow(fd->ref); break; }
        case C_DLG_BACK: if (gW[0].ref) SelectWindow(gW[0].ref); break;
        case C_DLG_CENTER: { Dlg *fd = d_find(c->id); if (fd && fd->ref && gW[0].ref) RepositionWindow(fd->ref, gW[0].ref, kWindowCenterOnParentWindow); break; }
        case C_BEEP: SysBeep(1); break;
        case C_CHAT_MENU:
            gChatOn = c->id != 0;
            if (gChatM) { if (gChatOn) EnableMenuItem(gChatM, 0); else DisableMenuItem(gChatM, 0); DrawMenuBar(); }
            break;
        case C_CALL: { MainCall *mc = (MainCall *)c->p; mc->fn(mc->arg); mc->done = 1; break; }
        }
        free(c);
    }
    poll_edits();
}
static void tick(EventLoopTimerRef t, void *ud) { (void)t; (void)ud; run_cmds(); }

/* ---- game folder and 1.02B exe (own SHA-256: CommonCrypto only exists from 10.5) ------------------- */
#include "../exe_sha.h"
/* path of the folder's GPM2.EXE (case-insensitive); 0 if it does not exist */
static int exe_in_folder(const char *dir, char *out, size_t cap) {
    DIR *d = opendir(dir);
    struct dirent *e;
    int ok = 0;
    if (!d) return 0;
    while (!ok && (e = readdir(d))) if (!strcasecmp(e->d_name, "gpm2.exe")) { path_join(out, cap, dir, e->d_name); ok = 1; }
    closedir(d);
    return ok;
}
static int is_game_folder(const char *dir) {
    char e[1200];
    return exe_in_folder(dir, e, sizeof e) && is_exe_102b(e);
}
static void alert_need_102b(const char *dir) {
    char e[1200];
    host_message_box("This app requires Grand Prix Manager 2 version 1.02B",
        exe_in_folder(dir, e, sizeof e) ? "GPM2.EXE in the chosen folder is not version 1.02B. Install the Grand Prix Manager 2 1.02B patch, then choose the game folder again."
                                        : "GPM2.EXE was not found in the chosen folder. Choose the folder that contains GPM2.EXE (Grand Prix Manager 2 version 1.02B).", 0x10);
}
static int nav_pick(const char *msg, int folder, char *out, size_t cap) {
    NavDialogCreationOptions o;
    NavDialogRef dlg = NULL;
    NavReplyRecord rep;
    int ok = 0;
    NavGetDefaultDialogCreationOptions(&o);
    o.message = cfs(msg);
    o.modality = kWindowModalityAppModal;
    o.optionFlags &= ~kNavAllowMultipleFiles;
    if (folder) { if (NavCreateChooseFolderDialog(&o, NULL, NULL, NULL, &dlg) != noErr) dlg = NULL; }
    else if (NavCreateGetFileDialog(&o, NULL, NULL, NULL, NULL, NULL, &dlg) != noErr) dlg = NULL;
    CFRelease(o.message);
    if (!dlg) return 0;
    if (NavDialogRun(dlg) == noErr && NavDialogGetReply(dlg, &rep) == noErr) {
        AEKeyword kw; DescType ty; Size sz; FSRef fr;
        if (rep.validRecord && AEGetNthPtr(&rep.selection, 1, typeFSRef, &kw, &ty, &fr, sizeof fr, &sz) == noErr && FSRefMakePath(&fr, (UInt8 *)out, (UInt32)cap) == noErr) ok = 1;
        NavDisposeReply(&rep);
    }
    NavDialogDispose(dlg);
    return ok;
}
static void pref_get(const char *key, char *out, size_t cap) {
    CFStringRef k = cfs(key);
    CFPropertyListRef v = CFPreferencesCopyAppValue(k, kCFPreferencesCurrentApplication);
    out[0] = 0;
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)v, out, (CFIndex)cap, kCFStringEncodingUTF8);
    if (v) CFRelease(v);
    CFRelease(k);
}
static void pref_set(const char *key, const char *val) {
    CFStringRef k = cfs(key), v = CFStringCreateWithCString(NULL, val, kCFStringEncodingUTF8);
    CFPreferencesSetAppValue(k, v, kCFPreferencesCurrentApplication);
    CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
    CFRelease(k); CFRelease(v);
}
/* No saved folder and no arguments: ALWAYS asks (no embedded path). */
static int resolve_files(void) {
    static char game[1024], exe[1200], wdir[1100];
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    if (!gCfg.game) {
        pref_get("GameFolder", game, sizeof game);
        if (!game[0] || !is_game_folder(game)) {
            if (!nav_pick("Choose the folder that contains GPM2.EXE (Grand Prix Manager 2 version 1.02B).", 1, game, sizeof game)) return 0;
            if (!is_game_folder(game)) { alert_need_102b(game); return 0; }
        }
        pref_set("GameFolder", game);
        gCfg.game = game;
    }
    if (!gCfg.exe) {                                           /* --exe: development only */
        if (!exe_in_folder(gCfg.game, exe, sizeof exe)) { alert_need_102b(gCfg.game); return 0; }
        gCfg.exe = exe;
    }
    if (!gCfg.wdir) {
        char p[1100];
        snprintf(p, sizeof p, "%s/Library/Application Support", home);
        mkdir(p, 0755);
        path_join(wdir, sizeof wdir, p, "GPM2");
        mkdir(wdir, 0755);
        gCfg.wdir = wdir;
    }
    return 1;
}
/* ---- Preferences window (same layout as the modern app) ------------------------------------------ */
enum { PT_SCALE = 1, PT_MODE, PT_OPEN, PT_FOLDER };
static ControlRef gPrefScale, gPrefMode, gPrefOpen, gPrefFolder;
static void prefs_refresh(void) {
    int cur = 0;
    char gf[1024];
    CFStringRef t;
    if (!gPrefWin) return;
    for (int i = 0; i < gNWsz; i++) if (fabsf(gScale - (float)gWszT10[i] / 10.0f) < 0.01f) cur = i + 1;
    SetControl32BitValue(gPrefScale, cur);                      /* scale outside the list (free): no item checked */
    SetControl32BitValue(gPrefMode, gFullMode + 1);
    SetControl32BitValue(gPrefOpen, gOpenFull ? 1 : 0);
    pref_get("GameFolder", gf, sizeof gf);
    t = cfs(gf[0] ? gf : gCfg.game);
    SetControlData(gPrefFolder, kControlEntireControl, kControlStaticTextCFStringTag, sizeof t, &t);
    CFRelease(t);
    HIViewSetNeedsDisplay(gPrefFolder, true);
}
static OSStatus pref_hit(EventHandlerCallRef next, EventRef ev, void *ud) {
    (void)next; (void)ev;
    switch ((int)(intptr_t)ud) {
    case PT_SCALE: {
        int v = (int)GetControl32BitValue(gPrefScale);
        if (v >= 1 && v <= gNWsz) scale_apply((float)gWszT10[v - 1] / 10.0f);
        break;
    }
    case PT_MODE: set_full_mode((int)GetControl32BitValue(gPrefMode) - 1, 1); break;
    case PT_OPEN: gOpenFull = GetControl32BitValue(gPrefOpen) != 0; pref_int_set(CFSTR("OpenFullScreen"), gOpenFull); break;
    case PT_FOLDER: {
        char dir[1024];
        if (!nav_pick("Choose the folder that contains GPM2.EXE (Grand Prix Manager 2 version 1.02B).", 1, dir, sizeof dir)) break;
        if (!is_game_folder(dir)) { alert_need_102b(dir); break; }
        pref_set("GameFolder", dir);
        prefs_refresh();
        host_message_box("Restart required", "The new game folder will be used the next time Grand Prix Manager 2 starts.", 0x40);
        break;
    }
    }
    return eventNotHandledErr;                                  /* the control still needs to handle the click */
}
static OSStatus pref_win_handler(EventHandlerCallRef next, EventRef ev, void *ud) {
    (void)next; (void)ev; (void)ud;
    HideWindow(gPrefWin);
    return noErr;
}
static void pref_style(ControlRef c, int right) {
    ControlFontStyleRec fs;
    memset(&fs, 0, sizeof fs);
    fs.flags = kControlUseThemeFontIDMask | (right ? kControlUseJustMask : 0);
    fs.font = kThemeSmallSystemFont;
    fs.just = teFlushRight;
    SetControlFontStyle(c, &fs);
}
static void pref_label(WindowRef win, int y, const char *text) {
    Rect r;
    ControlRef c = NULL;
    CFStringRef t = cfs(text);
    SetRect(&r, 10, y, 140, y + 16);
    if (CreateStaticTextControl(win, &r, t, NULL, &c) == noErr && c) pref_style(c, 1);
    CFRelease(t);
}
static ControlRef pref_popup(WindowRef win, int y, MenuRef m, int n, int tag) {
    static const EventTypeSpec hit[] = {{kEventClassControl, kEventControlHit}};
    Rect r;
    ControlRef c = NULL;
    ControlSize sz = kControlSizeSmall;
    SetRect(&r, 150, y, 350, y + 20);
    InsertMenu(m, kInsertHierarchicalMenu);
    if (CreatePopupButtonControl(win, &r, CFSTR(""), GetMenuID(m), false, 0, 0, 0, &c) != noErr || !c) return NULL;
    SetControl32BitMaximum(c, n);
    pref_style(c, 0);
    SetControlData(c, kControlEntireControl, kControlSizeTag, sizeof sz, &sz);
    InstallControlEventHandler(c, NewEventHandlerUPP(pref_hit), 1, hit, (void *)(intptr_t)tag, NULL);
    return c;
}
static void show_prefs(void) {
    if (!gPrefWin) {
        static const EventTypeSpec wcl[] = {{kEventClassWindow, kEventWindowClose}};
        static const EventTypeSpec hit[] = {{kEventClassControl, kEventControlHit}};
        static const char *const fmn[3] = {"Pixel Perfect", "Fill Screen", "Stretch"};
        WindowRef win = NULL;
        MenuRef ms, mf;
        Rect r;
        CFStringRef t;
        ControlSize sz = kControlSizeSmall;
        SetRect(&r, 100, 100, 550, 246);
        if (CreateNewWindow(kDocumentWindowClass, kWindowCompositingAttribute | kWindowStandardHandlerAttribute | kWindowCloseBoxAttribute, &r, &win) != noErr) return;
        gPrefWin = win;
        InstallWindowEventHandler(win, NewEventHandlerUPP(pref_win_handler), 1, wcl, NULL, NULL);
        SetWindowTitleWithCFString(win, CFSTR("Preferences"));
        wsz_fill();
        pref_label(win, 18, "Window scale:");
        CreateNewMenu((MenuID)gNextMenuId++, 0, &ms);
        for (int i = 0; i < gNWsz; i++) {
            char b[32];
            wsz_title(gWszT10[i], b, sizeof b);
            t = cfs(b);
            AppendMenuItemTextWithCFString(ms, t, 0, 0, NULL);
            CFRelease(t);
        }
        gPrefScale = pref_popup(win, 14, ms, gNWsz, PT_SCALE);
        pref_label(win, 50, "Full screen mode:");
        CreateNewMenu((MenuID)gNextMenuId++, 0, &mf);
        for (int i = 0; i < 3; i++) {
            t = cfs(fmn[i]);
            AppendMenuItemTextWithCFString(mf, t, 0, 0, NULL);
            CFRelease(t);
        }
        gPrefMode = pref_popup(win, 46, mf, 3, PT_MODE);
        SetRect(&r, 150, 78, 420, 96);
        CreateCheckBoxControl(win, &r, CFSTR("Open in full screen"), 0, true, &gPrefOpen);
        if (gPrefOpen) {
            pref_style(gPrefOpen, 0);
            SetControlData(gPrefOpen, kControlEntireControl, kControlSizeTag, sizeof sz, &sz);
            InstallControlEventHandler(gPrefOpen, NewEventHandlerUPP(pref_hit), 1, hit, (void *)(intptr_t)PT_OPEN, NULL);
        }
        pref_label(win, 110, "Game folder:");
        SetRect(&r, 150, 110, 334, 126);
        CreateStaticTextControl(win, &r, CFSTR(""), NULL, &gPrefFolder);
        if (gPrefFolder) {
            TruncCode tc = truncMiddle;
            pref_style(gPrefFolder, 0);
            SetControlData(gPrefFolder, kControlEntireControl, kControlStaticTextTruncTag, sizeof tc, &tc);
        }
        {
            ControlRef ch = NULL;
            t = cfs("Change\x85");
            SetRect(&r, 344, 108, 428, 128);
            if (CreatePushButtonControl(win, &r, t, &ch) == noErr && ch) {
                pref_style(ch, 0);
                SetControlData(ch, kControlEntireControl, kControlSizeTag, sizeof sz, &sz);
                InstallControlEventHandler(ch, NewEventHandlerUPP(pref_hit), 1, hit, (void *)(intptr_t)PT_FOLDER, NULL);
            }
            CFRelease(t);
        }
        if (!gPrefScale || !gPrefMode || !gPrefOpen || !gPrefFolder) { DisposeWindow(win); gPrefWin = NULL; return; }
        {                                                      /* margin of 16: content used to go from (10,14) to (428,128) */
            HIViewRef root = NULL, v;
            if (HIViewFindByID(HIViewGetRoot(win), kHIViewWindowContentID, &root) == noErr && root)
                for (v = HIViewGetFirstSubview(root); v; v = HIViewGetNextView(v)) HIViewMoveBy(v, 6, 2);
        }
        RepositionWindow(win, NULL, kWindowCenterOnMainScreen);
    }
    prefs_refresh();
    ShowWindow(gPrefWin);
    SelectWindow(gPrefWin);
}

static void start_timer(EventLoopTimerRef t, void *ud) {
    (void)ud;
    RemoveEventLoopTimer(t);
    EnableMenuCommand(NULL, kHICommandPreferences);
    {                                                          /* system Help menu > Grand Prix Manager 2 Help (Tiger only shows it if the app asks) */
        MenuRef hm = NULL; MenuItemIndex hi;
        if (HMGetHelpMenu(&hm, NULL) == noErr && hm) {
            gHelpMid = GetMenuID(hm);
            if (GetIndMenuItemWithCommandID(hm, kHICommandAppHelp, 1, NULL, NULL) != noErr) {
                AppendMenuItemTextWithCFString(hm, CFSTR("Grand Prix Manager 2 Help"), 0, kHICommandAppHelp, &hi);
                SetMenuItemCommandKey(hm, hi, false, '?');
            }
        }
    }
    {                                                          /* "About Grand Prix Manager 2" in the application menu (Carbon only creates it if the item exists) */
        MenuRef am = NULL; MenuItemIndex ai;
        EnableMenuCommand(NULL, kHICommandAbout);
        if (GetIndMenuItemWithCommandID(NULL, kHICommandAbout, 1, NULL, NULL) != noErr && GetIndMenuItemWithCommandID(NULL, kHICommandHide, 1, &am, &ai) == noErr && am) {
            InsertMenuItemTextWithCFString(am, CFSTR("About Grand Prix Manager 2"), 0, 0, kHICommandAbout);
            InsertMenuItemTextWithCFString(am, CFSTR(""), 1, kMenuItemAttrSeparator, 0);
        }
    }
    if (!resolve_files()) { QuitApplicationEventLoop(); return; }
    fprintf(stderr, "[host] game: %s\n[host] exe: %s\n[host] writable: %s\n", gCfg.game, gCfg.exe, gCfg.wdir);
    if (!gCfg.out) gCfg.out = "/tmp/gpm2-out";
    if (gpm2_start(&gCfg, &gThread)) {
        host_message_box("Could not start the game", "Run again with GPM2_LOG=1 (or --log) and see ~/Library/Logs/GPM2/run.log for details.", 0x10);
        exit(2);
    }
}

/* exits via exit() (win32_exit): saves the pending preferences (ViewScale, WinX/WinY) */
static void prefs_flush(void) { CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication); }

int main(int argc, char **argv) {
    static const EventTypeSpec mouse[] = {{kEventClassMouse, kEventMouseDown}, {kEventClassMouse, kEventMouseUp},
                                          {kEventClassMouse, kEventMouseMoved}, {kEventClassMouse, kEventMouseDragged}};
    static const EventTypeSpec keys[] = {{kEventClassKeyboard, kEventRawKeyDown}, {kEventClassKeyboard, kEventRawKeyUp},
                                         {kEventClassKeyboard, kEventRawKeyRepeat}, {kEventClassKeyboard, kEventRawKeyModifiersChanged}};
    static const EventTypeSpec cmds[] = {{kEventClassCommand, kEventProcessCommand}};
    EventLoopTimerRef tm;
    char logp[1100];
    float cli_scale = 0, fit;
    int iv;
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    memset(&gCfg, 0, sizeof gCfg);
    gCfg.stack_mb = 32;                                        /* iBook G4: guest thread stack much smaller than on a modern Mac */
    opt_idle_exit = 0;
    for (int i = 1; i < argc; i++) {                            /* only our own options; -psn_ is from the Finder */
        const char *a = argv[i];
        if (i + 1 >= argc) break;
        if (!strcmp(a, "--game")) gCfg.game = argv[++i];
        else if (!strcmp(a, "--exe")) gCfg.exe = argv[++i];
        else if (!strcmp(a, "--cd")) gCfg.cd = argv[++i];
        else if (!strcmp(a, "--write-dir")) gCfg.wdir = argv[++i];
        else if (!strcmp(a, "--out")) gCfg.out = argv[++i];
        else if (!strcmp(a, "--scale")) cli_scale = (float)atof(argv[++i]);
        else if (!strcmp(a, "--host-stack-mb")) gCfg.stack_mb = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--fixed-time")) { opt_fixed_time = (uint32_t)strtoul(argv[++i], 0, 0); opt_fixed_time_set = 1; }
    }
    {
        int log_on = getenv("GPM2_LOG") && getenv("GPM2_LOG")[0] == '1';   /* log off by default: GPM2_LOG=1 or --log */
        for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--log")) log_on = 1;
        if (log_on) {
            snprintf(logp, sizeof logp, "%s/Library/Logs/GPM2", home);
            mkdir(logp, 0755);
            strncat(logp, "/run.log", sizeof logp - strlen(logp) - 1);
            freopen(logp, "w", stderr);
            setvbuf(stderr, NULL, _IOLBF, 0);
            win32_slow_trace = 1;
        } else freopen("/dev/null", "w", stderr);
    }

    fit = (float)((int)CGDisplayPixelsHigh(CGMainDisplayID()) - 120) / GAME_H;
    gMaxScale = (float)((int)CGDisplayPixelsWide(CGMainDisplayID()) - 20) / GAME_W;
    if (fit < gMaxScale) gMaxScale = fit;
    gMaxScale = floorf(gMaxScale * 2.0f) / 2.0f;                /* steps of 0.5 */
    if (gMaxScale < 1) gMaxScale = 1;
    if (cli_scale >= 0.5f) gScale = cli_scale;
    else {
        float s = 0;
        CFPropertyListRef v = CFPreferencesCopyAppValue(CFSTR("ViewScale"), kCFPreferencesCurrentApplication);
        if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberFloatType, &s);
        if (v) CFRelease(v);
        gScale = s >= 0.5f ? s : (gMaxScale >= 2 ? 2 : 1);
    }
    {
        Rect ar;                                                /* Zoom to Fit Screen: largest scale that fits the usable area (minus the title bar) */
        float fw, fh;
        GetAvailableWindowPositioningBounds(GetMainDevice(), &ar);
        fw = (float)(ar.right - ar.left) / GAME_W; fh = (float)(ar.bottom - ar.top - 22) / GAME_H;
        gFitScale = floorf((fw < fh ? fw : fh) * 100.0f) / 100.0f;
        if (gFitScale < 1) gFitScale = 1;
    }
    if (gScale > (gMaxScale > gFitScale ? gMaxScale : gFitScale)) gScale = gMaxScale > gFitScale ? gMaxScale : gFitScale;
    if (load_int(CFSTR("FullScreenMode"), &iv) && iv >= 0 && iv <= 2) gFullMode = iv;
    if (load_int(CFSTR("OpenFullScreen"), &iv)) gOpenFull = iv != 0;
    atexit(prefs_flush);
    gMainThread = pthread_self();
    InitCursor();
    gFull = gOpenFull;
    img_update();
    if (!w_create(0xFFFFFFFFu, NULL, 1)) { fprintf(stderr, "no window\n"); return 1; }
    ShowWindow(gW[0].ref);
    if (gFull) SetSystemUIMode(kUIModeAllHidden, kUIOptionAutoShowMenuBar);
    InstallApplicationEventHandler(NewEventHandlerUPP(mouse_handler), 4, mouse, NULL, NULL);
    InstallApplicationEventHandler(NewEventHandlerUPP(key_handler), 4, keys, NULL, NULL);
    InstallApplicationEventHandler(NewEventHandlerUPP(cmd_handler), 1, cmds, NULL, NULL);
    InstallEventLoopTimer(GetMainEventLoop(), 0.1, 0.01, NewEventLoopTimerUPP(tick), NULL, &tm);
    InstallEventLoopTimer(GetMainEventLoop(), 0.2, 0, NewEventLoopTimerUPP(start_timer), NULL, &tm);
    RunApplicationEventLoop();
    return 0;
}
