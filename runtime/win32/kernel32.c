/* KERNEL32 (stdcall) for the runtime. Win32 / Windows 95 semantics. */
#include "win32.h"
#include "../host/host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- time ----------------------------------------------------------------------------------- */
static uint32_t vtime_ms;
static uint32_t real_start;
static int started;

uint32_t win32_now_ms(void) {
    if (opt_fixed_time_set) return vtime_ms;
    if (!started) { started = 1; real_start = host_now_ms(); }
    return host_now_ms() - real_start;
}
void win32_advance_ms(uint32_t ms) { if (opt_fixed_time_set) vtime_ms += ms; }

/* with --fixed-time the clock only advances through Sleep/waits; busy-waiting on GetTickCount (the race) needs 1 ms per query */
static uint32_t k_GetTickCount(void) { win32_advance_ms(1); return 0x00010000u + win32_now_ms(); }
static uint32_t k_Sleep(void) {
    if (opt_fixed_time_set) win32_advance_ms(ARG(0)); else host_sleep_ms(ARG(0));
    return 0;
}

/* ---- process ----------------------------------------------------------------------------------- */
static uint32_t k_GetVersion(void) { return 0xC3B60004u; }           /* Windows 95 (4.0, build 950) */
static uint32_t k_GetStartupInfoA(void) {
    uint32_t p = ARG(0);
    for (uint32_t i = 0; i < 68; i += 4) W32(p + i, 0);
    W32(p, 68);
    return 0;
}
static uint32_t k_GetModuleHandleA(void) {
    if (ARG(0) == 0) return 0x00400000u;
    { char n[64]; gstr_get(n, sizeof n, ARG(0));
      if (gstr_cmp_ci(n, "gpm2.exe") == 0 || gstr_cmp_ci(n, "gpm2") == 0) return 0x00400000u; }
    return 0;
}
static uint32_t k_GetComputerNameA(void) {
    uint32_t buf = ARG(0), np = ARG(1);
    uint32_t cap = R32(np), n;
    char name[32];
    host_computer_name(name, sizeof name);
    gstr_putn(buf, cap, name);
    n = (uint32_t)strlen(name);
    W32(np, cap && n > cap - 1u ? cap - 1u : n);
    return 1;
}
static uint32_t k_LoadLibraryA(void) { return 0; }                    /* no extra DLLs: fails (ERROR_MOD_NOT_FOUND) */
static uint32_t k_FreeLibrary(void) { return 1; }
static uint32_t k_CloseHandle(void) { return 1; }
static uint32_t k_CreateEventA(void) { static uint32_t h = 0x80; return h++; }
static uint32_t k_TerminateThread(void) { return 1; }
static uint32_t k_WinExec(void) { char c[256]; gstr_get(c, sizeof c, ARG(0)); win32_log("WinExec(\"%s\") ignorado", c); return 33; }

/* ---- strings and global memory -------------------------------------------------------------------- */
static uint32_t k_lstrlenA(void) { return ARG(0) ? gstr_len(ARG(0)) : 0; }
static uint32_t k_lstrcpyA(void) {
    uint32_t d = ARG(0), s = ARG(1), i = 0, c;
    do { c = R8(s + i); W8(d + i, c); i++; } while (c);
    return d;
}
static uint32_t k_lstrcmpA(void) {
    char a[1024], b[1024];
    int r;
    gstr_get(a, sizeof a, ARG(0)); gstr_get(b, sizeof b, ARG(1));
    r = strcmp(a, b);
    return (uint32_t)(r < 0 ? -1 : r > 0 ? 1 : 0);
}
static uint32_t k_GlobalAlloc(void) {
    uint32_t p = heap_alloc(ARG(1));
    if (p && (ARG(0) & 0x40u)) memset(M + p, 0, ARG(1));
    return p;
}
static uint32_t k_GlobalLock(void) { return ARG(0); }
static uint32_t k_GlobalUnlock(void) { return 0; }
static uint32_t k_GlobalFree(void) { heap_free(ARG(0)); return 0; }

