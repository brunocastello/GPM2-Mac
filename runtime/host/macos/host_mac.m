/* macOS (AppKit) backend of the Win32 runtime.
 *
 * Threads: AppKit runs on the main thread; the guest runs on a pthread (gpm2_start). Frames, windows, menus and
 * MessageBox go from the guest to the main thread (dispatch); mouse, keyboard and menus come back through the
 * core queue (host_push_event). Nothing here touches the guest state.
 *
 * Windows: the game's main window is the NSWindow `gWin`. Each other top-level guest window becomes a native
 * NSPanel, a child of `gWin` (addChildWindow), with the content at the same scale as the main one and positioned by the
 * Win32 offset between the client areas.
 *
 * Display: resizing the window is free, with the aspect ratio locked (nearest neighbor if the pixel scale is an integer,
 * sharp bilinear otherwise); the View menu snaps to 1x, 1.5x, 2x...; native full screen in "Pixel Perfect" (largest integer scale,
 * black borders), "Fill Screen" (largest scale keeping the aspect ratio) or "Stretch" (fills the screen ignoring the aspect ratio).
 */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <CommonCrypto/CommonDigest.h>

#include "compat_mac.h"
#include "../host.h"
#include "../hostname_posix.h"
#include "../../gpm2run.h"
#include "../../hooks/hooks.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <mach/mach_time.h>

extern int opt_idle_exit, opt_fixed_time_set;
extern uint32_t opt_fixed_time;

#define GAME_W 640
#define GAME_H 460
#define EXE_SHA256 @"266f214714b6a2d3396f969f0e142dc267b63d80beb255b9998f66ffcd253546"
#define WIN_STYLE (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)

static GpmConfig gCfg;
static NSWindow *gWin;
static pthread_t gThread;

/* display */
static double gScale = 1;                 /* window scale (1..gMaxScale; free when dragging the border) */
static double gMaxScale = 1;              /* largest scale (keeping the aspect ratio) that fits the usable screen area */
static int gFullMode;                     /* full screen: 0 = Pixel Perfect, 1 = Fill Screen, 2 = Stretch */
static const char *gCliFix[16]; static int gNCliFix;   /* --fix BUG-xxx=on|off: this run only */
static int gCliScale, gCliFull, gCliFill; /* --scale N, --fullscreen, --fill: apply to this run only */
static NSCursor *gCursor;
static BOOL gCursorHidden;
static BOOL gLegacyFull;                  /* 10.6 full screen (no native one): frameless window over the whole screen, Tiger style */
static BOOL is_full(void) { return gWin && ((gWin.styleMask & GPM_STYLE_FULLSCREEN) || gLegacyFull); }

