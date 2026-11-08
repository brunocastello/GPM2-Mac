/* Native dialogs and Open/Save on macOS. Each game DIALOG becomes an NSPanel with AppKit controls
 * at the Win32 position (pixels = points, factor 1.0); the core (controls.c/dialog.c) is the source of truth and resends each
 * control that changes (host_dlg_ctl). User actions come back as HOST_EV_DLG. The guest waits on the event
 * queue; the main thread never blocks. The Open/Save NSOpenPanel/NSSavePanel runs via dispatch_sync. */
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#include "compat_mac.h"
#include "../host.h"
#include "dlg_layout_modern.h"
#include "../dlg_layout.h"

#include <string.h>

NSWindow *gpm_main_window(void);

static NSString *cp(const char *s) { return [NSString stringWithCString:s encoding:NSWindowsCP1252StringEncoding] ?: @""; }

@interface FlipView : NSView
@end
@implementation FlipView
- (BOOL)isFlipped { return YES; }
@end

@interface DlgPanel : NSPanel <NSWindowDelegate, NSTextFieldDelegate, NSComboBoxDelegate, NSTableViewDataSource, NSTableViewDelegate>
@property (copy) NSString *tmpl;
@property int cw, ch;
@property int ox, oy;                                      /* offset to the margin (template dialogs) */
@property(nonatomic) uint32_t dlg;
@property(nonatomic) BOOL quiet;                         /* applying core state: do not generate events */
@property(nonatomic) int chat;
@property(nonatomic) BOOL hig;                           /* own layout (Link Play): normal-size controls */
@property(nonatomic, retain) NSMutableDictionary<NSNumber *, NSView *> *views;     /* control HWND -> view */
@property(nonatomic, retain) NSMutableDictionary<NSNumber *, NSNumber *> *ids;     /* HWND -> control id */
@property(nonatomic, retain) NSMutableDictionary<NSNumber *, NSMutableArray *> *items;   /* LISTBOX HWND -> items */
@end

static NSMutableDictionary<NSNumber *, DlgPanel *> *gDlgs;

static void emit(DlgPanel *p, uint32_t hwnd, int code, int idx, NSString *text) {
    HostEvent e;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_DLG; e.win = p.dlg; e.a = [[p.ids objectForKey:@(hwnd)] intValue]; e.b = code; e.c = idx;
    if (text) snprintf(e.text, sizeof e.text, "%s", [text cStringUsingEncoding:NSWindowsCP1252StringEncoding] ?: text.UTF8String);
    host_push_event(&e);
}
static uint32_t hwnd_of(NSView *v) { return (uint32_t)[objc_getAssociatedObject(v, "hwnd") unsignedIntValue]; }
static void tag_view(NSView *v, uint32_t hwnd) { objc_setAssociatedObject(v, "hwnd", @(hwnd), OBJC_ASSOCIATION_RETAIN); }