/* ---- _l* files (HFILE = host fd + 3, like the CRT fd) ------------------------------------------------ */
#define HF_BASE 3
static uint32_t k_lopen(void) {
    char path[512], hp[2048];
    uint32_t mode = ARG(1) & 3u;
    int fd;
    gstr_get(path, sizeof path, ARG(0));
    vfs_resolve(path, mode != 0, hp, sizeof hp);
    fd = open(hp, mode == 0 ? O_RDONLY : mode == 1 ? O_WRONLY : O_RDWR);
    if (fd < 0) win32_log("_lopen('%s') failed: host path '%s'", path, hp);
    return fd < 0 ? 0xFFFFFFFFu : (uint32_t)(fd + HF_BASE);
}
static uint32_t k_lcreat(void) {
    char path[512], hp[2048];
    int fd;
    gstr_get(path, sizeof path, ARG(0));
    vfs_resolve(path, 1, hp, sizeof hp);
    fd = open(hp, O_RDWR | O_CREAT | O_TRUNC, 0666);
    return fd < 0 ? 0xFFFFFFFFu : (uint32_t)(fd + HF_BASE);
}
static uint32_t k_lclose(void) { int fd = (int)ARG(0) - HF_BASE; return fd >= 0 && close(fd) == 0 ? 0 : 0xFFFFFFFFu; }
static uint32_t k_lread(void) {
    int fd = (int)ARG(0) - HF_BASE;
    ssize_t r;
    if (fd < 0) return 0xFFFFFFFFu;
    r = read(fd, guest_ptr(ARG(1), ARG(2)), ARG(2));
    return r < 0 ? 0xFFFFFFFFu : (uint32_t)r;
}
static uint32_t k_lwrite(void) {
    int fd = (int)ARG(0) - HF_BASE;
    ssize_t r;
    if (fd < 0) return 0xFFFFFFFFu;
    r = write(fd, guest_ptr(ARG(1), ARG(2)), ARG(2));
    return r < 0 ? 0xFFFFFFFFu : (uint32_t)r;
}
static uint32_t k_llseek(void) {
    int fd = (int)ARG(0) - HF_BASE;
    off_t r;
    if (fd < 0) return 0xFFFFFFFFu;
    r = lseek(fd, (off_t)(int32_t)ARG(1), ARG(2) == 1 ? SEEK_CUR : ARG(2) == 2 ? SEEK_END : SEEK_SET);
    return r < 0 ? 0xFFFFFFFFu : (uint32_t)r;
}

/* ---- profile (.INI) --------------------------------------------------------------------------------------- */
/* A name without a directory = the Windows directory (C:\WINDOWS), which does not exist in the original: only the writable layer. */
static void ini_dos(const char *name, char *dos, size_t cap) {
    if (strchr(name, '\\') || strchr(name, '/') || (name[0] && name[1] == ':')) snprintf(dos, cap, "%s", name);
    else snprintf(dos, cap, "C:\\WINDOWS\\%s", name);
}

typedef struct { char *buf; size_t n; } Blob;
static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    char *b;
    long l;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); l = ftell(f); fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)l + 1);
    if (!b || fread_yield(b, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return 0; }
    fclose(f);
    b[l] = 0; *n = (size_t)l;
    return b;
}