static NSCursor *blank_cursor(void) {
    static NSCursor *c;
    if (!c) c = [[NSCursor alloc] initWithImage:[[[NSImage alloc] initWithSize:NSMakeSize(1, 1)] autorelease] hotSpot:NSZeroPoint];   /* static: owned once */
    return c;
}
static void draw_hourglass(void) {
    NSBezierPath *p = [NSBezierPath bezierPath];
    [p moveToPoint:NSMakePoint(3, 1)]; [p lineToPoint:NSMakePoint(13, 1)]; [p lineToPoint:NSMakePoint(8.5, 8)];
    [p lineToPoint:NSMakePoint(13, 15)]; [p lineToPoint:NSMakePoint(3, 15)]; [p lineToPoint:NSMakePoint(7.5, 8)];
    [p closePath];
    [[NSColor whiteColor] setFill]; [p fill];
    [[NSColor blackColor] setStroke]; [p setLineWidth:1.2]; [p stroke];
}
static NSCursor *hourglass_cursor(void) {
    static NSCursor *c;
    if (!c) {
        NSImage *im;
        GPM_NEWER_BEGIN
        if ([NSImage respondsToSelector:@selector(imageWithSize:flipped:drawingHandler:)])      /* 10.8+: resolution independent */
            im = [NSImage imageWithSize:NSMakeSize(16, 16) flipped:NO drawingHandler:^BOOL(NSRect r) { draw_hourglass(); return YES; }];
        else {
            im = [[[NSImage alloc] initWithSize:NSMakeSize(16, 16)] autorelease];
            [im lockFocus]; draw_hourglass(); [im unlockFocus];
        }
        GPM_NEWER_END
        c = [[NSCursor alloc] initWithImage:im hotSpot:NSMakePoint(8, 8)];
    }
    return c;
}
static NSCursor *cursor_for(uint32_t idc) {
    switch (idc) {
    case 0: return blank_cursor();
    case 0x7F01: return [NSCursor IBeamCursor];
    case 0x7F02: case 0x7F8A: return hourglass_cursor();
    case 0x7F03: return [NSCursor crosshairCursor];
    case 0x7F84: return [NSCursor resizeLeftRightCursor];
    case 0x7F85: return [NSCursor resizeUpDownCursor];
    case 0x7F86: return [NSCursor closedHandCursor];
    case 0x7F88: return [NSCursor operationNotAllowedCursor];
    case 0x7F89: return [NSCursor pointingHandCursor];
    default: return [NSCursor arrowCursor];                   /* IDC_ARROW and those with no equivalent */
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* key table: kVK_* -> Windows virtual key                                                */
/* ---------------------------------------------------------------------------------------------- */
static int vk_from_keycode(unsigned short kc) {
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
static int cp1252_char(unichar u) {
    if (u == 0x7F) return 8;                                  /* Mac Delete (backspace) */
    if (u == 3) return 13;                                    /* numeric keypad Enter */
    if (u < 0x80) return u;
    if (u >= 0xF700 && u <= 0xF8FF) return 0;                 /* function/arrow keys */
    NSData *d = [[NSString stringWithCharacters:&u length:1] dataUsingEncoding:NSWindowsCP1252StringEncoding allowLossyConversion:NO];
    return d.length == 1 ? ((const unsigned char *)d.bytes)[0] : 0;
}
static unsigned mods_of(NSEventModifierFlags f) {
    return ((f & NSEventModifierFlagShift) ? HOST_MOD_SHIFT : 0u) | ((f & NSEventModifierFlagControl) ? HOST_MOD_CTRL : 0u) |
           ((f & NSEventModifierFlagOption) ? HOST_MOD_ALT : 0u);
}

/* ---------------------------------------------------------------------------------------------- */
/* the game view (the main window and each NSPanel have one)                                      */
/* ---------------------------------------------------------------------------------------------- */
static CGColorSpaceRef srgb(void) {
    static CGColorSpaceRef cs;
    if (!cs) cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    return cs;
}
/* upscales by nearest neighbor (integer k); returns +1 */
static CGImageRef upscale_nearest(CGImageRef src, int k) {
    size_t w = CGImageGetWidth(src) * (size_t)k, h = CGImageGetHeight(src) * (size_t)k;
    CGContextRef c = CGBitmapContextCreate(NULL, w, h, 8, w * 4, srgb(), (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
    if (!c) return NULL;
    CGContextSetInterpolationQuality(c, kCGInterpolationNone);
    CGContextDrawImage(c, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), src);
    CGImageRef r = CGBitmapContextCreateImage(c);
    CGContextRelease(c);
    return r;
}

@interface GameView : NSView
@property(nonatomic) uint32_t hwnd;                          /* guest HWND (0 = main window) */
@property(nonatomic) BOOL isMain;
@property(nonatomic) int gw, gh;                             /* client area size in game pixels */
@property(nonatomic) NSEventModifierFlags lastFlags;
@property(nonatomic) int buttons;                            /* mask of the pressed buttons */
@property(nonatomic) BOOL ctrlClick;
- (instancetype)initWithFrame:(NSRect)f hwnd:(uint32_t)h main:(BOOL)m gw:(int)gw gh:(int)gh;
- (void)setImage:(CGImageRef)img;
- (void)relayout;
- (NSRect)imgRect;
- (CGFloat)scale;
@end

static NSMutableArray<GameView *> *gViews;                   /* all views (cursor) */
static void refresh_cursors(void) {
    for (GameView *v in gViews) {
        [v.window invalidateCursorRectsForView:v];
        NSPoint p = [v convertPoint:[v.window mouseLocationOutsideOfEventStream] fromView:nil];
        if (v.window.isKeyWindow && NSPointInRect(p, v.bounds)) [(gCursorHidden ? blank_cursor() : (gCursor ?: [NSCursor arrowCursor])) set];
    }
}

@implementation GameView {
    CALayer *_imgLayer;
    CGImageRef _raw, _up;                                    /* original frame and the upscaled one (sharp bilinear) */
    int _upK;
    BOOL _upValid;
}
- (instancetype)initWithFrame:(NSRect)f hwnd:(uint32_t)h main:(BOOL)m gw:(int)gw gh:(int)gh {
    self = [super initWithFrame:f];
    if (self) {
        _hwnd = h; _isMain = m; _gw = gw; _gh = gh;
        self.wantsLayer = YES;
        self.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        _imgLayer = [[CALayer layer] retain];
        _imgLayer.contentsGravity = kCAGravityResize;
        _imgLayer.opaque = YES;
        _imgLayer.anchorPoint = CGPointZero;
        [self.layer addSublayer:_imgLayer];
        [self addTrackingArea:[[[NSTrackingArea alloc] initWithRect:NSZeroRect
            options:NSTrackingMouseMoved | NSTrackingActiveInActiveApp | NSTrackingInVisibleRect | NSTrackingCursorUpdate owner:self userInfo:nil] autorelease]];
        if (!gViews) gViews = [[NSMutableArray alloc] init];   /* static: owned once */
        [gViews addObject:self];
    }
    return self;
}
- (void)dealloc {
    [gViews removeObject:self];
    if (_raw) CGImageRelease(_raw);
    if (_up) CGImageRelease(_up);
    [_imgLayer release];
    [super dealloc];
}
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (BOOL)isOpaque { return YES; }

/* Scale (points per game pixel). Window: the view's own (the window always keeps the game's aspect ratio).
 * Full screen: Pixel Perfect = largest integer in physical pixels; Fill = largest keeping the aspect ratio. */
- (CGFloat)scale {
    NSSize b = self.bounds.size;
    if (!_isMain) return _gw > 0 ? b.width / _gw : 1;
    CGFloat bs = gpm_backing_scale(self.window), s = MIN(b.width / _gw, b.height / _gh);
    if (is_full() && gFullMode == 0) s = floor(s * bs + 1e-6) / bs;
    return s < 1 ? 1 : s;
}
- (NSRect)imgRect {
    if (!_isMain) return self.bounds;
    if (is_full() && gFullMode == 2) return self.bounds;     /* Stretch: ignores the aspect ratio */
    CGFloat s = [self scale], w = _gw * s, h = _gh * s;
    return NSMakeRect(floor((self.bounds.size.width - w) / 2), floor((self.bounds.size.height - h) / 2), w, h);
}
/* 10.6: changing the window style (full screen) makes AppKit replace the view's backing layer, dropping the image layer
 * (white window). Re-attach it whenever the backing layer changed; on 10.7+ this is a no-op. */
- (void)attachLayers {
    CALayer *l = self.layer;
    if (!l || _imgLayer.superlayer == l) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [_imgLayer removeFromSuperlayer];
    l.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    [l addSublayer:_imgLayer];
    [CATransaction commit];
}
- (void)viewDidMoveToWindow { [super viewDidMoveToWindow]; [self attachLayers]; }
- (void)applyImage {
    [self attachLayers];
    if (!_raw) return;
    CGFloat bs = gpm_backing_scale(self.window);
    NSRect ir = [self imgRect];
    CGFloat sx = ir.size.width / _gw * bs, sy = ir.size.height / _gh * bs, sp = MIN(sx, sy);
    BOOL integer = fabs(sx - round(sx)) < 0.01 && fabs(sy - round(sy)) < 0.01;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (integer) {                                           /* integer scale: nearest neighbor */
        _imgLayer.magnificationFilter = _imgLayer.minificationFilter = kCAFilterNearest;
        _imgLayer.contents = (__bridge id)_raw;
    } else {                                                 /* sharp bilinear: integer part by nearest neighbor, the rest by bilinear */
        int k = (int)floor(sp);
        if (k < 1) k = 1;
        if (k > 1) {
            if (!_upValid || _upK != k) {
                if (_up) CGImageRelease(_up);
                _up = upscale_nearest(_raw, k); _upK = k; _upValid = _up != NULL;
            }
        } else _upValid = NO;
        _imgLayer.magnificationFilter = _imgLayer.minificationFilter = kCAFilterLinear;
        _imgLayer.contents = (__bridge id)(k > 1 && _upValid ? _up : _raw);
    }
    [CATransaction commit];
}
- (void)relayout {
    [self attachLayers];
    NSRect r = [self imgRect];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _imgLayer.frame = NSMakeRect(r.origin.x, r.origin.y, r.size.width, r.size.height);
    [CATransaction commit];
    _upValid = NO;
    [self applyImage];
}
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; [self relayout]; }
/* layout and viewDidChangeBackingProperties are only called from 10.7 on */
- (void)layout {
    GPM_NEWER_BEGIN
    [super layout];
    GPM_NEWER_END
    [self relayout];
}
- (void)viewDidChangeBackingProperties {
    GPM_NEWER_BEGIN
    [super viewDidChangeBackingProperties];
    GPM_NEWER_END
    _imgLayer.contentsScale = gpm_backing_scale(self.window);
    [self relayout];
}
- (void)setImage:(CGImageRef)img {
    if (_raw) CGImageRelease(_raw);
    _raw = CGImageRetain(img);
    _upValid = NO;
    [self applyImage];
}

- (void)resetCursorRects { [self addCursorRect:self.bounds cursor:gCursorHidden ? blank_cursor() : (gCursor ?: [NSCursor arrowCursor])]; }
- (void)cursorUpdate:(NSEvent *)e { [(gCursorHidden ? blank_cursor() : (gCursor ?: [NSCursor arrowCursor])) set]; }

/* ---- mouse ---- */
- (BOOL)gamePoint:(NSEvent *)e x:(int *)x y:(int *)y {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSRect r = [self imgRect];
    *x = (int)floor((p.x - r.origin.x) / (r.size.width / _gw));
    *y = (int)floor((p.y - r.origin.y) / (r.size.height / _gh));
    return *x >= 0 && *x < _gw && *y >= 0 && *y < _gh;
}
- (void)sendMove:(NSEvent *)e {
    HostEvent h; int x, y;
    BOOL inside = [self gamePoint:e x:&x y:&y];
    if (!inside && !_buttons) return;                         /* outside the image it only counts during a drag */
    memset(&h, 0, sizeof h);
    h.type = HOST_EV_MOUSEMOVE; h.win = _hwnd; h.x = x; h.y = y; h.mods = mods_of(e.modifierFlags);
    host_push_event(&h);
}
- (void)sendButton:(int)btn down:(BOOL)down event:(NSEvent *)e {
    HostEvent h; int x, y;
    BOOL inside = [self gamePoint:e x:&x y:&y];
    if (down && !inside) return;
    if (!down && !(_buttons & (1 << btn))) return;
    if (down) _buttons |= 1 << btn; else _buttons &= ~(1 << btn);
    memset(&h, 0, sizeof h);
    h.type = HOST_EV_BUTTON; h.win = _hwnd; h.a = btn; h.b = down; h.x = x; h.y = y; h.mods = mods_of(e.modifierFlags);
    host_push_event(&h);
}
- (void)mouseMoved:(NSEvent *)e { [self sendMove:e]; }
- (void)mouseDragged:(NSEvent *)e { [self sendMove:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self sendMove:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self sendMove:e]; }
- (void)mouseDown:(NSEvent *)e {
    if (e.modifierFlags & NSEventModifierFlagControl) { [self sendButton:1 down:YES event:e]; _ctrlClick = YES; }   /* Ctrl+click = right button */
    else { _ctrlClick = NO; [self sendButton:0 down:YES event:e]; }
}
- (void)mouseUp:(NSEvent *)e { [self sendButton:_ctrlClick ? 1 : 0 down:NO event:e]; _ctrlClick = NO; }
- (void)rightMouseDown:(NSEvent *)e { [self sendButton:1 down:YES event:e]; }
- (void)rightMouseUp:(NSEvent *)e { [self sendButton:1 down:NO event:e]; }
- (void)otherMouseDown:(NSEvent *)e { [self sendButton:2 down:YES event:e]; }
- (void)otherMouseUp:(NSEvent *)e { [self sendButton:2 down:NO event:e]; }
- (NSMenu *)menuForEvent:(NSEvent *)e { return nil; }

/* ---- teclado ---- */
- (void)sendKey:(NSEvent *)e down:(BOOL)down {
    HostEvent h; int vk = vk_from_keycode(e.keyCode);
    if (!vk) return;
    memset(&h, 0, sizeof h);
    h.type = HOST_EV_KEY; h.a = vk; h.b = down; h.c = down && e.isARepeat; h.mods = mods_of(e.modifierFlags);
    if (down) {
        NSString *s = (e.modifierFlags & NSEventModifierFlagOption) ? e.charactersIgnoringModifiers : e.characters;
        if (s.length > 0) h.ch = cp1252_char([s characterAtIndex:0]);
    }
    host_push_event(&h);
}
- (void)keyDown:(NSEvent *)e { if (!(e.modifierFlags & NSEventModifierFlagCommand)) [self sendKey:e down:YES]; }
- (void)keyUp:(NSEvent *)e { [self sendKey:e down:NO]; }
- (void)flagsChanged:(NSEvent *)e {
    static const struct { NSEventModifierFlags f; int vk; } m[] = {
        {NSEventModifierFlagShift, 0x10}, {NSEventModifierFlagControl, 0x11}, {NSEventModifierFlagOption, 0x12}, {NSEventModifierFlagCapsLock, 0x14}};
    NSEventModifierFlags now = e.modifierFlags, old = _lastFlags;
    _lastFlags = now;
    for (unsigned i = 0; i < 4; i++) {
        BOOL a = (now & m[i].f) != 0, b = (old & m[i].f) != 0;
        if (a == b) continue;
        HostEvent h;
        memset(&h, 0, sizeof h);
        h.type = HOST_EV_KEY; h.a = m[i].vk; h.b = m[i].vk == 0x14 ? 1 : a; h.mods = mods_of(now);
        host_push_event(&h);
        if (m[i].vk == 0x14) { h.b = 0; host_push_event(&h); }   /* Mac Caps Lock only reports the change: generates the pair */
    }
}
- (BOOL)performKeyEquivalent:(NSEvent *)e { return NO; }
@end

/* ---------------------------------------------------------------------------------------------- */
/* secondary windows (NSPanel), scale and menus                                                    */
/* ---------------------------------------------------------------------------------------------- */
static GameView *gView;                                      /* the main window's view */
static NSMutableArray<NSMenuItem *> *gGameItems;
static NSMutableDictionary<NSNumber *, NSPanel *> *gPanels;  /* HWND -> NSPanel (main thread only) */
static NSMutableDictionary<NSNumber *, NSData *> *gInfos;    /* HWND -> HostWindow */
static HostWindow gMainInfo;
static BOOL gHaveMain;
static NSPanel *gPrefs;

/* the main window must stay key when it is frameless (10.6 full screen) */
@interface MainWindow : NSWindow
@end
@implementation MainWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (void)gameMenu:(NSMenuItem *)it;
- (void)showHelp:(id)sender;
- (void)requestClose:(id)sender;
- (void)setScaleFromMenu:(NSMenuItem *)it;
- (void)zoomToFit:(id)sender;
- (void)toggleFull:(id)sender;
- (void)setFullMode:(NSMenuItem *)it;
@end
static AppDelegate *gDelegate;

static void push_close(uint32_t win) {
    HostEvent h;
    memset(&h, 0, sizeof h);
    h.type = HOST_EV_CLOSE; h.win = win;
    host_push_event(&h);
}

/* largest scale (with the game's aspect ratio) whose window, with the title bar, fits the usable screen area */
static double max_scale_for(NSScreen *scr) {
    NSRect vf = (scr ?: [NSScreen mainScreen]).visibleFrame;
    NSRect f0 = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, GAME_W, GAME_H) styleMask:WIN_STYLE];
    double ew = f0.size.width - GAME_W, eh = f0.size.height - GAME_H;
    double m = MIN((vf.size.width - ew) / GAME_W, (vf.size.height - eh) / GAME_H);
    return m < 1 ? 1 : floor(m * 100) / 100;
}

/* positions each NSPanel: the client-area corner sits at the same offset (in game pixels, times the
 * scale) that the guest window has relative to the main client area */
static void layout_panels(void) {
    if (!gHaveMain || !gWin || !gView.window) return;
    NSRect ir = [gView imgRect];
    CGFloat s = ir.size.width / GAME_W, sy = ir.size.height / GAME_H;
    NSPoint tl = gpm_to_screen(gWin, [gView convertPoint:ir.origin toView:nil]);   /* top-left corner of the image */
    for (NSNumber *k in gPanels) {
        NSPanel *p = [gPanels objectForKey:k];
        HostWindow w;
        [[gInfos objectForKey:k] getBytes:&w length:sizeof w];
        CGFloat cw = w.cw * s, ch = w.ch * sy;
        CGFloat x = tl.x + ((w.x + w.cx) - (gMainInfo.x + gMainInfo.cx)) * s;
        CGFloat top = tl.y - ((w.y + w.cy) - (gMainInfo.y + gMainInfo.cy)) * sy;
        NSRect cr = NSMakeRect(round(x), round(top - ch), round(cw), round(ch));
        NSRect fr = [p frameRectForContentRect:cr];
        if (!NSEqualRects(fr, p.frame)) [p setFrame:fr display:YES];
    }
}
static void order_panels(void) {
    NSArray *keys = [gPanels.allKeys sortedArrayUsingComparator:^NSComparisonResult(NSNumber *a, NSNumber *b) {
        HostWindow x, y;
        [[gInfos objectForKey:a] getBytes:&x length:sizeof x]; [[gInfos objectForKey:b] getBytes:&y length:sizeof y];
        if (x.topmost != y.topmost) return x.topmost < y.topmost ? NSOrderedAscending : NSOrderedDescending;
        return x.z < y.z ? NSOrderedAscending : x.z > y.z ? NSOrderedDescending : NSOrderedSame;
    }];
    for (NSNumber *k in keys) if ([[gPanels objectForKey:k] isVisible]) [[gPanels objectForKey:k] orderFront:nil];
}
static GameView *view_for_hwnd(uint32_t id) {
    if (gHaveMain && id == gMainInfo.id) return gView;
    return (GameView *)[[gPanels objectForKey:@(id)] contentView];
}

/* guest top-level window (created, moved, shown, hidden...) -> main thread */
void *gpm_chat_snapshot(void);                                  /* dialogs_mac.m */
void gpm_chat_restore(void *chat);
static void apply_window_inner(HostWindow w);
static void apply_window(HostWindow w) {                        /* a child window that appears or changes brings the main one to the front, above the chat */
    void *chat = gpm_chat_snapshot();
    apply_window_inner(w);
    gpm_chat_restore(chat);
}
static void apply_window_inner(HostWindow w) {
    NSNumber *key = @(w.id);
    if (!gInfos) { gInfos = [[NSMutableDictionary alloc] init]; gPanels = [[NSMutableDictionary alloc] init]; }
    [gInfos setObject:[NSData dataWithBytes:&w length:sizeof w] forKey:key];
    if (w.main) { gMainInfo = w; gHaveMain = YES; layout_panels(); return; }
    NSPanel *p = [gPanels objectForKey:key];
    if (!w.visible || w.cw <= 0 || w.ch <= 0) { [p orderOut:nil]; return; }
    NSString *title = [NSString stringWithCString:w.title encoding:NSWindowsCP1252StringEncoding] ?: @"";
    if (!p) {
        NSWindowStyleMask st = w.caption ? (NSWindowStyleMaskTitled | (w.sysmenu ? NSWindowStyleMaskClosable : 0)) : NSWindowStyleMaskBorderless;
        p = [[[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, w.cw, w.ch) styleMask:st backing:NSBackingStoreBuffered defer:NO] autorelease];
        p.releasedWhenClosed = NO;
        p.hidesOnDeactivate = NO;
        p.becomesKeyOnlyIfNeeded = NO;
        p.acceptsMouseMovedEvents = YES;
        p.delegate = gDelegate;
        if (gpm_native_fullscreen()) p.collectionBehavior = GPM_FS_AUXILIARY;
        GameView *v = [[[GameView alloc] initWithFrame:NSMakeRect(0, 0, w.cw, w.ch) hwnd:w.id main:NO gw:w.cw gh:w.ch] autorelease];
        p.contentView = v;
        [gPanels setObject:p forKey:key];
        [gWin addChildWindow:p ordered:NSWindowAbove];        /* follows the main window; always stays above it */
    }
    GameView *v = (GameView *)p.contentView;
    if (v.gw != w.cw || v.gh != w.ch) { v.gw = w.cw; v.gh = w.ch; }
    p.title = title;
    p.floatingPanel = w.topmost != 0;                         /* HWND_TOPMOST: floats above the others */
    layout_panels();
    if (!p.isVisible) [p orderFront:nil];
    order_panels();
}
static void remove_window(uint32_t id) {
    NSNumber *key = @(id);
    NSPanel *p = [[[gPanels objectForKey:key] retain] autorelease];       /* alive until the end of this event */
    if (p) { [gViews removeObject:p.contentView]; [gWin removeChildWindow:p]; [p orderOut:nil]; [p close]; [gPanels removeObjectForKey:key]; }
    [gInfos removeObjectForKey:key];
    if (gHaveMain && id == gMainInfo.id) gHaveMain = NO;
}

static void keep_on_screen(NSWindow *w) {
    NSRect vf = (w.screen ?: [NSScreen mainScreen]).visibleFrame, f = w.frame;
    if (NSMaxY(f) > NSMaxY(vf)) f.origin.y = NSMaxY(vf) - f.size.height;
    if (f.origin.y < vf.origin.y) f.origin.y = vf.origin.y;
    if (NSMaxX(f) > NSMaxX(vf)) f.origin.x = NSMaxX(vf) - f.size.width;
    if (f.origin.x < vf.origin.x) f.origin.x = vf.origin.x;
    if (!NSEqualRects(f, w.frame)) [w setFrame:f display:YES];
}
/* changes the content size keeping the top-left corner */
static void resize_content(CGFloat w, CGFloat h) {
    NSRect f = gWin.frame, nf = [gWin frameRectForContentRect:NSMakeRect(0, 0, w, h)];
    nf.origin.x = f.origin.x;
    nf.origin.y = f.origin.y + f.size.height - nf.size.height;
    [gWin setFrame:nf display:YES animate:NO];
    keep_on_screen(gWin);
}
/* the size items go up to 2x even if the usable screen does not fit it (the window is kept visible by keep_on_screen) */
static double menu_max_scale(void) { return gMaxScale < 2 ? 2 : gMaxScale; }
static void refresh_prefs(void);
/* Full screen without the native one (10.6): like the Tiger/PPC build, a frameless window covers the whole main screen, the Dock is
 * hidden and the menu bar auto-hides (shows when the mouse hits the top); black background, no resolution change.
 * Pixel Perfect / Fill / Stretch: see GameView imgRect. */
static NSRect gSavedFrame;
static void layout_panels(void);
static void legacy_full(BOOL on) {
    if (!gWin || on == gLegacyFull) return;
    gLegacyFull = on;
    if (on) {
        gSavedFrame = gWin.frame;
        [gWin setStyleMask:NSWindowStyleMaskBorderless];
        [gWin setFrame:[[NSScreen mainScreen] frame] display:YES animate:NO];
        [NSApp setPresentationOptions:NSApplicationPresentationHideDock | NSApplicationPresentationAutoHideMenuBar];
        [gWin setBackgroundColor:[NSColor blackColor]];
    } else {
        [NSApp setPresentationOptions:NSApplicationPresentationDefault];
        [gWin setStyleMask:WIN_STYLE];
        [gWin setBackgroundColor:[NSColor windowBackgroundColor]];
        [gWin setFrame:gSavedFrame display:YES animate:NO];
        NSString *t = [[gWin.title copy] autorelease];         /* 10.6: the rebuilt title bar comes back empty; set the title again */
        gWin.title = @"";
        gWin.title = t.length ? t : @"Grand Prix Manager 2";
    }
    [gWin makeKeyAndOrderFront:nil];
    [gWin makeFirstResponder:gView];
    [gView setWantsLayer:YES];                                /* the style change may have dropped the backing layer */
    [gView relayout];
    layout_panels();
}
static void apply_scale(double s, BOOL persist);
static void toggle_full_screen(void) {
    if (gpm_native_fullscreen()) {
        GPM_NEWER_BEGIN
        [gWin toggleFullScreen:nil];
        GPM_NEWER_END
    } else {
        legacy_full(!gLegacyFull);
        if (!gLegacyFull) apply_scale(gScale, NO);
        refresh_prefs();
    }
}
static void apply_scale(double s, BOOL persist) {
    gScale = s < 1 ? 1 : s > menu_max_scale() ? menu_max_scale() : s;
    if (persist) [[NSUserDefaults standardUserDefaults] setDouble:gScale forKey:@"ViewScale"];
    if (gWin && !is_full()) resize_content(GAME_W * gScale, GAME_H * gScale);
    refresh_prefs();
}
static void set_full_mode(int m, BOOL persist) {
    gFullMode = m < 0 ? 0 : m > 2 ? 2 : m;
    if (persist) [[NSUserDefaults standardUserDefaults] setInteger:gFullMode forKey:@"FullScreenMode"];
    [gView relayout];
    layout_panels();
    refresh_prefs();
}

static NSMenuItem *gChatTop;                                  /* Chat menu: grayed out until Link Play is established */
@implementation AppDelegate
- (void)gameMenu:(NSMenuItem *)it {
    NSDictionary *d = it.representedObject;
    HostEvent h;
    memset(&h, 0, sizeof h);
    h.type = HOST_EV_MENU; h.a = [[d objectForKey:@"id"] intValue]; h.b = [[d objectForKey:@"sys"] intValue];
    host_push_event(&h);
}
- (void)chatShow:(id)sender { HostEvent h; memset(&h, 0, sizeof h); h.type = HOST_EV_CHAT; h.a = 0; host_push_event(&h); }
- (void)chatSettings:(id)sender { HostEvent h; memset(&h, 0, sizeof h); h.type = HOST_EV_CHAT; h.a = 1; host_push_event(&h); }
- (void)showAbout:(id)sender {                                            /* same command as the game's Help > About */
    HostEvent h;
    memset(&h, 0, sizeof h);
    h.type = HOST_EV_MENU; h.a = 40029; h.b = 1;
    host_push_event(&h);
}
- (void)showHelp:(id)sender { host_help(-1); }                              /* same path as the game's Help (NSApp showHelp: would look for a book in the bundle) */
- (void)requestClose:(id)sender { push_close(0); }
- (void)setScaleFromMenu:(NSMenuItem *)it { apply_scale(it.tag / 10.0, YES); }          /* tag = scale x 10 */
- (void)zoomToFit:(id)sender { apply_scale(gMaxScale, YES); }
- (void)toggleFull:(id)sender { toggle_full_screen(); }
- (void)setFullMode:(NSMenuItem *)it { set_full_mode((int)it.tag, YES); }
- (BOOL)validateMenuItem:(NSMenuItem *)it {
    SEL a = it.action;
    if (a == @selector(setScaleFromMenu:)) {
        double s = gView.bounds.size.width / GAME_W;
        it.state = !is_full() && fabs(s - it.tag / 10.0) < 0.01 ? NSControlStateValueOn : NSControlStateValueOff;
    } else if (a == @selector(setFullMode:)) it.state = it.tag == gFullMode ? NSControlStateValueOn : NSControlStateValueOff;
    else if (a == @selector(toggleFull:)) it.title = is_full() ? @"Exit Full Screen" : @"Enter Full Screen";
    return YES;
}
- (BOOL)windowShouldClose:(NSWindow *)w {
    push_close(w == gWin ? 0 : ((GameView *)w.contentView).hwnd);
    return NO;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)a { push_close(0); return NSTerminateCancel; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return NO; }
/* resizing: free, but always at the game's aspect ratio (snapping to scales is in the View menu items) */
- (NSSize)windowWillResize:(NSWindow *)w toSize:(NSSize)fs {
    if (w != gWin || is_full()) return fs;
    NSSize c = [w contentRectForFrameRect:NSMakeRect(0, 0, fs.width, fs.height)].size;
    CGFloat s = MIN(c.width / GAME_W, c.height / GAME_H);
    if (s < 1) s = 1;
    return [w frameRectForContentRect:NSMakeRect(0, 0, GAME_W * s, GAME_H * s)].size;
}
- (void)windowDidResize:(NSNotification *)n { if (n.object == gWin) { [gView relayout]; layout_panels(); } }
- (void)windowDidEndLiveResize:(NSNotification *)n {
    if (n.object != gWin || is_full()) return;
    gScale = gView.bounds.size.width / GAME_W;                            /* free size: stores the current scale */
    [[NSUserDefaults standardUserDefaults] setDouble:gScale forKey:@"ViewScale"];
    refresh_prefs();
}
- (void)windowDidEnterFullScreen:(NSNotification *)n { [gView relayout]; layout_panels(); refresh_prefs(); }
- (void)windowDidExitFullScreen:(NSNotification *)n { apply_scale(gScale, NO); [gView relayout]; layout_panels(); }
@end

static NSMenuItem *submenu_item(NSString *title, NSMenu *sub) {
    NSMenuItem *it = [[[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""] autorelease];
    sub.title = title;
    it.submenu = sub;
    return it;
}
static void build_main_menu(void) {
    NSMenu *main = [[[NSMenu alloc] initWithTitle:@""] autorelease];
    NSString *name = @"Grand Prix Manager 2";
    NSMenu *app = [[[NSMenu alloc] initWithTitle:name] autorelease];
    NSMenuItem *ab = [app addItemWithTitle:@"About Grand Prix Manager 2" action:@selector(showAbout:) keyEquivalent:@""];
    ab.target = gDelegate;
    [app addItem:[NSMenuItem separatorItem]];
    NSMenuItem *pr = [app addItemWithTitle:@"Settings…" action:@selector(showPrefs:) keyEquivalent:@","];
    pr.target = gDelegate;
    [app addItem:[NSMenuItem separatorItem]];
    [app addItemWithTitle:@"Hide Grand Prix Manager 2" action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem *ho = [app addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    ho.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [app addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [app addItem:[NSMenuItem separatorItem]];
    NSMenuItem *q = [app addItemWithTitle:@"Quit Grand Prix Manager 2" action:@selector(requestClose:) keyEquivalent:@"q"];
    q.target = gDelegate;
    [main addItem:submenu_item(name, app)];

    NSMenu *view = [[[NSMenu alloc] initWithTitle:@"View"] autorelease];
    NSMenu *wsz = [[[NSMenu alloc] initWithTitle:@"Window Size"] autorelease];
    for (int t10 = 10; t10 <= (int)(menu_max_scale() * 10 + 1e-6); t10 += (t10 == 10 ? 5 : t10 == 15 ? 5 : 10)) {      /* 1x, 1.5x, 2x, 3x ... */
        NSString *t = t10 == 10 ? @"Actual Size (1×)" : t10 % 10 ? [NSString stringWithFormat:@"%.1f×", t10 / 10.0] : [NSString stringWithFormat:@"%d×", t10 / 10];
        NSMenuItem *it = [wsz addItemWithTitle:t action:@selector(setScaleFromMenu:) keyEquivalent:(t10 % 10 == 0 && t10 <= 40) ? [NSString stringWithFormat:@"%d", t10 / 10] : @""];
        it.target = gDelegate; it.tag = t10;
    }
    fprintf(stderr, "[host] View > Window Size: %s (max %.2f)\n", [[wsz.itemArray valueForKey:@"title"] componentsJoinedByString:@", "].UTF8String, gMaxScale);
    [view addItem:submenu_item(@"Window Size", wsz)];
    NSMenuItem *fit = [view addItemWithTitle:@"Zoom to Fit Screen" action:@selector(zoomToFit:) keyEquivalent:@"0"];
    fit.target = gDelegate;
    [view addItem:[NSMenuItem separatorItem]];
    NSMenuItem *fs = [view addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFull:) keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    fs.target = gDelegate;
    NSMenu *fm = [[[NSMenu alloc] initWithTitle:@"Full Screen Mode"] autorelease];
    NSMenuItem *pp = [fm addItemWithTitle:@"Pixel Perfect" action:@selector(setFullMode:) keyEquivalent:@""];
    pp.target = gDelegate; pp.tag = 0;
    NSMenuItem *fl = [fm addItemWithTitle:@"Fill Screen" action:@selector(setFullMode:) keyEquivalent:@""];
    fl.target = gDelegate; fl.tag = 1;
    NSMenuItem *st = [fm addItemWithTitle:@"Stretch" action:@selector(setFullMode:) keyEquivalent:@""];
    st.target = gDelegate; st.tag = 2;
    NSMenuItem *fmi = [[[NSMenuItem alloc] initWithTitle:@"Full Screen Mode" action:nil keyEquivalent:@""] autorelease];
    fmi.submenu = fm;
    [view addItem:fmi];
    [main addItem:submenu_item(@"View", view)];

    NSMenu *chat = [[[NSMenu alloc] initWithTitle:@"Chat"] autorelease];
    chat.autoenablesItems = NO;
    [chat addItemWithTitle:@"Show Chat" action:@selector(chatShow:) keyEquivalent:@""].target = gDelegate;
    [chat addItemWithTitle:@"Chat Settings\u2026" action:@selector(chatSettings:) keyEquivalent:@""].target = gDelegate;
    gChatTop = [submenu_item(@"Chat", chat) retain];
    gChatTop.enabled = NO;
    main.autoenablesItems = NO;                                   /* top-level items only: the Chat title is grayed out without Link Play */
    [main addItem:gChatTop];
    NSMenu *win = [[[NSMenu alloc] initWithTitle:@"Window"] autorelease];
    [win addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [main addItem:submenu_item(@"Window", win)];
    NSMenu *help = [[[NSMenu alloc] initWithTitle:@"Help"] autorelease];
    NSMenuItem *hi = [help addItemWithTitle:@"Grand Prix Manager 2 Help" action:@selector(showHelp:) keyEquivalent:@"?"];
    hi.target = gDelegate;
    [main addItem:submenu_item(@"Help", help)];
    [NSApp setMainMenu:main];
    [NSApp setWindowsMenu:win];
    [NSApp setHelpMenu:help];
}

static NSDictionary *item_dict(const HostMenuItem *m, int sys) {
    NSMutableArray *kids = [NSMutableArray array];
    for (int i = 0; i < m->nchild; i++) [kids addObject:item_dict(&m->child[i], sys)];
    return @{@"title": [NSString stringWithCString:m->text encoding:NSWindowsCP1252StringEncoding] ?: @"", @"id": @(m->id),
             @"gray": @(m->grayed), @"check": @(m->checked), @"sep": @(m->separator), @"kids": kids, @"sys": @(sys),
             @"popup": @(m->nchild > 0)};
}
static NSMenu *menu_from(NSString *title, NSArray *items) {
    NSMenu *m = [[[NSMenu alloc] initWithTitle:title] autorelease];
    m.autoenablesItems = NO;
    for (NSDictionary *d in items) {
        if ([[d objectForKey:@"sep"] boolValue]) { [m addItem:[NSMenuItem separatorItem]]; continue; }
        NSMenuItem *it = [[[NSMenuItem alloc] initWithTitle:[d objectForKey:@"title"] action:nil keyEquivalent:@""] autorelease];
        if ([[d objectForKey:@"popup"] boolValue]) it.submenu = menu_from([d objectForKey:@"title"], [d objectForKey:@"kids"]);
        else { it.action = @selector(gameMenu:); it.target = gDelegate; it.representedObject = d; }
        it.enabled = ![[d objectForKey:@"gray"] boolValue];
        it.state = [[d objectForKey:@"check"] boolValue] ? NSControlStateValueOn : NSControlStateValueOff;
        [m addItem:it];
    }
    return m;
}
void host_chat_menu_enable(int on) { dispatch_async(dispatch_get_main_queue(), ^{ gChatTop.enabled = on != 0; }); }
void host_computer_name(char *out, size_t cap) { hostname_short(out, cap); }
void host_set_menu(const HostMenuItem *top, int n, int sys) {
  @autoreleasepool {
    NSMutableArray *tops = [NSMutableArray array];
    for (int i = 0; i < n; i++) [tops addObject:item_dict(&top[i], sys)];
    dispatch_async(dispatch_get_main_queue(), ^{
        NSMenu *main = [NSApp mainMenu];
        if (!main) return;
        for (NSMenuItem *it in gGameItems) [main removeItem:it];
        [gGameItems release];
        gGameItems = [[NSMutableArray alloc] init];
        NSInteger at = 1;
        for (NSDictionary *d in tops) {
            NSArray *kids = [d objectForKey:@"kids"];
            NSMenuItem *it = submenu_item([d objectForKey:@"title"], menu_from([d objectForKey:@"title"], kids));
            [main insertItem:it atIndex:at++];
            [gGameItems addObject:it];
        }
    });
  }
}

/* ---------------------------------------------------------------------------------------------- */
/* preferences (GPM2 > Settings)                                                                   */
/* ---------------------------------------------------------------------------------------------- */
static NSPopUpButton *gPrefScale, *gPrefFull;
static NSButton *gPrefOpenFull;
static NSTextField *gPrefFolder;

static void refresh_prefs(void) {
    if (!gPrefs) return;
    if (![gPrefScale selectItemWithTag:lround(gScale * 10)]) [gPrefScale selectItem:nil];
    [gPrefFull selectItemWithTag:gFullMode];
    gPrefOpenFull.state = [[NSUserDefaults standardUserDefaults] boolForKey:@"OpenFullScreen"] ? NSControlStateValueOn : NSControlStateValueOff;
    gPrefFolder.stringValue = gCfg.game ? @(gCfg.game) : ([[NSUserDefaults standardUserDefaults] stringForKey:@"GameFolder"] ?: @"");
}
static NSTextField *label_at(NSString *t, NSRect r) {
    NSTextField *l = gpm_label(t, NO);
    l.frame = r;
    l.alignment = NSTextAlignmentRight;
    return l;
}
static void show_prefs(void) {
    if (!gPrefs) {
        gPrefs = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 532, 282) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                              backing:NSBackingStoreBuffered defer:NO];
        gPrefs.title = @"Settings";
        gPrefs.releasedWhenClosed = NO;
        gPrefs.hidesOnDeactivate = NO;
        NSTabView *tabs = [[[NSTabView alloc] initWithFrame:NSMakeRect(16, 16, 500, 250)] autorelease];
        [gPrefs.contentView addSubview:tabs];
        NSView *c = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 478, 200)] autorelease];
        NSTabViewItem *general = [[[NSTabViewItem alloc] initWithIdentifier:@"general"] autorelease];
        general.label = @"General";
        general.view = c;
        [tabs addTabViewItem:general];
        [c addSubview:label_at(@"Window scale:", NSMakeRect(10, 140, 130, 20))];
        gPrefScale = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, 136, 200, 26) pullsDown:NO];
        for (int t10 = 10; t10 <= (int)(menu_max_scale() * 10 + 1e-6); t10 += (t10 == 10 ? 5 : t10 == 15 ? 5 : 10)) {
            [gPrefScale addItemWithTitle:t10 == 10 ? @"1× (Actual Size)" : t10 % 10 ? [NSString stringWithFormat:@"%.1f×", t10 / 10.0] : [NSString stringWithFormat:@"%d×", t10 / 10]];
            gPrefScale.lastItem.tag = t10;
        }
        gPrefScale.target = gDelegate; gPrefScale.action = @selector(prefScale:);
        [c addSubview:gPrefScale];
        [c addSubview:label_at(@"Full screen mode:", NSMakeRect(10, 104, 130, 20))];
        gPrefFull = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, 100, 200, 26) pullsDown:NO];
        [gPrefFull addItemWithTitle:@"Pixel Perfect"]; gPrefFull.lastItem.tag = 0;
        [gPrefFull addItemWithTitle:@"Fill Screen"]; gPrefFull.lastItem.tag = 1;
        [gPrefFull addItemWithTitle:@"Stretch"]; gPrefFull.lastItem.tag = 2;
        gPrefFull.target = gDelegate; gPrefFull.action = @selector(prefFullMode:);
        [c addSubview:gPrefFull];
        gPrefOpenFull = [gpm_checkbox(@"Open in full screen", gDelegate, @selector(prefOpenFull:)) retain];
        gPrefOpenFull.frame = NSMakeRect(148, 70, 250, 22);
        [c addSubview:gPrefOpenFull];
        [c addSubview:label_at(@"Game folder:", NSMakeRect(10, 36, 130, 20))];
        gPrefFolder = [gpm_label(@"", NO) retain];
        gPrefFolder.frame = NSMakeRect(150, 36, 230, 20);
        [[gPrefFolder cell] setLineBreakMode:NSLineBreakByTruncatingMiddle];
        [c addSubview:gPrefFolder];
        NSButton *ch = gpm_button(@"Change…", gDelegate, @selector(prefFolder:));
        ch.frame = NSMakeRect(386, 32, 84, 28);
        [c addSubview:ch];
        [gPrefs center];
    }
    refresh_prefs();
    [gPrefs makeKeyAndOrderFront:nil];
}
/* ---------------------------------------------------------------------------------------------- */
/* host.h                                                                                           */
/* ---------------------------------------------------------------------------------------------- */
static pthread_mutex_t frame_mu = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSNumber *, id> *gPending;       /* HWND -> CGImageRef (the latest), under frame_mu */
static int frame_scheduled;
static unsigned nframes;
static struct { uint32_t win, hash; int have; } gHash[16];