@implementation DlgPanel
- (void)dealloc {
    [_tmpl release]; [_views release]; [_ids release]; [_items release];
    [super dealloc];
}
- (BOOL)canBecomeKeyWindow { return YES; }
- (void)cancelOperation:(id)sender {                     /* Esc */
    for (NSNumber *k in self.ids) if ([[self.ids objectForKey:k] intValue] == 2) { emit(self, k.unsignedIntValue, DLG_CLICK, 0, nil); return; }
    HostEvent e;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_CLOSE; e.win = self.dlg;
    host_push_event(&e);
}
- (BOOL)windowShouldClose:(id)w {
    HostEvent e;
    memset(&e, 0, sizeof e);
    e.type = HOST_EV_CLOSE; e.win = self.dlg;
    host_push_event(&e);
    return NO;
}
- (void)windowDidResize:(NSNotification *)n {              /* chat: the children follow autoresizing; the next apply_ctl uses the new size */
    if (!self.chat) return;
    NSSize cs = self.contentView.frame.size;
    self.cw = (int)cs.width; self.ch = (int)cs.height;
    for (NSView *v in self.contentView.subviews) {
        if (![v isKindOfClass:NSScrollView.class]) continue;
        NSTableView *t = ((NSScrollView *)v).documentView;
        if (t.tableColumns.count) [[t.tableColumns objectAtIndex:0] setWidth:MAX(v.frame.size.width - 20, 10)];
        if (t.numberOfRows > 0) [t scrollRowToVisible:t.numberOfRows - 1];
    }
}
- (void)clicked:(NSButton *)b { if (!self.quiet) emit(self, hwnd_of(b), DLG_CLICK, 0, nil); }
- (void)popped:(NSPopUpButton *)b { if (!self.quiet) emit(self, hwnd_of(b), DLG_SEL, (int)b.indexOfSelectedItem, nil); }
- (void)dbl:(NSTableView *)t { if (!self.quiet && t.clickedRow >= 0) emit(self, hwnd_of(t), DLG_DBLCLK, (int)t.clickedRow, nil); }
- (void)controlTextDidChange:(NSNotification *)n {
    NSTextField *f = n.object;
    if (self.quiet) return;
    if ([objc_getAssociatedObject(f, "number") boolValue]) {              /* ES_NUMBER: digits only */
        NSString *s = f.stringValue, *d = [[s componentsSeparatedByCharactersInSet:[[NSCharacterSet decimalDigitCharacterSet] invertedSet]] componentsJoinedByString:@""];
        if (![s isEqualToString:d]) f.stringValue = d;
    }
    emit(self, hwnd_of(f), DLG_TEXT, 0, f.stringValue);
}
- (void)controlTextDidBeginEditing:(NSNotification *)n { if (!self.quiet) emit(self, hwnd_of(n.object), DLG_FOCUS, 0, nil); }
- (void)comboBoxSelectionDidChange:(NSNotification *)n {
    NSComboBox *c = n.object;
    if (!self.quiet) emit(self, hwnd_of(c), DLG_SEL, (int)c.indexOfSelectedItem, nil);
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)t { return (NSInteger)[[self.items objectForKey:@(hwnd_of(t))] count]; }
- (id)tableView:(NSTableView *)t objectValueForTableColumn:(NSTableColumn *)c row:(NSInteger)r { return [[self.items objectForKey:@(hwnd_of(t))] objectAtIndex:(NSUInteger)r]; }
- (void)tableViewSelectionDidChange:(NSNotification *)n {
    NSTableView *t = n.object;
    if (!self.quiet && t.selectedRow >= 0) emit(self, hwnd_of(t), DLG_SEL, (int)t.selectedRow, nil);
}
@end

static NSFont *ctl_font(DlgPanel *p) { return [NSFont systemFontOfSize:p.hig ? [NSFont systemFontSize] : [NSFont smallSystemFontSize]]; }

