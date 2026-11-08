/* Mac OS X 10.6 compatibility for the AppKit host. The binary is built with manual retain/release (MRC) and a 10.6
 * minimum on x86_64; APIs newer than 10.6 go through these helpers (a respondsToSelector: guard; on macOS 11+ they
 * call exactly the modern API). Every call to a newer selector is inside GPM_NEWER_BEGIN/END. */
#ifndef COMPAT_MAC_H
#define COMPAT_MAC_H
#import <Cocoa/Cocoa.h>

#define GPM_NEWER_BEGIN _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wunguarded-availability\"")
#define GPM_NEWER_END _Pragma("clang diagnostic pop")

/* raw values of the 10.7 full screen constants (unguarded use would warn at 10.6) */
#define GPM_STYLE_FULLSCREEN ((NSUInteger)1 << 14)       /* NSWindowStyleMaskFullScreen */
#define GPM_FS_PRIMARY ((NSUInteger)1 << 7)              /* NSWindowCollectionBehaviorFullScreenPrimary */
#define GPM_FS_AUXILIARY ((NSUInteger)1 << 8)            /* NSWindowCollectionBehaviorFullScreenAuxiliary */
/* native full screen (toggleFullScreen:) exists from 10.7 */
static inline BOOL gpm_native_fullscreen(void) { return [NSWindow instancesRespondToSelector:@selector(toggleFullScreen:)]; }
static inline CGFloat gpm_backing_scale(NSWindow *w) {
    GPM_NEWER_BEGIN
    return [w respondsToSelector:@selector(backingScaleFactor)] ? [w backingScaleFactor] : 1;
    GPM_NEWER_END
}
static inline NSRect gpm_align_frame(NSView *v, NSRect fr) {
    GPM_NEWER_BEGIN
    return [v respondsToSelector:@selector(frameForAlignmentRect:)] ? [v frameForAlignmentRect:fr] : fr;
    GPM_NEWER_END
}
static inline NSPoint gpm_to_screen(NSWindow *w, NSPoint p) {
    GPM_NEWER_BEGIN
    if ([w respondsToSelector:@selector(convertPointToScreen:)]) return [w convertPointToScreen:p];
    if ([w respondsToSelector:@selector(convertRectToScreen:)]) return [w convertRectToScreen:NSMakeRect(p.x, p.y, 0, 0)].origin;
    GPM_NEWER_END
    return [w convertBaseToScreen:p];
}
static inline NSColor *gpm_label_color(void) {
    GPM_NEWER_BEGIN
    return [NSColor respondsToSelector:@selector(labelColor)] ? [NSColor labelColor] : [NSColor controlTextColor];
    GPM_NEWER_END
}
/* autoreleased */
static inline NSTextField *gpm_label(NSString *t, BOOL wrapping) {
    GPM_NEWER_BEGIN
    if ([NSTextField respondsToSelector:@selector(wrappingLabelWithString:)])
        return wrapping ? [NSTextField wrappingLabelWithString:t] : [NSTextField labelWithString:t];
    GPM_NEWER_END
    NSTextField *f = [[[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 100, 17)] autorelease];
    [f setStringValue:t]; [f setEditable:NO]; [f setSelectable:wrapping]; [f setBezeled:NO]; [f setDrawsBackground:NO];
    [f setTextColor:[NSColor controlTextColor]];
    [[f cell] setWraps:wrapping];
    [[f cell] setLineBreakMode:wrapping ? NSLineBreakByWordWrapping : NSLineBreakByClipping];
    return f;
}
static inline NSButton *gpm_button(NSString *t, id target, SEL action) {
    GPM_NEWER_BEGIN
    if ([NSButton respondsToSelector:@selector(buttonWithTitle:target:action:)]) return [NSButton buttonWithTitle:t target:target action:action];
    GPM_NEWER_END
    NSButton *b = [[[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 80, 24)] autorelease];
    [b setTitle:t]; [b setTarget:target]; [b setAction:action];
    [b setBezelStyle:NSRoundedBezelStyle]; [b setButtonType:NSMomentaryPushInButton];
    return b;
}
static inline NSButton *gpm_checkbox(NSString *t, id target, SEL action) {
    GPM_NEWER_BEGIN
    if ([NSButton respondsToSelector:@selector(checkboxWithTitle:target:action:)]) return [NSButton checkboxWithTitle:t target:target action:action];
    GPM_NEWER_END
    NSButton *b = gpm_button(t, target, action);
    [b setButtonType:NSSwitchButton];
    return b;
}
#endif