static void free_buf(void *info, const void *data, size_t size) { free((void *)data); }

void host_window_update(const HostWindow *w) {
    HostWindow c = *w;
    dispatch_async(dispatch_get_main_queue(), ^{ apply_window(c); });
}
void host_window_destroy(uint32_t id) {
    dispatch_async(dispatch_get_main_queue(), ^{ remove_window(id); });
}

void host_present(uint32_t win, const uint8_t *pix, int w, int h, const uint8_t pal[256][4], const char *who) {
    uint32_t hs = 2166136261u;
    size_t n = (size_t)w * (size_t)h;
    int slot = -1;
    (void)who;
    for (size_t i = 0; i < n; i++) { hs ^= pix[i]; hs *= 16777619u; }
    for (int i = 0; i < 1024; i++) { hs ^= ((const uint8_t *)pal)[i]; hs *= 16777619u; }
    for (int i = 0; i < 16; i++) if (gHash[i].have && gHash[i].win == win) slot = i;
    if (slot < 0) for (int i = 0; i < 16; i++) if (!gHash[i].have) { slot = i; gHash[i].have = 1; gHash[i].win = win; gHash[i].hash = 0; break; }
    if (slot >= 0) {
        if (gHash[slot].hash == hs) return;        /* same frame as this window's */
        gHash[slot].hash = hs;
    }
    uint32_t *rgbx = (uint32_t *)malloc(n * 4);
    uint32_t lut[256];
    for (int i = 0; i < 256; i++) lut[i] = (uint32_t)pal[i][0] | ((uint32_t)pal[i][1] << 8) | ((uint32_t)pal[i][2] << 16) | 0xFF000000u;   /* bytes R,G,B,X (little endian) */
    for (size_t i = 0; i < n; i++) rgbx[i] = lut[pix[i]];
    CGDataProviderRef prov = CGDataProviderCreateWithData(NULL, rgbx, n * 4, free_buf);
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, srgb(), (CGBitmapInfo)kCGImageAlphaNoneSkipLast, prov, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(prov);
    pthread_mutex_lock(&frame_mu);
    @autoreleasepool {                                        /* guest thread */
    if (!gPending) gPending = [[NSMutableDictionary alloc] init];
    [gPending setObject:(__bridge id)img forKey:@(win)];      /* the dictionary retains the image */
    }
    nframes++;
    if (nframes == 1 || nframes % 25 == 0) fprintf(stderr, "[host] frame %u (%dx%d, hwnd %x)\n", nframes, w, h, win);
    int need = !frame_scheduled;
    frame_scheduled = 1;
    pthread_mutex_unlock(&frame_mu);
    CGImageRelease(img);
    if (need)
        dispatch_async(dispatch_get_main_queue(), ^{
            pthread_mutex_lock(&frame_mu);
            NSDictionary *d = gPending;                       /* ownership passes to d */
            gPending = nil;
            frame_scheduled = 0;
            pthread_mutex_unlock(&frame_mu);
            for (NSNumber *k in d) [view_for_hwnd(k.unsignedIntValue) setImage:(__bridge CGImageRef)[d objectForKey:k]];
            [d release];
        });
}
void host_set_title(const char *t) {
    @autoreleasepool {
        NSString *s = [NSString stringWithCString:t encoding:NSWindowsCP1252StringEncoding] ?: @"Grand Prix Manager 2";
        dispatch_async(dispatch_get_main_queue(), ^{ gWin.title = s; });
    }
}
void host_set_cursor(uint32_t idc) {
    dispatch_async(dispatch_get_main_queue(), ^{ gCursor = cursor_for(idc); refresh_cursors(); });
}
void host_show_cursor(int visible) {
    dispatch_async(dispatch_get_main_queue(), ^{ gCursorHidden = !visible; refresh_cursors(); });
}
NSWindow *gpm_main_window(void) { return gWin; }
static void center_over_main(NSWindow *w) {
    if (!gWin || !gWin.visible) { [w center]; return; }
    NSRect hf = gWin.frame, f = w.frame;
    [w setFrameOrigin:NSMakePoint(NSMidX(hf) - f.size.width / 2, NSMidY(hf) - f.size.height / 2)];
}
void host_help(int context) {
    char idx[1200];
    /* GPMHELP.HLP becomes HTML in the writable folder and opens in the default browser */
    if (!gCfg.game || !gCfg.wdir || !hlp_prepare(gCfg.game, gCfg.wdir, idx, sizeof idx)) {
        host_message_box("Help", "GPMHELP.HLP was not found in the game folder.", 0x30);
        return;
    }
    @autoreleasepool {
        NSString *path = @(idx);
        dispatch_async(dispatch_get_main_queue(), ^{
            NSURL *u = [NSURL fileURLWithPath:path];
            if (context >= 0) u = [NSURL URLWithString:[u.absoluteString stringByAppendingFormat:@"#ctx%d", context]] ?: u;
            [[NSWorkspace sharedWorkspace] openURL:u];
        });
    }
}
int host_message_box(const char *title, const char *text, uint32_t flags) {
    __block int result = 1;
  @autoreleasepool {
    NSString *t = [NSString stringWithCString:title encoding:NSWindowsCP1252StringEncoding] ?: @"";
    NSString *m = [NSString stringWithCString:text encoding:NSWindowsCP1252StringEncoding] ?: @"";
    void (^run)(void) = ^{
        if (flags & 0x40000000u) {                                        /* native About panel (the game's "About" window) */
            NSMutableParagraphStyle *ps = [[[NSMutableParagraphStyle alloc] init] autorelease];
            ps.alignment = NSTextAlignmentCenter;
            NSAttributedString *cr = [[[NSAttributedString alloc] initWithString:m attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:11],
                                                                                              NSParagraphStyleAttributeName: ps}] autorelease];
            [NSApp activateIgnoringOtherApps:YES];
            [NSApp orderFrontStandardAboutPanelWithOptions:@{@"ApplicationName": @"Grand Prix Manager 2", @"Credits": cr}];
            for (NSWindow *w in NSApp.windows)                                /* centered over the main window */
                if (w != gWin && w.visible && [w.className rangeOfString:@"About"].location != NSNotFound) center_over_main(w);
            return;
        }
        NSAlert *a = [[[NSAlert alloc] init] autorelease];
        a.messageText = t;
        a.informativeText = m;
        switch (flags & 0xF0u) {
        case 0x10: a.alertStyle = NSAlertStyleCritical; break;           /* MB_ICONERROR */
        case 0x30: a.alertStyle = NSAlertStyleWarning; break;            /* MB_ICONWARNING */
        default: a.alertStyle = NSAlertStyleInformational;
        }
        static const struct { const char *name; int id; } sets[6][3] = {
            {{"OK", 1}}, {{"OK", 1}, {"Cancel", 2}}, {{"Abort", 3}, {"Retry", 4}, {"Ignore", 5}},
            {{"Yes", 6}, {"No", 7}, {"Cancel", 2}}, {{"Yes", 6}, {"No", 7}}, {{"Retry", 4}, {"Cancel", 2}}};
        unsigned ty = flags & 0xFu;
        if (ty > 5) ty = 0;
        int ids[3], n = 0;
        for (int i = 0; i < 3 && sets[ty][i].name; i++) { [a addButtonWithTitle:[NSString stringWithUTF8String:sets[ty][i].name]]; ids[n++] = sets[ty][i].id; }
        unsigned def = (flags >> 8) & 0xFu;                               /* MB_DEFBUTTONn */
        if (def >= (unsigned)n) def = 0;
        for (int i = 0; i < n; i++) {
            NSButton *b = [a.buttons objectAtIndex:(NSUInteger)i];
            b.keyEquivalent = (unsigned)i == def ? @"\r" : (ids[i] == 2 ? @"\033" : @"");
        }
        [NSApp activateIgnoringOtherApps:YES];
        NSModalResponse r;
        if (gWin && gWin.visible && !gWin.miniaturized && [a respondsToSelector:@selector(beginSheetModalForWindow:completionHandler:)]) {   /* as a sheet of the main window (centered on it); 10.9+ */
            __block NSModalResponse sr = NSAlertFirstButtonReturn;
            GPM_NEWER_BEGIN
            [a beginSheetModalForWindow:gWin completionHandler:^(NSModalResponse rr) { sr = rr; [NSApp stopModal]; }];
            GPM_NEWER_END
            [NSApp runModalForWindow:a.window];
            r = sr;
        } else { r = [a runModal]; }               /* 10.6 to 10.8: application modal alert */
        int k = (int)(r - NSAlertFirstButtonReturn);
        result = k >= 0 && k < n ? ids[k] : ids[0];
    };
    if ([NSThread isMainThread]) run(); else dispatch_sync(dispatch_get_main_queue(), run);
  }
    return result;
}
/* Link Play (TCP/IP): address of the other Mac, "ip" or "ip:port"; empty = LAN discovery only. Returns 0 if cancelled. */
int host_net_ask_address(char *out, int cap) {
    __block int ok = 1;
    NSString *env = [[[NSProcessInfo processInfo] environment] objectForKey:@"GPM2_NET_HOST"];
    if (env) { snprintf(out, (size_t)cap, "%s", env.UTF8String); return 1; }
    void (^run)(void) = ^{
        NSAlert *a = [[[NSAlert alloc] init] autorelease];
        a.messageText = @"TCP/IP Connection";
        a.informativeText = @"Enter the IP address of the host (leave empty to search the local network).";
        [a addButtonWithTitle:@"OK"]; [a addButtonWithTitle:@"Cancel"];
        NSTextField *tf = [[[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 220, 24)] autorelease];
        a.accessoryView = tf;
        [NSApp activateIgnoringOtherApps:YES];
        NSModalResponse r = [a runModal];
        ok = r == NSAlertFirstButtonReturn;
        snprintf(out, (size_t)cap, "%s", ok ? tf.stringValue.UTF8String : "");
    };
    if ([NSThread isMainThread]) run(); else dispatch_sync(dispatch_get_main_queue(), run);
    return ok;
}
uint32_t host_now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (uint32_t)((mach_absolute_time() * tb.numer / tb.denom) / 1000000ull);
}
void host_sleep_ms(uint32_t ms) { usleep(ms * 1000u); }
void host_init(const char *out) { (void)out; }
unsigned host_frame_count(void) { return nframes; }
const char *host_last_frame_path(void) { return ""; }
int host_poll_events(unsigned idle) { (void)idle; return 0; }
int host_script_active(void) { return 0; }
void host_set_script(const char *p) { (void)p; }