static NSView *make_view(DlgPanel *p, const HostCtl *c) {
    NSRect fr = NSMakeRect(c->x, c->y, c->w, c->h);
    NSView *v = nil;
    switch (c->kind) {
    case HC_PUSH: case HC_DEFPUSH: {
        NSButton *b = gpm_button(cp(c->text), p, @selector(clicked:));
        b.bezelStyle = NSBezelStyleRounded; b.font = ctl_font(p);
        v = b; break;
    }
    case HC_CHECK: case HC_RADIO: {
        NSButton *b = gpm_button(cp(c->text), p, @selector(clicked:));
        [b setButtonType:c->kind == HC_CHECK ? NSButtonTypeSwitch : NSButtonTypeRadio];
        b.font = ctl_font(p);
        v = b; break;
    }
    case HC_GROUP: {
        NSBox *b = [[[NSBox alloc] initWithFrame:fr] autorelease];
        b.title = cp(c->text); b.titleFont = ctl_font(p); b.boxType = NSBoxPrimary;
        v = b; break;
    }
    case HC_EDIT: {
        NSTextField *f = [(c->password ? [NSSecureTextField alloc] : [NSTextField alloc]) initWithFrame:fr];
        [f autorelease];
        f.font = ctl_font(p); f.editable = !c->readonly; f.bezeled = YES; f.delegate = (id)p;
        if (c->multiline) { f.cell.wraps = YES; f.cell.scrollable = NO; }
        objc_setAssociatedObject(f, "number", @(c->number), OBJC_ASSOCIATION_RETAIN);
        if (p.chat) dispatch_async(dispatch_get_main_queue(), ^{ [p makeFirstResponder:f]; });   /* after entering the hierarchy */
        v = f; break;
    }
    case HC_STATIC: {
        NSTextField *f = gpm_label(cp(c->text), YES);
        f.font = ctl_font(p);
        f.alignment = c->align == 1 ? NSTextAlignmentCenter : c->align == 2 ? NSTextAlignmentRight : NSTextAlignmentLeft;
        v = f; break;
    }
    case HC_LIST: {
        NSScrollView *sv = [[[NSScrollView alloc] initWithFrame:fr] autorelease];
        NSTableView *t = [[[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, c->w, c->h)] autorelease];
        NSTableColumn *col = [[[NSTableColumn alloc] initWithIdentifier:@"c"] autorelease];
        NSTextFieldCell *cell = col.dataCell;
        cell.font = ctl_font(p); cell.lineBreakMode = NSLineBreakByClipping;
        col.width = MAX(c->w - 20, 10);
        [t addTableColumn:col];
        t.headerView = nil; t.dataSource = p; t.delegate = p; t.target = p; t.doubleAction = @selector(dbl:);
        t.allowsMultipleSelection = c->multisel; t.rowHeight = p.hig ? 18 : 15;
        if (p.hig) { GPM_NEWER_BEGIN if ([t respondsToSelector:@selector(setStyle:)]) t.style = NSTableViewStyleFullWidth; GPM_NEWER_END t.usesAlternatingRowBackgroundColors = !p.chat; t.intercellSpacing = NSMakeSize(0, 2); }
        if (p.chat) t.selectionHighlightStyle = NSTableViewSelectionHighlightStyleNone;   /* the game selects the last row only to scroll to it */
        tag_view(t, c->hwnd);
        sv.documentView = t; sv.hasVerticalScroller = YES; sv.borderType = NSBezelBorder;
        v = sv; break;
    }
    case HC_COMBO_LIST: {
        NSPopUpButton *b = [[[NSPopUpButton alloc] initWithFrame:NSMakeRect(c->x, c->y, c->w, 22) pullsDown:NO] autorelease];
        b.font = ctl_font(p); b.target = p; b.action = @selector(popped:);
        v = b; break;
    }
    case HC_COMBO_DROP: {
        NSComboBox *b = [[[NSComboBox alloc] initWithFrame:NSMakeRect(c->x, c->y, c->w, 22)] autorelease];
        b.font = ctl_font(p); b.delegate = (id)p; b.completes = NO;
        v = b; break;
    }
    default: v = [[[NSView alloc] initWithFrame:fr] autorelease];
    }
    tag_view(v, c->hwnd);
    if (p.chat) v.autoresizingMask = c->kind == HC_LIST ? NSViewWidthSizable | NSViewHeightSizable
                                   : c->kind == HC_EDIT ? NSViewWidthSizable | NSViewMinYMargin : NSViewMinXMargin | NSViewMinYMargin;   /* FlipView: MinY = top margin */
    if (c->kind == HC_GROUP) [p.contentView addSubview:v positioned:NSWindowBelow relativeTo:nil];
    else [p.contentView addSubview:v];
    return v;
}

