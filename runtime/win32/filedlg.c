/* GetOpenFileNameA / GetSaveFileNameA: reads OPENFILENAMEA, asks the host for the panel (NSOpenPanel/NSSavePanel)
 * and returns the path as a DOS path (C:\GPM2\... for the game folder/writable layer; Z:\... = synthetic drive
 * for anywhere else on the host). Never writes to original/ or the game folder: warns and cancels. */
#include "win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define OFN_OVERWRITEPROMPT 0x00000002u
#define OFN_FILEMUSTEXIST 0x00001000u

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    while (*s == ' ') memmove(s, s + 1, strlen(s));
}
static int has_wild(const char *s) { return strchr(s, '*') || strchr(s, '?'); }

static uint32_t fd_common(int save) {
    uint32_t p = ARG(0), fp, filt, flags, nmax, inidir, ttl;
    HostFileReq rq;
    char host[1024], dos[600], tmp[300];
    struct stat st;
    memset(&rq, 0, sizeof rq);
    if (!p || R32(p) < 0x4Cu) return 0;
    filt = R32(p + 0x0C); fp = R32(p + 0x1C); nmax = R32(p + 0x20); inidir = R32(p + 0x2C); ttl = R32(p + 0x30);
    flags = R32(p + 0x34);
    rq.save = save;
    rq.overwrite_prompt = save && (flags & OFN_OVERWRITEPROMPT);
    /* filter: pairs description\0pattern\0 ... \0\0 (the game pads with trailing spaces) */
    while (filt && rq.nfilt < 8) {
        char d[96], pt[128];
        gstr_get(d, sizeof d, filt);
        filt += gstr_len(filt) + 1u;
        if (!d[0] && !R8(filt)) break;
        gstr_get(pt, sizeof pt, filt);
        filt += gstr_len(filt) + 1u;
        trim(d); trim(pt);
        if (!pt[0]) break;
        snprintf(rq.fdesc[rq.nfilt], sizeof rq.fdesc[0], "%s", d);
        snprintf(rq.fpat[rq.nfilt], sizeof rq.fpat[0], "%s", pt);
        rq.nfilt++;
    }
    rq.fsel = (int)R32(p + 0x18) - 1;
    if (rq.fsel < 0 || rq.fsel >= rq.nfilt) rq.fsel = 0;
    /* default extension: from the chosen filter ("*.gps"); the game passes its own pattern in lpstrDefExt */
    if (rq.nfilt && strncmp(rq.fpat[rq.fsel], "*.", 2) == 0 && !has_wild(rq.fpat[rq.fsel] + 2)) {
        snprintf(rq.defext, sizeof rq.defext, "%s", rq.fpat[rq.fsel] + 2);
        { char *sc = strchr(rq.defext, ';'); if (sc) *sc = 0; }
    }
    /* initial name and folder */
    if (fp) {
        gstr_get(tmp, sizeof tmp, fp);
        trim(tmp);
        if (!has_wild(tmp)) { const char *b = strrchr(tmp, '\\'); snprintf(rq.name, sizeof rq.name, "%s", b ? b + 1 : tmp); }
    }
    if (inidir && R8(inidir)) {
        gstr_get(tmp, sizeof tmp, inidir);
        vfs_resolve(tmp, 0, rq.dir, sizeof rq.dir);
    } else vfs_cwd_host(rq.dir, sizeof rq.dir);
    if (ttl) gstr_get(rq.title, sizeof rq.title, ttl); else snprintf(rq.title, sizeof rq.title, "%s", save ? "Save As" : "Open");
    win32_log("%s: dir=%s name=%s filters=%d", save ? "GetSaveFileNameA" : "GetOpenFileNameA", rq.dir, rq.name, rq.nfilt);
    menu_sync();
    if (!host_file_dialog(&rq, host, sizeof host)) return 0;                  /* cancelled: CommDlgExtendedError = 0 */
    if (save && vfs_host_protected(host)) {
        host_message_box("Save As", "The original game folder is read-only. Please choose another folder.", 0x30);
        return 0;
    }
    if (!save && (flags & OFN_FILEMUSTEXIST) && stat(host, &st) != 0) { win32_log("chosen file does not exist: %s", host); return 0; }
    vfs_host_to_dos(host, dos, sizeof dos);
    win32_log("  -> %s (%s)", dos, host);
    if (fp && nmax) {
        uint32_t n = (uint32_t)strlen(dos), off = 0, ext = 0;
        if (n + 1 > nmax) return 0;                                           /* buffer too small: fail */
        gstr_putn(fp, nmax, dos);
        for (uint32_t i = 0; i < n; i++) if (dos[i] == '\\') off = i + 1;
        for (uint32_t i = off; i < n; i++) if (dos[i] == '.') ext = i + 1;
        W16(p + 0x38, off); W16(p + 0x3A, ext);
        if (R32(p + 0x24) && R32(p + 0x28)) gstr_putn(R32(p + 0x24), R32(p + 0x28), dos + off);
    }
    return 1;
}
static uint32_t c_GetOpenFileNameA(void) { return fd_common(0); }
static uint32_t c_GetSaveFileNameA(void) { return fd_common(1); }

void filedlg_register(void) {
    win32_register("comdlg32.dll!GetOpenFileNameA", c_GetOpenFileNameA);
    win32_register("comdlg32.dll!GetSaveFileNameA", c_GetSaveFileNameA);
}