/* finds the value of [sec]key in the text; returns 1 and copies into out */
static int ini_get(const char *txt, const char *sec, const char *key, char *out, size_t cap) {
    const char *p = txt;
    int in = 0;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char line[1024];
        size_t k = 0;
        while (l && (p[l - 1] == '\r')) l--;
        if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, p, l); line[l] = 0;
        p = e ? e + 1 : p + strlen(p);
        while (line[k] == ' ' || line[k] == '\t') k++;
        if (line[k] == '[') {
            char *r = strchr(line + k, ']');
            if (r) { *r = 0; in = gstr_cmp_ci(line + k + 1, sec) == 0; }
        } else if (in && line[k] != ';') {
            char *eq = strchr(line + k, '=');
            if (eq) {
                size_t kl = (size_t)(eq - (line + k));
                while (kl && (line[k + kl - 1] == ' ' || line[k + kl - 1] == '\t')) kl--;
                if (strlen(key) == kl && strncasecmp(line + k, key, kl) == 0) {
                    const char *v = eq + 1;
                    while (*v == ' ' || *v == '\t') v++;
                    snprintf(out, cap, "%s", v);
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* Cache of the last .INI: reading and writing the file on every key costs ~30 ms on OS 9 and the game makes dozens of calls in a row.
 * Writes stay in memory and go to disk after 1 s (ini_flush_due), when switching files and on exit. */
static struct { char dos[600]; char *txt; size_t n; int dirty; uint32_t t; } inic;
static void ini_flush(void) {
    char hp[2048];
    FILE *f;
    if (!inic.dirty) return;
    inic.dirty = 0;
    vfs_resolve(inic.dos, 1, hp, sizeof hp);
    f = fopen(hp, "wb");
    if (!f) { win32_log("WritePrivateProfileString: could not write %s", hp); return; }
    fwrite(inic.txt, 1, inic.n, f);
    fclose(f);
}
void ini_flush_due(uint32_t now) { if (inic.dirty && now - inic.t >= 1000u) ini_flush(); }
void ini_flush_all(void) { ini_flush(); }
/* loads `name` into the cache; with no file, empty text */
static void ini_load(const char *name) {
    char dos[600], hp[2048];
    ini_dos(name, dos, sizeof dos);
    if (inic.txt && gstr_cmp_ci(dos, inic.dos) == 0) return;
    ini_flush();
    free(inic.txt);
    snprintf(inic.dos, sizeof inic.dos, "%s", dos);
    vfs_resolve(dos, 0, hp, sizeof hp);
    inic.txt = slurp(hp, &inic.n);
    if (!inic.txt) { inic.txt = (char *)calloc(1, 1); inic.n = 0; }
}

static uint32_t k_GetPrivateProfileStringA(void) {
    char sec[256], key[256], def[1024], name[512], val[1024];
    uint32_t out = ARG(3), cap = ARG(4);
    gstr_get(sec, sizeof sec, ARG(0));
    gstr_get(key, sizeof key, ARG(1));
    gstr_get(def, sizeof def, ARG(2));
    gstr_get(name, sizeof name, ARG(5));
    ini_load(name);
    snprintf(val, sizeof val, "%s", def);
    if (ARG(0) && ARG(1)) ini_get(inic.txt, sec, key, val, sizeof val);
    else if (ARG(0) && ARG(1) == 0) win32_stub_note("GetPrivateProfileStringA(key=NULL)");
    if (cap == 0) return 0;
    gstr_putn(out, cap, val);
    return (uint32_t)strlen(val) < cap ? (uint32_t)strlen(val) : cap - 1u;
}

static uint32_t k_WritePrivateProfileStringA(void) {
    char sec[256], key[256], val[1024], name[512];
    char *txt, *o;
    size_t n, w = 0, i;
    int in = 0, done = 0;
    static int hooked;
    if (!ARG(0) || !ARG(1)) return 0;
    gstr_get(sec, sizeof sec, ARG(0)); gstr_get(key, sizeof key, ARG(1));
    gstr_get(val, sizeof val, ARG(2)); gstr_get(name, sizeof name, ARG(3));
    ini_load(name);
    txt = inic.txt; n = inic.n;
    o = (char *)malloc(n + 2048);
    for (i = 0; i < n;) {
        char *e = memchr(txt + i, '\n', n - i);
        size_t l = e ? (size_t)(e - (txt + i)) + 1 : n - i;
        char line[1024];
        size_t ll = l, k = 0;
        if (ll >= sizeof line) ll = sizeof line - 1;
        memcpy(line, txt + i, ll); line[ll] = 0;
        while (line[k] == ' ') k++;
        if (line[k] == '[') {
            if (in && !done) { w += (size_t)sprintf(o + w, "%s=%s\r\n", key, val); done = 1; }
            { char *r = strchr(line + k, ']');
              if (r) { *r = 0; in = gstr_cmp_ci(line + k + 1, sec) == 0; *r = ']'; } }
        } else if (in && !done) {
            char *eq = strchr(line + k, '=');
            if (eq) {
                size_t kl = (size_t)(eq - (line + k));
                while (kl && line[k + kl - 1] == ' ') kl--;
                if (strlen(key) == kl && strncasecmp(line + k, key, kl) == 0) {
                    w += (size_t)sprintf(o + w, "%s=%s\r\n", key, val);
                    done = 1; i += l; continue;
                }
            }
        }
        memcpy(o + w, txt + i, l); w += l;
        i += l;
    }
    if (!done) {
        if (!in) w += (size_t)sprintf(o + w, "[%s]\r\n", sec);
        w += (size_t)sprintf(o + w, "%s=%s\r\n", key, val);
    }
    o[w] = 0;
    free(txt);
    inic.txt = o; inic.n = w; inic.dirty = 1; inic.t = win32_now_ms();
    if (!hooked) { hooked = 1; atexit(ini_flush_all); }
    return 1u;
}

void kernel32_register(void) {
#define R(n, f) win32_register("KERNEL32.dll!" n, f)
    R("GetTickCount", k_GetTickCount); R("Sleep", k_Sleep); R("GetVersion", k_GetVersion);
    R("GetStartupInfoA", k_GetStartupInfoA); R("GetModuleHandleA", k_GetModuleHandleA);
    R("GetComputerNameA", k_GetComputerNameA); R("LoadLibraryA", k_LoadLibraryA); R("FreeLibrary", k_FreeLibrary);
    R("CloseHandle", k_CloseHandle); R("CreateEventA", k_CreateEventA); R("TerminateThread", k_TerminateThread);
    R("WinExec", k_WinExec);
    R("lstrlenA", k_lstrlenA); R("lstrcpyA", k_lstrcpyA); R("lstrcmpA", k_lstrcmpA);
    R("GlobalAlloc", k_GlobalAlloc); R("GlobalLock", k_GlobalLock); R("GlobalUnlock", k_GlobalUnlock);
    R("GlobalFree", k_GlobalFree);
    R("_lopen", k_lopen); R("_lcreat", k_lcreat); R("_lclose", k_lclose); R("_lread", k_lread);
    R("_hread", k_lread); R("_lwrite", k_lwrite); R("_hwrite", k_lwrite); R("_llseek", k_llseek);
    R("GetPrivateProfileStringA", k_GetPrivateProfileStringA);
    R("WritePrivateProfileStringA", k_WritePrivateProfileStringA);
#undef R
}