static void apply_ctl(DlgPanel *p, HostCtl c, NSArray *items) {
    if (!p.hig) { c.x += p.ox; c.y += p.oy; }
    dlg_layout_ctl(p.tmpl.UTF8String, p.chat, p.cw, p.ch, &c);
    NSNumber *key = @(c.hwnd);
    NSView *v = [p.views objectForKey:key];
    if (!v) { v = make_view(p, &c); [p.views setObject:v forKey:key]; }
    [p.ids setObject:@(c.id) forKey:key];
    p.quiet = YES;
    NSRect fr = NSMakeRect(c.x, c.y, c.w, c.h);
    if (c.kind == HC_PUSH || c.kind == HC_DEFPUSH) {
        NSButton *b = (NSButton *)v;
        b.title = cp(c.text);
        CGFloat need = [b.title sizeWithAttributes:@{NSFontAttributeName: ctl_font(p)}].width + 24;
        fr.size.width = MAX(fr.size.width, need);
        fr.size.height = MAX(fr.size.height, 21);
        b.keyEquivalent = c.kind == HC_DEFPUSH ? @"\r" : c.id == 2 ? @"\033" : @"";
        b.enabled = c.enabled;
    } else if (c.kind == HC_CHECK || c.kind == HC_RADIO) {
        NSButton *b = (NSButton *)v;
        b.title = cp(c.text); b.state = c.checked == 1 ? NSControlStateValueOn : c.checked == 2 ? NSControlStateValueMixed : NSControlStateValueOff;
        b.enabled = c.enabled;
        fr.size.width = MAX(fr.size.width, [b.title sizeWithAttributes:@{NSFontAttributeName: ctl_font(p)}].width + 24);   /* MS Sans Serif layout: the text must not be clipped */
    } else if (c.kind == HC_GROUP) {
        ((NSBox *)v).title = cp(c.text);
    } else if (c.kind == HC_EDIT) {
        NSTextField *f = (NSTextField *)v;
        NSString *t = cp(c.text);
        if (![f.stringValue isEqualToString:t]) f.stringValue = t;
        f.enabled = c.enabled; f.editable = !c.readonly;
    } else if (c.kind == HC_STATIC) {
        NSTextField *f = (NSTextField *)v;
        f.stringValue = cp(c.text);
        if (c.h <= 20 && c.align == 0) fr.size.width = MAX(fr.size.width, [f.stringValue sizeWithAttributes:@{NSFontAttributeName: ctl_font(p)}].width + 6);   /* the layout is MS Sans Serif: the native text must not be clipped */
        f.textColor = c.enabled ? gpm_label_color() : NSColor.disabledControlTextColor;
    } else if (c.kind == HC_LIST) {
        NSTableView *t = ((NSScrollView *)v).documentView;
        if (t.tableColumns.count) [[t.tableColumns objectAtIndex:0] setWidth:MAX(c.w - 20, 10)];        /* chat creates the list at 0x0 and only positions it afterwards */
        if (!p.items) p.items = [NSMutableDictionary dictionary];
        if (![[p.items objectForKey:key] isEqualToArray:items]) { [p.items setObject:[NSMutableArray arrayWithArray:items] forKey:key]; [t reloadData]; }
        if (t.selectedRow != c.sel) {
            if (c.sel >= 0 && c.sel < (int)items.count) { [t selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)c.sel] byExtendingSelection:NO]; [t scrollRowToVisible:c.sel]; }
            else [t deselectAll:nil];
        }
        t.enabled = c.enabled;
    } else if (c.kind == HC_COMBO_LIST) {
        NSPopUpButton *b = (NSPopUpButton *)v;
        [b removeAllItems];
        for (NSString *s in items) [b addItemWithTitle:s];
        if (c.sel >= 0) [b selectItemAtIndex:c.sel]; else [b selectItem:nil];
        b.enabled = c.enabled;
        fr.size.height = 22;
    } else if (c.kind == HC_COMBO_DROP) {
        NSComboBox *b = (NSComboBox *)v;
        [b removeAllItems]; [b addItemsWithObjectValues:items];
        NSString *t = cp(c.text);
        if (![b.stringValue isEqualToString:t]) b.stringValue = t;
        b.enabled = c.enabled;
        fr.size.height = 22;
    }
    v.frame = c.kind == HC_PUSH || c.kind == HC_DEFPUSH || c.kind == HC_COMBO_LIST || (p.hig && c.kind == HC_EDIT) ? gpm_align_frame(v, fr) : fr;   /* the grid applies to the drawing, without the bezel overhang */
    v.hidden = !c.visible;
    if (c.visible) {                                                  /* a control that grew does not leave the window; margin at the edges */
        NSSize cs = p.contentView.frame.size;
        NSSize ns = NSMakeSize(MAX(cs.width, NSMaxX(fr) + HOST_DLG_MARGIN), MAX(cs.height, NSMaxY(fr) + HOST_DLG_MARGIN));
        if (!NSEqualSizes(cs, ns)) {
            NSPoint tl = NSMakePoint(NSMinX(p.frame), NSMaxY(p.frame));
            [p setContentSize:ns];
            [p setFrameTopLeftPoint:tl];
        }
    }
    p.quiet = NO;
}

