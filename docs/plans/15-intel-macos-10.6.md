# Intel Macs from Mac OS X 10.6 (one universal app)

**Date:** 2026-11-08. **Status:** approved by the user (path B). Test: the user's 10.6 VM.

## Goal
The modern app (`make -C runtime app`) becomes one universal binary that runs on:
- arm64: macOS 11+ (unchanged);
- x86_64: Mac OS X 10.6 Snow Leopard and later (today 11.0).

Same icon as today (the modern one), readable by 10.6.

## Facts (checked with the local Xcode, clang 21, SDK 27.2)
- ARC cannot link below 10.13: `SDK does not contain 'libarclite'`. Without ARC, `-mmacosx-version-min=10.6`
  links fine. => the Cocoa host goes to **manual retain/release (MRC)** for both archs.
- `runtime/` C core: no availability warnings at 10.6; no `__thread`/TLS (10.7+), no other new APIs.
- Only `runtime/host/macos/{host_mac.m,dialogs_mac.m,audio_mac.m}` need work.

## Work
1. **MRC.** Drop `-fobjc-arc` from `OBJCFLAGS` in `runtime/Makefile`. Convert the three `.m` files:
   - `alloc`/`new`/`copy` objects kept in statics, ivars or dictionaries: own them exactly once; release when
     replaced or freed; transient ones get `autorelease`.
   - `@property(strong)` -> `retain` and release in `dealloc` (call `[super dealloc]`); `copy` stays, released
     in `dealloc`.
   - Blocks stored beyond the call: `Block_copy`/`Block_release` (or `[blk copy]`). Blocks passed to GCD or
     AppKit are copied by them.
   - `__bridge` casts: keep the cast, add the matching `CFRetain`/`CFRelease`/`retain` where ARC used to do it
     (e.g. `host_mac.m:755` puts a CGImage in a dictionary; `dialogs_mac.m:267` returns a panel pointer).
   - Any thread that is not the main thread and runs Objective-C needs its own `@autoreleasepool`.
   - Do not leak per frame: the paths that run on every frame (image update, `setImage:`) must stay balanced.
2. **APIs newer than 10.6** (list from `-Wunguarded-availability` at 10.6). Use plain old APIs, or a
   `respondsToSelector:` guard when the new behavior must stay on 11+:
   | API | Replacement |
   |---|---|
   | `dict[@"k"]`, `dict[@"k"] = v`, `arr[i]` (10.8) | `objectForKey:`, `setObject:forKey:`, `objectAtIndex:` |
   | `labelWithString:`, `wrappingLabelWithString:`, `buttonWithTitle:target:action:`, `checkboxWithTitle:...` (10.12) | `alloc/initWithFrame:` + setters |
   | `setStyle:` / `NSTableViewStyleFullWidth` (11.0) | only when `respondsToSelector:@selector(setStyle:)` |
   | `setTabbingMode:`, `setAllowsAutomaticWindowTabbing:` (10.12) | `respondsToSelector:` guard; use the raw value of `NSWindowTabbingModeDisallowed` |
   | `clock_gettime_nsec_np(_CLOCK_UPTIME_RAW)` (10.12) | `mach_absolute_time` + `mach_timebase_info` |
   | `convertPointToScreen:` (10.12) | `convertRectToScreen:` (10.7) with guard, else `convertBaseToScreen:` |
   | `labelColor` (10.10) | `controlTextColor` |
   | `containsString:` (10.10) | `rangeOfString:].location != NSNotFound` |
   | `setLineBreakMode:` on NSTextField (10.10) | `[[field cell] setLineBreakMode:]` |
   | `imageWithSize:flipped:drawingHandler:` (10.8) | `initWithSize:` + `lockFocus`/`unlockFocus` |
   | `beginSheetModalForWindow:completionHandler:` (10.9) | guard; else `beginSheetModalForWindow:modalDelegate:didEndSelector:contextInfo:` (or `runModal`) |
   | `backingScaleFactor`, `frameForAlignmentRect:` (10.7) | guard; 1.0 / the frame itself |
   | Native full screen (`toggleFullScreen:`, `NSWindowStyleMaskFullScreen`, `NSWindowCollectionBehaviorFullScreen*`) (10.7) | guard; on 10.6 full screen = like the Tiger/PPC build (`host_carbon.c`): frameless window covering the whole main screen (`[[NSScreen mainScreen] frame]`), `[NSApp setPresentationOptions:HideDock\|AutoHideMenuBar]` (10.6 API), restored to `NSApplicationPresentationDefault` on Exit; black background, no Carbon, no resolution change. Pixel Perfect / Fill / Stretch and "Open in full screen" work in both |
   Modern behavior on 11+ must not change.
3. **Build** (`runtime/Makefile`):
   - per-arch minimum: arm64 `-mmacosx-version-min=11.0`, x86_64 `-mmacosx-version-min=10.6`.
   - `Info.plist.in`: `LSMinimumSystemVersion` 10.6 plus `LSMinimumSystemVersionByArchitecture`
     (`arm64` = 11.0, `x86_64` = 10.6).
   - Do not add `@available` (no compiler-rt dependency surprises); use `respondsToSelector:`/`NSClassFromString`.