/* ---------------------------------------------------------------------------------------------- */
/* game folder, 1.02B exe and writable folder                                                        */
/* ---------------------------------------------------------------------------------------------- */
static NSString *sha256_of(NSString *path) {
    NSData *d = [NSData dataWithContentsOfFile:path];
    if (!d) return @"";
    unsigned char h[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(d.bytes, (CC_LONG)d.length, h);
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [s appendFormat:@"%02x", h[i]];
    return s;
}
/* GPM2.EXE in the folder (case-insensitive), or nil */
static NSString *exe_in_folder(NSString *dir) {
    for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil])
        if ([f.lowercaseString isEqualToString:@"gpm2.exe"]) return [dir stringByAppendingPathComponent:f];
    return nil;
}
static BOOL is_game_folder(NSString *dir) { NSString *e = exe_in_folder(dir); return e && [sha256_of(e) isEqualToString:EXE_SHA256]; }
static void alert_need_102b(NSString *dir) {
    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    a.messageText = @"This app requires Grand Prix Manager 2 version 1.02B";
    a.informativeText = exe_in_folder(dir) ? @"GPM2.EXE in the chosen folder is not version 1.02B. Install the Grand Prix Manager 2 1.02B patch, then choose the game folder again."
                                           : @"GPM2.EXE was not found in the chosen folder. Choose the folder that contains GPM2.EXE (Grand Prix Manager 2 version 1.02B).";
    [NSApp activateIgnoringOtherApps:YES];
    [a runModal];
}
static NSString *choose_path(NSString *msg, BOOL dirs, NSString *startDir) {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.message = msg;
    p.canChooseDirectories = dirs; p.canChooseFiles = !dirs; p.allowsMultipleSelection = NO;
    p.prompt = @"Choose";
    if (startDir) p.directoryURL = [NSURL fileURLWithPath:startDir];
    [NSApp activateIgnoringOtherApps:YES];
    return [p runModal] == NSModalResponseOK ? p.URL.path : nil;
}
static BOOL resolve_files(void) {
    NSUserDefaults *u = [NSUserDefaults standardUserDefaults];
    NSString *game = gCfg.game ? @(gCfg.game) : [u stringForKey:@"GameFolder"];
    if (!gCfg.game && !(game && is_game_folder(game))) {
        game = choose_path(@"Choose the folder that contains GPM2.EXE (Grand Prix Manager 2 version 1.02B).", YES, game);
        if (!game) return NO;
        if (!is_game_folder(game)) { alert_need_102b(game); return NO; }
        [u setObject:game forKey:@"GameFolder"];
    }
    NSString *exe = gCfg.exe ? @(gCfg.exe) : exe_in_folder(game);   /* --exe: development only */
    if (!exe) { alert_need_102b(game); return NO; }
    gCfg.game = strdup(game.fileSystemRepresentation);
    gCfg.exe = strdup(exe.fileSystemRepresentation);
    if (!gCfg.wdir) {
        NSString *w = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:@"GPM2"];
        [[NSFileManager defaultManager] createDirectoryAtPath:w withIntermediateDirectories:YES attributes:nil error:nil];
        gCfg.wdir = strdup(w.fileSystemRepresentation);
    }
    return YES;
}

