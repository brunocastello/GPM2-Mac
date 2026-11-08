# Win32 runtime import inventory (M1)

Generated from `runtime/win32/*.c` and from the run `--fixed-time 851990400` (calls = count until the
menu goes idle). State: **complete** (Win32/MSVCRT20 semantics for the game's use), **partial**
(works; limits in the note) or **stub** (logs the 1st call on stderr and returns a default value).
`USER32.dll!CreateWindowExA`, messages and timers are described in the header of `runtime/win32/user32.c`.

## USER32.dll (95: 46 complete, 13 partial, 36 stubs)

| import | state | calls | notes |
|---|---|---|---|
| `CheckRadioButton` | stub | 0 |  |
| `SendMessageA` | complete | 0 |  |
| `GetDlgItem` | stub | 0 |  |
| `PostMessageA` | complete | 0 |  |
| `SetWindowLongA` | complete | 0 |  |
| `DestroyCursor` | complete | 1 |  |
| `GetWindowLongA` | complete | 1 |  |
| `DefDlgProcA` | stub | 0 |  |
| `SetSysColors` | stub | 0 |  |
| `GetSysColor` | complete | 0 |  |
| `SetCapture` | complete | 0 | M2: with capture, mouse events go to the capturing window |
| `ReleaseCapture` | complete | 0 |  |
| `FillRect` | complete | 1 |  |
| `SetCursor` | complete | 1 | M2: handle 0x50000+IDC_* -> host_set_cursor (NSCursor); 0 = no cursor |
| `GetKeyState` | complete | 0 | M2: real state of keys/buttons (signed SHORT); GetAsyncKeyState likewise |
| `IsDlgButtonChecked` | stub | 0 |  |
| `CheckDlgButton` | stub | 0 |  |
| `AppendMenuA` | complete | 0 | M2 menu.c: full menu model |
| `UnregisterClassA` | complete | 0 |  |
| `SetActiveWindow` | complete | 4 |  |
| `GetClientRect` | complete | 0 | M2: top-level window has a 19 px caption: client 640x460 (19 px less with a Win32 menu bar) |
| `IsIconic` | complete | 0 |  |
| `IsWindowVisible` | complete | 0 |  |
| `GetClassInfoA` | complete | 0 |  |
| `GetClassLongA` | complete | 10 |  |
| `SetWindowsHookA` | stub | 0 |  |
| `UnhookWindowsHook` | stub | 0 |  |
| `CallNextHookEx` | stub | 0 |  |
| `GetWindowTextA` | complete | 0 |  |
| `SetWindowPos` | partial | 0 | position/size; no Z-order |
| `InsertMenuA` | complete | 0 | M2 menu.c: by position and by command; MF_POPUP/MF_SEPARATOR |
| `CallWindowProcA` | complete | 0 |  |
| `DrawTextA` | stub | 0 |  |
| `GetDialogBaseUnits` | complete | 0 |  |
| `CreateDialogParamA` | stub | 0 |  |
| `ShowCursor` | complete | 2 | M2: counter; <0 hides the cursor on the host |
| `IsWindowEnabled` | complete | 0 |  |
| `DdeFreeDataHandle` | stub | 0 |  |
| `RedrawWindow` | partial | 0 | invalidates the whole client area |
| `EnableWindow` | complete | 0 |  |
| `DdeConnect` | stub | 0 |  |
| `DdeDisconnect` | stub | 0 |  |
| `FindWindowA` | stub | 0 |  |
| `DdeClientTransaction` | stub | 0 |  |
| `DdeQueryStringA` | stub | 0 |  |
| `wsprintfA` | partial | 0 | same formatter |
| `DdePostAdvise` | stub | 0 |  |
| `DdeGetData` | stub | 0 |  |
| `DdeCmpStringHandles` | stub | 0 |  |
| `DdeCreateDataHandle` | stub | 0 |  |
| `DdeInitializeA` | stub | 0 |  |
| `DdeCreateStringHandleA` | stub | 0 |  |
| `DdeNameService` | stub | 0 |  |
| `DdeFreeStringHandle` | stub | 0 |  |
| `GetSubMenu` | complete | 0 | M2 menu.c |
| `EnableMenuItem` | complete | 0 | M2 menu.c: returns the previous state or -1; recursive search by command |
| `ModifyMenuA` | complete | 0 | M2 menu.c: the game replaces the 8th item of the system menu (Options) with gray text (flags 0x401) |
| `LoadMenuA` | complete | 0 | M2 menu.c: reads the RT_MENU from .rsrc (ids 0x81/0x83/0x84 in the game) |
| `SetClassLongA` | complete | 1 |  |
| `SetWindowTextA` | complete | 1 |  |
| `SendDlgItemMessageA` | stub | 0 |  |
| `EndDialog` | stub | 0 |  |
| `DialogBoxParamA` | stub | 0 |  |
| `DestroyWindow` | complete | 1 |  |
| `SetTimer` | complete | 6 |  |
| `KillTimer` | complete | 18 |  |
| `VkKeyScanA` | complete | 0 |  |
| `wvsprintfA` | partial | 0 | same formatter |
| `WinHelpA` | stub | 0 |  |
| `PostQuitMessage` | complete | 0 |  |
| `GetSystemMenu` | complete | 0 | M2 menu.c: system menu per window (Restore/Move/Size/Minimize/Maximize/sep/Close); bRevert=1 discards it |
| `MessageBeep` | partial | 0 | logs (no sound: M3) |
| `LoadStringA` | complete | 242 |  |
| `BringWindowToTop` | complete | 4 |  |
| `GetDesktopWindow` | complete | 1 |  |
| `GetWindowRect` | complete | 2 |  |
| `MoveWindow` | complete | 1 |  |
| `InvalidateRect` | complete | 0 |  |
| `BeginPaint` | complete | 1 |  |
| `EndPaint` | complete | 1 |  |
| `DefWindowProcA` | complete | 99 |  |
| `PeekMessageA` | complete | 302 |  |
| `TranslateMessage` | complete | 151 | M2: WM_KEYDOWN/WM_SYSKEYDOWN -> WM_CHAR/WM_SYSCHAR with the host's character (stored in pt.x) |
| `DispatchMessageA` | complete | 151 |  |
| `WaitMessage` | complete | 151 |  |
| `ReleaseDC` | complete | 207 |  |
| `MessageBoxA` | complete | 0 | M2: host_message_box (NSAlert on macOS; headless returns IDOK/IDYES/IDRETRY) |
| `LoadCursorA` | partial | 6 | system cursors only (the exe has no cursors in .rsrc); handle = 0x50000+IDC_* |
| `LoadIconA` | partial | 2 | fake handle per id |
| `RegisterClassA` | complete | 6 |  |
| `GetSystemMetrics` | partial | 4 | Windows 95 640x480 values |
| `CreateWindowExA` | complete | 2 |  |
| `GetDC` | complete | 208 |  |
| `ShowWindow` | complete | 1 |  |
| `SetFocus` | complete | 2 |  |

## WINMM.dll (5: 0 complete, 1 partial, 4 stubs)

| import | state | calls | notes |
|---|---|---|---|
| `waveOutOpen` | partial | 0 | fake handle, no audio |
| `sndPlaySoundA` | stub | 3 |  |
| `mciSendCommandA` | stub | 2 |  |
| `waveOutClose` | stub | 0 |  |
| `mciGetErrorStringA` | stub | 0 |  |

## KERNEL32.dll (33: 20 complete, 5 partial, 8 stubs)

| import | state | calls | notes |
|---|---|---|---|
| `WinExec` | stub | 0 |  |
| `CloseHandle` | stub | 0 |  |
| `GetStartupInfoA` | complete | 1 |  |
| `GetModuleHandleA` | complete | 1 |  |
| `GetTickCount` | partial | 0 | virtual with --fixed-time |
| `FindNextFileA` | complete | 0 |  |
| `FindFirstFileA` | complete | 0 |  |
| `SetFileTime` | stub | 0 |  |
| `DosDateTimeToFileTime` | stub | 0 |  |
| `TerminateThread` | stub | 0 |  |
| `lstrlenA` | complete | 0 |  |
| `lstrcpyA` | complete | 0 |  |
| `lstrcmpA` | complete | 0 |  |
| `FreeLibrary` | stub | 0 |  |
| `LoadLibraryA` | stub | 0 |  |
| `CreateEventA` | stub | 1 |  |
| `_lclose` | complete | 84 |  |
| `WritePrivateProfileStringA` | partial | 0 | likewise |
| `_hwrite` | complete | 0 |  |
| `GetVersion` | partial | 0 | Windows 95 (0xC3B60004) |
| `Sleep` | complete | 5 |  |
| `GetComputerNameA` | partial | 0 | "GPM2" |
| `GlobalUnlock` | complete | 56 |  |
| `GlobalLock` | complete | 82 |  |
| `GlobalAlloc` | complete | 69 |  |
| `GlobalFree` | complete | 56 |  |
| `GetPrivateProfileStringA` | partial | 8 | .ini in C:\WINDOWS (writable layer only) |
| `_lcreat` | complete | 0 |  |
| `_lwrite` | complete | 0 |  |
| `_llseek` | complete | 61 |  |
| `_hread` | complete | 177 |  |
| `_lopen` | complete | 89 |  |
| `_lread` | complete | 78 |  |

## comdlg32.dll (2: 0 complete, 0 partial, 2 stubs)

| import | state | calls | notes |
|---|---|---|---|
| `GetSaveFileNameA` | stub | 0 |  |
| `GetOpenFileNameA` | stub | 0 |  |

## WING32.dll (7: 7 complete, 0 partial, 0 stubs)

| import | state | calls | notes |
|---|---|---|---|
| `WinGGetDIBPointer` | complete | 2 |  |
| `WinGSetDIBColorTable` | complete | 177 |  |
| `WinGCreateBitmap` | complete | 2 |  |
| `WinGCreateDC` | complete | 2 |  |
| `WinGRecommendDIBFormat` | complete | 2 |  |
| `WinGStretchBlt` | complete | 90 |  |
| `WinGBitBlt` | complete | 36 |  |

## SHELL32.dll (3: 0 complete, 0 partial, 3 stubs)

| import | state | calls | notes |
|---|---|---|---|
| `DragFinish` | stub | 0 |  |
| `DragAcceptFiles` | stub | 1 |  |
| `DragQueryFileA` | stub | 0 |  |

## MSVCRT20.dll (81: 76 complete, 3 partial, 2 stubs)

| import | state | calls | notes |
|---|---|---|---|
| `srand` | complete | 1 |  |
| `toupper` | complete | 2 |  |
| `gmtime` | complete | 0 |  |
| `__dllonexit` | complete | 0 |  |
| `_controlfp` | partial | 1 | only stores the control word |
| `_except_handler3` | stub | 0 |  |
| `__p__fmode` | complete | 1 |  |
| `__p__commode` | complete | 1 |  |
| `__getmainargs` | complete | 1 |  |
| `_initterm` | complete | 2 |  |
| `__p__acmdln` | complete | 1 |  |
| `exit` | complete | 0 |  |
| `_XcptFilter` | stub | 0 |  |
| `_exit` | complete | 0 |  |
| `_strnicmp` | complete | 0 |  |
| `_stricmp` | complete | 1 |  |
| `realloc` | complete | 0 |  |
| `_open` | complete | 0 |  |
| `_filelength` | complete | 0 |  |
| `_eof` | complete | 0 |  |
| `_read` | complete | 0 |  |
| `_write` | complete | 0 |  |
| `_errno` | complete | 0 |  |
| `_close` | complete | 0 |  |
| `memcmp` | complete | 0 |  |
| `fflush` | complete | 0 |  |
| `ftell` | complete | 0 |  |
| `strtok` | complete | 0 |  |
| `strtol` | complete | 0 |  |
| `fwrite` | complete | 0 |  |
| `_stat` | complete | 0 |  |
| `_strdate` | complete | 0 |  |
| `fprintf` | complete | 0 |  |
| `sqrt` | complete | 0 |  |
| `asin` | complete | 0 |  |
| `acos` | complete | 0 |  |
| `_unlink` | complete | 0 |  |
| `_msize` | complete | 0 |  |
| `strcmp` | complete | 2 |  |
| `_chdrive` | complete | 0 |  |
| `calloc` | complete | 1 |  |
| `memchr` | complete | 0 |  |
| `sprintf` | partial | 127 | MSVC formatter, 3-digit exponent |
| `strncpy` | complete | 115 |  |
| `atan2` | complete | 0 |  |
| `cos` | complete | 0 |  |
| `sin` | complete | 0 |  |
| `_ftol` | complete | 0 |  |
| `abs` | complete | 0 |  |
| `strrchr` | complete | 0 |  |
| `strstr` | complete | 0 |  |
| `qsort` | complete | 4 |  |
| `fopen` | complete | 1 |  |
| `fgets` | complete | 330 |  |
| `fclose` | complete | 1 |  |
| `atoi` | complete | 2 |  |
| `atol` | complete | 1 |  |
| `malloc` | complete | 28 |  |
| `_isctype` | complete | 0 |  |
| `fseek` | complete | 0 |  |
| `fread` | complete | 0 |  |
| `strncmp` | complete | 0 |  |
| `memmove` | complete | 0 |  |
| `localtime` | partial | 0 | UTC with --fixed-time |
| `memset` | complete | 643 |  |
| `memcpy` | complete | 109 |  |
| `tolower` | complete | 5 |  |
| `_chdir` | complete | 1 |  |
| `_getdrive` | complete | 1 |  |
| `free` | complete | 28 |  |
| `strchr` | complete | 305 |  |
| `rand` | complete | 3615 |  |
| `_onexit` | complete | 0 |  |
| `_getdcwd` | complete | 1 |  |
| `time` | complete | 1 |  |
| `strcpy` | complete | 2018 |  |
| `_chmod` | complete | 0 |  |
| `_ltoa` | complete | 0 |  |
| `strlen` | complete | 558 |  |
| `_itoa` | complete | 0 |  |
| `strcat` | complete | 11 |  |

## GDI32.dll (32: 18 complete, 6 partial, 8 stubs)

| import | state | calls | notes |
|---|---|---|---|
| `TextOutA` | stub | 0 |  |
| `GetTextExtentPoint32A` | stub | 0 |  |
| `ExtTextOutA` | stub | 0 |  |
| `GetTextMetricsA` | stub | 0 |  |
| `Polygon` | stub | 0 |  |
| `CreateBrushIndirect` | partial | 0 | solid/hollow/pattern |
| `Pie` | stub | 0 |  |
| `CreatePen` | complete | 0 |  |
| `MoveToEx` | stub | 0 |  |
| `LineTo` | stub | 0 |  |
| `SetSystemPaletteUse` | complete | 0 |  |
| `GetSystemPaletteUse` | complete | 169 |  |
| `GetSystemPaletteEntries` | complete | 169 |  |
| `CreatePalette` | complete | 170 |  |
| `CreateFontA` | partial | 0 | only records |
| `SetBkMode` | complete | 0 |  |
| `CreateBitmapIndirect` | partial | 0 | 8 and 1 bpp only |
| `CreatePatternBrush` | complete | 0 |  |
| `CreateSolidBrush` | complete | 1 |  |
| `RealizePalette` | partial | 211 | sys_pal model (see gdi32.c) |
| `CreateCompatibleDC` | complete | 0 |  |
| `CreateCompatibleBitmap` | complete | 0 |  |
| `BitBlt` | partial | 0 | common ROPs, indices |
| `DeleteDC` | complete | 0 |  |
| `GetStockObject` | complete | 2 |  |
| `SelectPalette` | complete | 212 |  |
| `DeleteObject` | complete | 207 |  |
| `GetPaletteEntries` | complete | 47 |  |
| `GetDeviceCaps` | partial | 170 | 256 colors 640x480 |
| `SelectObject` | complete | 6 |  |
| `SetTextColor` | complete | 0 |  |
| `SetBkColor` | complete | 0 |  |

## DPLAYX.dll (2: 0 complete, 0 partial, 2 stubs)

| import | state | calls | notes |
|---|---|---|---|
| `#1` | stub | 0 |  |
| `#2` | stub | 0 |  |

### Added in M2 (registered but outside the import table of the 1.02B exe: never called by the game)

`GetAsyncKeyState`, `GetCursorPos`, `ClientToScreen`, `ScreenToClient`, `GetFocus`, `GetCapture`, `GetActiveWindow`,
`CreateMenu`, `CreatePopupMenu`, `DestroyMenu`, `DeleteMenu`, `RemoveMenu`, `CheckMenuItem`, `GetMenuItemCount`,
`GetMenuItemID`, `GetMenu`, `SetMenu`, `DrawMenuBar` (all complete; `SetMenu` does not resize the client area).
`LoadAcceleratorsA`/`TranslateAcceleratorA` do not exist: the exe has no accelerator table and does not import these functions.

Total: 260 imports registered. Called up to the menu: 101.
- 2026-11-02 (session 5, M3) — imports now implemented: `DialogBoxParamA`, `CreateDialogParamA`, `EndDialog`, `DefDlgProcA`, `GetDlgItem`,
  `SendDlgItemMessageA`, `CheckDlgButton`, `IsDlgButtonChecked`, `CheckRadioButton`, `GetOpenFileNameA`, `GetSaveFileNameA`, `DrawTextA`, `TextOutA`,
  `ExtTextOutA`, `GetTextExtentPoint32A`, `GetTextMetricsA`, `sndPlaySoundA`, `mciSendCommandA`, `mciGetErrorStringA` (details in the 09-dialogs-files-sound Log).
  Still stubs: `DragAcceptFiles/DragFinish/DragQueryFileA`, `Dde*`, `DPLAYX`, `WinHelpA`, hooks, `MoveToEx/LineTo/Polygon/Pie`, `waveOutOpen/Close` (detection only).
