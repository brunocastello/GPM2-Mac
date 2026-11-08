"""Arity of the imports (bytes the callee pops from the stack), for the stubs.

Table copied from oracle/emu.py (STDCALL). Everything from MSVCRT* and wsprintfA is cdecl (0 bytes);
names outside the table are a *guess* of 0 arguments and are counted in the manifest.
"""

from __future__ import annotations

import re

STDCALL = {}
for _spec in """
CheckRadioButton4 SendMessageA4 GetDlgItem2 PostMessageA4 SetWindowLongA3 DestroyCursor1 GetWindowLongA2
DefDlgProcA4 SetSysColors3 GetSysColor1 SetCapture1 ReleaseCapture0 FillRect3 SetCursor1 GetKeyState1
IsDlgButtonChecked2 CheckDlgButton3 AppendMenuA4 UnregisterClassA2 SetActiveWindow1 GetClientRect2 IsIconic1
IsWindowVisible1 GetClassInfoA3 GetClassLongA2 SetWindowsHookA2 UnhookWindowsHook2 CallNextHookEx4
GetWindowTextA3 SetWindowPos7 InsertMenuA5 CallWindowProcA5 DrawTextA5 GetDialogBaseUnits0 CreateDialogParamA5
ShowCursor1 IsWindowEnabled1 DdeFreeDataHandle1 RedrawWindow4 EnableWindow2 DdeConnect4 DdeDisconnect1
FindWindowA2 DdeClientTransaction8 DdeQueryStringA5 DdePostAdvise3 DdeGetData4 DdeCmpStringHandles2
DdeCreateDataHandle7 DdeInitializeA4 DdeCreateStringHandleA3 DdeNameService4 DdeFreeStringHandle2 GetSubMenu2
EnableMenuItem3 ModifyMenuA5 LoadMenuA2 SetClassLongA3 SetWindowTextA2 SendDlgItemMessageA5 EndDialog2
DialogBoxParamA5 DestroyWindow1 SetTimer4 KillTimer2 VkKeyScanA1 wvsprintfA3 WinHelpA4 PostQuitMessage1
GetSystemMenu2 MessageBeep1 LoadStringA4 BringWindowToTop1 GetDesktopWindow0 GetWindowRect2 MoveWindow6
InvalidateRect3 BeginPaint2 EndPaint2 DefWindowProcA4 PeekMessageA5 TranslateMessage1 DispatchMessageA1
ReleaseDC2 MessageBoxA4 LoadCursorA2 LoadIconA2 RegisterClassA1 GetSystemMetrics1 CreateWindowExA12 GetDC1
ShowWindow2 SetFocus1
waveOutOpen6 sndPlaySoundA2 mciSendCommandA4 waveOutClose1 mciGetErrorStringA3
WinExec2 CloseHandle1 GetStartupInfoA1 GetModuleHandleA1 GetTickCount0 FindNextFileA2 FindFirstFileA2
SetFileTime4 DosDateTimeToFileTime3 TerminateThread2 lstrlenA1 lstrcpyA2 lstrcmpA2 FreeLibrary1 LoadLibraryA1
CreateEventA4 _lclose1 WritePrivateProfileStringA4 _hwrite3 GetVersion0 Sleep1 GetComputerNameA2 GlobalUnlock1
GlobalLock1 GlobalAlloc2 GlobalFree1 GetPrivateProfileStringA6 _lcreat2 _lwrite3 _llseek3 _hread3 _lopen2 _lread3
GetSaveFileNameA1 GetOpenFileNameA1
WinGGetDIBPointer2 WinGSetDIBColorTable4 WinGCreateBitmap3 WinGCreateDC0 WinGRecommendDIBFormat1
WinGStretchBlt10 WinGBitBlt8 DragFinish1 DragAcceptFiles2 DragQueryFileA4
TextOutA5 GetTextExtentPoint32A4 ExtTextOutA8 GetTextMetricsA2 Polygon3 CreateBrushIndirect1 Pie9 CreatePen3
MoveToEx4 LineTo3 SetSystemPaletteUse2 GetSystemPaletteUse1 GetSystemPaletteEntries4 CreatePalette1
CreateFontA14 SetBkMode2 CreateBitmapIndirect1 CreatePatternBrush1 CreateSolidBrush1 RealizePalette1
CreateCompatibleDC1 CreateCompatibleBitmap3 BitBlt9 DeleteDC1 GetStockObject1 SelectPalette3 DeleteObject1
GetPaletteEntries4 GetDeviceCaps2 SelectObject2 SetTextColor2 SetBkColor2
""".split():
    _m = re.match(r"^(.*?)(\d+)$", _spec)
    STDCALL[_m.group(1)] = int(_m.group(2))
STDCALL["#1"] = 3   # DPLAYX (ordinals): guess, as in the oracle
STDCALL["#2"] = 2
CDECL_USER32 = frozenset(["wsprintfA"])


def pop_bytes(dll: str, name: str):
    """(bytes popped by the callee, known?)"""
    if dll.upper().startswith("MSVCRT") or name in CDECL_USER32:
        return 0, True
    if name in STDCALL:
        return 4 * STDCALL[name], True
    return 0, False