@interface AppDelegate (Launch)
- (void)showPrefs:(id)sender;
- (void)prefScale:(NSPopUpButton *)b;
- (void)prefFullMode:(NSPopUpButton *)b;
- (void)prefOpenFull:(NSButton *)b;
- (void)prefFolder:(id)sender;
@end
@implementation AppDelegate (Launch)
- (void)showPrefs:(id)sender { show_prefs(); }
- (void)prefScale:(NSPopUpButton *)b { apply_scale(b.selectedItem.tag / 10.0, YES); }
- (void)prefFullMode:(NSPopUpButton *)b { set_full_mode((int)b.selectedItem.tag, YES); }
- (void)prefOpenFull:(NSButton *)b { [[NSUserDefaults standardUserDefaults] setBool:b.state == NSControlStateValueOn forKey:@"OpenFullScreen"]; }
- (void)prefFolder:(id)sender {
    NSUserDefaults *u = [NSUserDefaults standardUserDefaults];
    NSString *g = choose_path(@"Choose the folder that contains GPM2.EXE (Grand Prix Manager 2 version 1.02B).", YES, gCfg.game ? @(gCfg.game) : [u stringForKey:@"GameFolder"]);
    if (!g) return;
    if (!is_game_folder(g)) { alert_need_102b(g); return; }
    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    [u setObject:g forKey:@"GameFolder"];
    refresh_prefs();
    a.messageText = @"Restart required";
    a.informativeText = @"The new game folder will be used the next time Grand Prix Manager 2 starts.";
    [a runModal];
}
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    GPM_NEWER_BEGIN
    if ([NSWindow respondsToSelector:@selector(setAllowsAutomaticWindowTabbing:)]) [NSWindow setAllowsAutomaticWindowTabbing:NO];
    GPM_NEWER_END
    NSUserDefaults *u = [NSUserDefaults standardUserDefaults];
    /* all fixes always on (fix_on default); --fix for tests only */
    for (int i = 0; i < gNCliFix; i++) if (fix_parse_arg(gCliFix[i])) fprintf(stderr, "--fix: invalid argument: %s\n", gCliFix[i]);
    gMaxScale = max_scale_for([NSScreen mainScreen]);
    gScale = [u objectForKey:@"ViewScale"] ? [u doubleForKey:@"ViewScale"] : floor(gMaxScale);   /* first use: the largest integer that fits */
    if (gCliScale > 0) gScale = gCliScale / 10.0;             /* --scale N (e.g. 1.5, 2) applies to this run only */
    gScale = gScale < 1 ? 1 : gScale > menu_max_scale() ? menu_max_scale() : gScale;
    gFullMode = (int)[u integerForKey:@"FullScreenMode"];
    if (gFullMode < 0 || gFullMode > 2) gFullMode = 0;
    if (gCliFill) gFullMode = 1;
    gDelegate = self;
    build_main_menu();

    NSRect content = NSMakeRect(0, 0, GAME_W * gScale, GAME_H * gScale);
    gWin = [[MainWindow alloc] initWithContentRect:content styleMask:WIN_STYLE backing:NSBackingStoreBuffered defer:NO];
    gWin.title = @"Grand Prix Manager 2";
    GPM_NEWER_BEGIN
    if ([gWin respondsToSelector:@selector(setTabbingMode:)]) gWin.tabbingMode = NSWindowTabbingModeDisallowed;
    GPM_NEWER_END
    gWin.delegate = self;
    gWin.acceptsMouseMovedEvents = YES;
    if (gpm_native_fullscreen()) gWin.collectionBehavior = GPM_FS_PRIMARY;
    gWin.contentMinSize = NSMakeSize(GAME_W, GAME_H);
    gView = [[GameView alloc] initWithFrame:content hwnd:0 main:YES gw:GAME_W gh:GAME_H];
    gView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    gWin.contentView = gView;
    BOOL had = [u objectForKey:@"NSWindow Frame GPM2MainWindow"] != nil;
    [gWin setFrameAutosaveName:@"GPM2MainWindow"];            /* the position is restored as it was; the size follows the scale */
    if (!had) [gWin center];
    resize_content(content.size.width, content.size.height);
    [gView relayout];
    [gWin makeKeyAndOrderFront:nil];
    [gWin makeFirstResponder:gView];
    [NSApp activateIgnoringOtherApps:YES];

    if (!resolve_files()) { [NSApp terminate:nil]; exit(0); }
    fprintf(stderr, "[host] game: %s\n[host] exe: %s\n[host] writable: %s\n", gCfg.game, gCfg.exe, gCfg.wdir);
    if (!gCfg.out) gCfg.out = "/tmp/gpm2-out";
    if (gpm2_start(&gCfg, &gThread)) {
        NSAlert *a = [[[NSAlert alloc] init] autorelease];
        a.messageText = @"Could not start the game";
        a.informativeText = @"Run again with GPM2_LOG=1 (or --log) and see ~/Library/Logs/GPM2/run.log for details.";
        [a runModal];
        exit(2);
    }
    if (gCliFull || [u boolForKey:@"OpenFullScreen"]) dispatch_async(dispatch_get_main_queue(), ^{ toggle_full_screen(); });
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        memset(&gCfg, 0, sizeof gCfg);
        gCfg.stack_mb = 256;
        opt_idle_exit = 0;                                    /* real time: never exits on idle */
        for (int i = 1; i < argc; i++) {                      /* only our own options; the rest (-psn_, -NS...) belongs to Cocoa */
            const char *a = argv[i];
            if (!strcmp(a, "--check-install") && i + 1 < argc) {   /* same detection as first use, without UI or defaults */
                NSString *f = @(argv[i + 1]);
                NSString *e = exe_in_folder(f);
                printf("folder: %s\nexe: %s (%s)\n", f.fileSystemRepresentation, e ? e.fileSystemRepresentation : "NOT FOUND", is_game_folder(f) ? "1.02B" : "not 1.02B: app would refuse");
                return is_game_folder(f) ? 0 : 1;
            }
            if (!strcmp(a, "--fullscreen")) { gCliFull = 1; continue; }
            if (!strcmp(a, "--fill")) { gCliFill = 1; continue; }
            if (i + 1 >= argc) break;
            if (!strcmp(a, "--game")) gCfg.game = argv[++i];
            else if (!strcmp(a, "--exe")) gCfg.exe = argv[++i];
            else if (!strcmp(a, "--cd")) gCfg.cd = argv[++i];
            else if (!strcmp(a, "--write-dir")) gCfg.wdir = argv[++i];
            else if (!strcmp(a, "--out")) gCfg.out = argv[++i];
            else if (!strcmp(a, "--fix")) { if (gNCliFix < 16) gCliFix[gNCliFix++] = argv[++i]; else i++; }
            else if (!strcmp(a, "--scale")) gCliScale = (int)lround(atof(argv[++i]) * 10);
            else if (!strcmp(a, "--host-stack-mb")) gCfg.stack_mb = (unsigned)atoi(argv[++i]);
            else if (!strcmp(a, "--fixed-time")) { opt_fixed_time = (uint32_t)strtoul(argv[++i], 0, 0); opt_fixed_time_set = 1; }
        }
        BOOL logOn = getenv("GPM2_LOG") && getenv("GPM2_LOG")[0] == '1';   /* log off by default: GPM2_LOG=1 or --log */
        for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--log")) logOn = YES;
        if (logOn) {
            NSString *logdir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/GPM2"];
            [[NSFileManager defaultManager] createDirectoryAtPath:logdir withIntermediateDirectories:YES attributes:nil error:nil];
            freopen([logdir stringByAppendingPathComponent:@"run.log"].fileSystemRepresentation, "w", stderr);
            setvbuf(stderr, NULL, _IOLBF, 0);
        } else freopen("/dev/null", "w", stderr);
        NSApplication *app = [NSApplication sharedApplication];
        gDelegate = [[AppDelegate alloc] init];                /* static: owned once */
        app.delegate = gDelegate;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