/* Opening or closing a child window brings the game window to the front, above the chat, which is independent: whatever was
 * above it stays above, and whatever the user sent to the back stays behind. */
static DlgPanel *chat_above_main(void) {
    NSWindow *m = gpm_main_window();
    DlgPanel *chat = nil;
    for (DlgPanel *o in gDlgs.allValues) if (o.chat) chat = o;
    if (!chat || !m) return nil;
    NSArray *l = [(NSArray *)CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID) autorelease];
    for (NSDictionary *w in l) {
        long n = [[w objectForKey:(id)kCGWindowNumber] longValue];
        if (n == chat.windowNumber) return chat;
        if (n == m.windowNumber) return nil;
    }
    return nil;
}
static void chat_restore(DlgPanel *chat) {
    NSWindow *m = gpm_main_window();
    if (!chat || !m || chat.parentWindow) return;               /* a child is already above its parent */
    [chat orderWindow:NSWindowAbove relativeTo:m.windowNumber];
}
/* host_mac.m: the game's auxiliary windows are also children of the main one */
/* the snapshot holds the panel (+1) until gpm_chat_restore, which is always called after it */
void *gpm_chat_snapshot(void) { return [chat_above_main() retain]; }
void gpm_chat_restore(void *chat) {
    chat_restore((DlgPanel *)chat);
    [(DlgPanel *)chat release];
}
void host_dlg_open(const HostDlg *d) {
    HostDlg c = *d;
    if (c.mw && !dlg_layout_has(c.name, c.chat)) { c.w = c.mw; c.h = c.mh; }
    dlg_layout_size(c.name, &c.w, &c.h);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!gDlgs) gDlgs = [[NSMutableDictionary alloc] init];
        DlgPanel *p = [[[DlgPanel alloc] initWithContentRect:NSMakeRect(0, 0, c.w, c.h)
                                                 styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | (c.chat ? NSWindowStyleMaskResizable : 0) backing:NSBackingStoreBuffered defer:NO] autorelease];
        p.tmpl = @(c.name); p.cw = c.w; p.ch = c.h; p.ox = c.mw ? c.ox : 0; p.oy = c.mw ? c.oy : 0;
        p.chat = c.chat; p.hig = dlg_layout_has(c.name, c.chat); p.dlg = c.id; p.views = [NSMutableDictionary dictionary]; p.ids = [NSMutableDictionary dictionary];
        p.contentView = [[[FlipView alloc] initWithFrame:NSMakeRect(0, 0, c.w, c.h)] autorelease];
        if (c.chat) p.contentMinSize = NSMakeSize(220, 110);
        p.title = cp(c.title); p.releasedWhenClosed = NO; p.hidesOnDeactivate = NO; p.delegate = p;
        if (gpm_native_fullscreen()) p.collectionBehavior = GPM_FS_AUXILIARY;
        DlgPanel *chat = c.chat ? nil : chat_above_main();
        [gDlgs setObject:p forKey:@(c.id)];
        NSWindow *m = gpm_main_window();
        if (m) {
            NSRect mf = m.frame;
            if (c.chat) [p setFrameOrigin:NSMakePoint(NSMinX(mf) + 10, NSMaxY(mf) - 40 - p.frame.size.height)];
            else [p setFrameOrigin:NSMakePoint(NSMidX(mf) - p.frame.size.width / 2, NSMidY(mf) - p.frame.size.height / 2)];
            [m addChildWindow:p ordered:NSWindowAbove];             /* the chat too: only Send Window To Back releases it (host_dlg_back) */
        } else [p center];
        [p makeKeyAndOrderFront:nil];
        chat_restore(chat);
    });
}
void host_dlg_ctl(uint32_t dlg, const HostCtl *c) {
  @autoreleasepool {                                             /* guest thread */
    HostCtl cc = *c;
    NSMutableArray *items = [NSMutableArray array];
    for (int i = 0; i < c->nitems; i++) [items addObject:cp(c->items[i])];
    dispatch_async(dispatch_get_main_queue(), ^{
        DlgPanel *p = [gDlgs objectForKey:@(dlg)];
        if (p) apply_ctl(p, cc, items);
    });
  }
}
void host_dlg_front(uint32_t dlg) {
    dispatch_async(dispatch_get_main_queue(), ^{
        DlgPanel *p = [gDlgs objectForKey:@(dlg)];
        NSWindow *m = gpm_main_window();
        if (!p) return;
        if (m && !p.parentWindow) [m addChildWindow:p ordered:NSWindowAbove];
        [p orderFrontRegardless];                                /* appears even with the app in the background */
        [p makeKeyWindow];
    });
}
void host_dlg_back(uint32_t dlg) {
    dispatch_async(dispatch_get_main_queue(), ^{
        DlgPanel *p = [gDlgs objectForKey:@(dlg)];
        NSWindow *m = gpm_main_window();
        if (!p || !m) return;
        [m removeChildWindow:p];                                 /* a child never ends up behind its parent */
        [p orderWindow:NSWindowBelow relativeTo:m.windowNumber];
        [m makeKeyAndOrderFront:nil];
    });
}
void host_dlg_center(uint32_t dlg) {
    dispatch_async(dispatch_get_main_queue(), ^{
        DlgPanel *p = [gDlgs objectForKey:@(dlg)];
        NSWindow *m = gpm_main_window();
        if (!p) return;
        if (m) { NSRect mf = m.frame; [p setFrameOrigin:NSMakePoint(NSMidX(mf) - p.frame.size.width / 2, NSMidY(mf) - p.frame.size.height / 2)]; }
        else [p center];
    });
}
void host_beep(void) { dispatch_async(dispatch_get_main_queue(), ^{ NSBeep(); }); }
void host_dlg_close(uint32_t dlg) {
    dispatch_async(dispatch_get_main_queue(), ^{
        DlgPanel *p = [gDlgs objectForKey:@(dlg)];
        if (!p) return;
        NSWindow *m = gpm_main_window();
        DlgPanel *chat = p.chat ? nil : chat_above_main();
        [[p retain] autorelease];                                /* alive until the end of this event */
        [m removeChildWindow:p];
        p.delegate = nil;
        [p orderOut:nil];
        [p close];
        [gDlgs removeObjectForKey:@(dlg)];
        [m makeKeyAndOrderFront:nil];
        chat_restore(chat);
    });
}

