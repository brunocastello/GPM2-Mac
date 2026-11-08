/* KERNEL32 (file search), SHELL32, comdlg32, DPLAYX and USER32 dialogs/DDE: stubs with
 * logging. */
#include "win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ---- FindFirstFileA / FindNextFileA --------------------------------------------------------- */
#define MAX_FIND 16
#define MAX_NAMES 1024
static struct { int used, n, pos; char dir[600]; char (*names)[64]; } finds[MAX_FIND];

static void put_filetime(uint32_t a, time_t t) {
    uint64_t v = ((uint64_t)t + 11644473600ull) * 10000000ull;
    W32(a, (uint32_t)v); W32(a + 4, (uint32_t)(v >> 32));
}
static int fill_find(int h, uint32_t out) {
    char path[700], hp[2048], *bs;
    struct stat st;
    const char *nm;
    if (finds[h].pos >= finds[h].n) return 0;
    nm = finds[h].names[finds[h].pos++];
    snprintf(path, sizeof path, "%s", finds[h].dir);
    bs = strrchr(path, '\\');
    if (bs) bs[1] = 0; else path[0] = 0;
    strncat(path, nm, sizeof path - strlen(path) - 1);
    vfs_resolve(path, 0, hp, sizeof hp);
    for (uint32_t i = 0; i < 320; i += 4) W32(out + i, 0);
    if (stat(hp, &st) == 0) {
        W32(out, S_ISDIR(st.st_mode) ? 0x10u : 0x20u);
        put_filetime(out + 4, st.st_ctime); put_filetime(out + 12, st.st_atime); put_filetime(out + 20, st.st_mtime);
        W32(out + 32, (uint32_t)st.st_size);
    } else W32(out, 0x20u);
    gstr_putn(out + 44, 260, nm);
    return 1;
}
static uint32_t k_FindFirstFileA(void) {
    char pat[600];
    int h;
    gstr_get(pat, sizeof pat, ARG(0));
    for (h = 0; h < MAX_FIND; h++) if (!finds[h].used) break;
    if (h == MAX_FIND) return 0xFFFFFFFFu;
    finds[h].names = (char (*)[64])malloc((size_t)MAX_NAMES * 64);
    finds[h].n = vfs_list(pat, finds[h].names, MAX_NAMES);
    finds[h].pos = 0;
    snprintf(finds[h].dir, sizeof finds[h].dir, "%s", pat);
    if (finds[h].n == 0) { free(finds[h].names); return 0xFFFFFFFFu; }
    finds[h].used = 1;
    fill_find(h, ARG(1));
    return 0x00000F00u + (uint32_t)h;
}
static uint32_t k_FindNextFileA(void) {
    int h = (int)ARG(0) - 0xF00;
    if (h < 0 || h >= MAX_FIND || !finds[h].used) return 0;
    return (uint32_t)fill_find(h, ARG(1));
}
static uint32_t k_FindClose(void) {
    int h = (int)ARG(0) - 0xF00;
    if (h >= 0 && h < MAX_FIND && finds[h].used) { free(finds[h].names); finds[h].used = 0; }
    return 1;
}

static uint32_t s0(void) { win32_stub_note(win32_cur_import()); return 0; }
static uint32_t s1(void) { win32_stub_note(win32_cur_import()); return 1; }

/* USER32: dialogs (nothing happens) */

void misc_register(void) {
    win32_register("KERNEL32.dll!FindFirstFileA", k_FindFirstFileA);
    win32_register("KERNEL32.dll!FindNextFileA", k_FindNextFileA);
    win32_register("KERNEL32.dll!FindClose", k_FindClose);
    win32_register("KERNEL32.dll!SetFileTime", s1);
    win32_register("KERNEL32.dll!DosDateTimeToFileTime", s1);
    win32_register("SHELL32.dll!DragAcceptFiles", s0);
    win32_register("SHELL32.dll!DragFinish", s0);
    win32_register("SHELL32.dll!DragQueryFileA", s0);
    win32_register("USER32.dll!DdeInitializeA", s0);
    win32_register("USER32.dll!DdeFreeDataHandle", s0);
    win32_register("USER32.dll!DdeConnect", s0);
    win32_register("USER32.dll!DdeDisconnect", s0);
    win32_register("USER32.dll!DdeClientTransaction", s0);
    win32_register("USER32.dll!DdeQueryStringA", s0);
    win32_register("USER32.dll!DdePostAdvise", s0);
    win32_register("USER32.dll!DdeGetData", s0);
    win32_register("USER32.dll!DdeCmpStringHandles", s0);
    win32_register("USER32.dll!DdeCreateDataHandle", s0);
    win32_register("USER32.dll!DdeCreateStringHandleA", s0);
    win32_register("USER32.dll!DdeNameService", s0);
    win32_register("USER32.dll!DdeFreeStringHandle", s0);
}