4. **Icon.** Keep the modern art. 10.6 cannot read the small `ic04/ic05/icp4/icp5` entries that `iconutil`
   writes. After `iconutil`, append the legacy chunks produced by `tools/make_icns.py` without `--plain`
   (same white squircle style: `is32/s8mk/il32/l8mk/ih32/h8mk/it32/t8mk`, 256/512 PNG) to the `.icns`,
   skipping types already present, and fix the total length in the header. Small stdlib helper in
   `tools/` (or a flag of `make_icns.py`).

## Acceptance (agent, locally; nothing installed)
- `make -C runtime app` builds; `lipo -info` = `x86_64 arm64`; `vtool -show-build` shows minos 10.6 (x86_64)
  and 11.0 (arm64).
- Each `.m` compiled at x86_64 10.6 with `-Wunguarded-availability` gives **zero** warnings.
- `nm -u` of the x86_64 slice: no symbol newer than 10.6 (spot-check `_clock_gettime*`, `_objc_*` ARC
  entry points such as `_objc_retainAutoreleasedReturnValue`/`_objc_storeStrong`, which must be absent).
- Clang static analyzer (`clang --analyze`) on the three `.m`: no leak/over-release reports on the converted code.
- The app still starts on this Mac (arm64): the main menu shows, then quit. The user does the visual
  acceptance on macOS 27 and on the 10.6 VM.
- No commit; the lead reviews the diff.

## Log

### 2026-11-08 (implementation)
- Changed: `runtime/Makefile` (no `-fobjc-arc`; per-arch minos via `minver_for`; icon step appends legacy chunks), `host/macos/Info.plist.in`, `host_mac.m`, `dialogs_mac.m`, new `host/macos/compat_mac.h` (guarded helpers, raw values of the 10.7 full screen constants), new `tools/add_legacy_icns.py`. `audio_mac.m` unchanged.
- MRC: statics own their object once (cursors, `gWin`, `gView`, `gDelegate`, `gPending`, `gInfos/gPanels`, `gGameItems`, `gChatTop`, pref controls, `gDlgs`); locals are `autorelease`d; `DlgPanel`/`GameView` have `dealloc`; `@autoreleasepool` on every guest-thread entry that makes ObjC objects. Decisions I was less sure of: (1) `gpm_chat_snapshot` now retains the chat panel and `gpm_chat_restore` releases it (always paired in `apply_window`); (2) `remove_window`/`host_dlg_close` hold the window with `retain/autorelease` until the end of the event before removing it from the dictionary; (3) `remove_window` also removes the view from `gViews` (before it was never freed). `@autoreleasepool` compiles to `NSAutoreleasePool` at 10.6 (no `objc_autoreleasePoolPush`).
- 10.6 full screen: `legacy_full()` in `host_mac.m`: `setStyleMask:Borderless`, frame = main screen, `setPresentationOptions:HideDock|AutoHideMenuBar`, black background; exit restores style, frame, `NSApplicationPresentationDefault`. The main window is now a `MainWindow` subclass (`canBecomeKeyWindow` YES) so the frameless window gets the keyboard. Not tested on 10.6 (only code paths compiled; this Mac uses the native one).
- Message boxes: sheet only if `beginSheetModalForWindow:completionHandler:` exists (10.9+); on 10.6 to 10.8 `runModal`.
- Acceptance: `lipo -info` = `x86_64 arm64`; `vtool`: x86_64 `LC_VERSION_MIN_MACOSX 10.6`, arm64 `minos 11.0`; the three `.m` at x86_64 10.6 with `-Wunguarded-availability`: 0 warnings; `nm -u` x86_64: no `clock_gettime*`, no ARC `objc_*` entry points; `--analyze`: no leak/over-release (2 pre-existing core.* reports: uninitialized `ids[0]` in `host_message_box`, `getenv` null in `main`); app started on arm64 (menus App/View/Chat/Window/Help), process alive after 6 s; icns has `is32...t8mk` appended (148017 bytes, header correct).
- 2026-11-08 (lead review): fixed `gViews` (static assigned an autoreleased array: would crash under MRC) and a stale comment. Rebuilt; x86_64 minos 10.6, links only 10.6-era frameworks (CoreGraphics resolved to ApplicationServices), no ARC/new symbols. Handed to the user for the test on a 10.6 VM.
- 2026-11-08 (10.6 VM test 1): dyld "Symbol not found: _AudioComponentFindNext" (on 10.6 the AudioComponent/AudioUnit functions are in AudioUnit.framework; the modern SDK binds them to AudioToolbox). Fix: audio_mac.m resolves the 5 functions with dlopen/dlsym; no AudioToolbox imports remain. Harness test OK on arm64 and x86_64.
- 2026-11-08 (10.6 VM test 2): app opens; legacy full screen shows a white window, stays white after Exit. Cause (likely): setStyleMask: on 10.6 replaces the layer-backed view's backing layer, dropping the manually added image layer. Fix: GameView attachLayers re-attaches it (viewDidMoveToWindow, relayout, applyImage); no-op on 10.7+.
- 2026-11-08 (10.6 VM test 3): full screen OK. Title bar empty after leaving full screen; fix: legacy_full re-sets the title after restoring the style.
- 2026-11-08: **accepted** by the user on the 10.6.8 VM (app opens, full screen, title).