/* ---- Open / Save ---------------------------------------------------------------------------------------- */
int host_file_dialog(const HostFileReq *rq, char *out, size_t cap) {
    __block int ok = 0;
    HostFileReq r = *rq;
    void (^run)(void) = ^{
        NSMutableArray *exts = [NSMutableArray array];
        BOOL any = NO;
        for (int i = 0; i < r.nfilt; i++) {
            if (r.save && i != r.fsel) continue;
            for (NSString *pat in [cp(r.fpat[i]) componentsSeparatedByString:@";"]) {
                NSString *t = [pat stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
                if ([t hasPrefix:@"*."] && ![t hasSuffix:@"*"]) [exts addObject:[t substringFromIndex:2].lowercaseString];
                else any = YES;
            }
        }
        NSSavePanel *p = r.save ? [NSSavePanel savePanel] : [NSOpenPanel openPanel];
        p.message = cp(r.title);
        if (r.dir[0]) p.directoryURL = [NSURL fileURLWithPath:@(r.dir) isDirectory:YES];
        if (exts.count && !any) p.allowedFileTypes = exts;
        else if (r.save && r.defext[0]) p.allowedFileTypes = @[@(r.defext)];
        if (r.save) { p.nameFieldStringValue = r.name[0] ? cp(r.name) : (r.defext[0] ? [@"Untitled." stringByAppendingString:@(r.defext)] : @""); p.canCreateDirectories = YES; }
        else { NSOpenPanel *o = (NSOpenPanel *)p; o.canChooseFiles = YES; o.canChooseDirectories = NO; o.allowsMultipleSelection = NO; }
        [NSApp activateIgnoringOtherApps:YES];
        if ([p runModal] == NSModalResponseOK && p.URL.isFileURL) {
            const char *s = p.URL.path.fileSystemRepresentation;
            if (strlen(s) < cap) { strcpy(out, s); ok = 1; }
        }
    };
    if ([NSThread isMainThread]) run(); else dispatch_sync(dispatch_get_main_queue(), run);
    return ok;
}
