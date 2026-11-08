/* DOS -> host path mapping, case-insensitive, with a writable layer.
 *
 * `X:\gpm2\resto` (any letter) -> game folder; other absolute paths -> CD root.
 * Read: writable layer first, then the original. Write: always to the writable layer
 * (copy-on-write: a file that only exists in the original is copied before being opened for writing).
 */
#include "../pathjoin.h"
#include "win32.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

static char g_game[1024], g_write[1024], g_cd[1024];
static char cwd[260] = "C:\\GPM2";

void vfs_init(const char *game_dir, const char *write_dir, const char *cd_dir) {
    snprintf(g_game, sizeof g_game, "%s", game_dir);
    snprintf(g_write, sizeof g_write, "%s", write_dir);
    snprintf(g_cd, sizeof g_cd, "%s", cd_dir ? cd_dir : game_dir);
    path_strip_slash(g_game); path_strip_slash(g_write); path_strip_slash(g_cd);
}
const char *vfs_cwd(void) { return cwd; }
void vfs_set_cwd(const char *dos) { snprintf(cwd, sizeof cwd, "%s", dos); }

#define MAXC 32
typedef struct { char c[MAXC][128]; int n; } Path;

static void split_path(Path *p, const char *s) {
    char tok[128];
    size_t t = 0;
    for (;; s++) {
        if (*s == '\\' || *s == '/' || *s == 0) {
            if (t) {
                tok[t] = 0;
                if (strcmp(tok, ".") == 0) { }
                else if (strcmp(tok, "..") == 0) { if (p->n) p->n--; }
                else if (p->n < MAXC) { snprintf(p->c[p->n], sizeof p->c[0], "%s", tok); p->n++; }
                t = 0;
            }
            if (!*s) break;
        } else if (t + 1 < sizeof tok) tok[t++] = *s;
    }
}

/* looks up `name` in `dir` case-insensitively; 1 = found (writes the real name) */
static int ci_entry(const char *dir, const char *name, char *real, size_t cap) {
    DIR *d;
    struct dirent *e;
    char full[2048];
    struct stat st;
    path_join(full, sizeof full, dir, name);
    if (stat(full, &st) == 0) { snprintf(real, cap, "%s", name); return 1; }
    d = opendir(dir);
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        if (gstr_cmp_ci(e->d_name, name) == 0) { snprintf(real, cap, "%s", e->d_name); closedir(d); return 1; }
    }
    closedir(d);
    return 0;
}

/* path `root/c0/c1/...` with the on-disk case; 1 = exists */
static int ci_walk(const char *root, const Path *p, int from, char *out, size_t cap) {
    char cur[2048], real[256];
    snprintf(cur, sizeof cur, "%s", root);
    for (int i = from; i < p->n; i++) {
        if (!ci_entry(cur, p->c[i], real, sizeof real)) {
            size_t l = strlen(cur);
            snprintf(cur + l, sizeof cur - l, "/%s", p->c[i]);
            for (int k = i + 1; k < p->n; k++) { l = strlen(cur); snprintf(cur + l, sizeof cur - l, "/%s", p->c[k]); }
            snprintf(out, cap, "%s", cur);
            return 0;
        }
        { size_t l = strlen(cur); snprintf(cur + l, sizeof cur - l, "/%s", real); }
    }
    snprintf(out, cap, "%s", cur);
    return 1;
}

/* fread in 64 KB chunks yielding the CPU to the host between them (Mac OS 9 is cooperative: large reads would freeze the system) */
size_t fread_yield(void *buf, size_t n, FILE *f) {
    size_t got = 0;
    while (got < n) {
        size_t c = n - got < 65536u ? n - got : 65536u, r = fread((char *)buf + got, 1, c, f);
        got += r;
        if (dispatch_yield_hook) dispatch_yield_hook();
        if (r != c) break;
    }
    return got;
}

static void mkdirs_for(const char *file) {
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", file);
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(tmp, 0777); *q = '/'; }
}

static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b;
    char buf[65536];
    size_t n;
    if (!a) return -1;
    mkdirs_for(to);
    b = fopen(to, "wb");
    if (!b) { fclose(a); return -1; }
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) { fwrite(buf, 1, n, b); if (dispatch_yield_hook) dispatch_yield_hook(); }
    fclose(a); fclose(b);
    return 0;
}

/* ---- synthetic Z: drive and host <-> DOS conversion ------------------------------------- */
static void real_of(const char *path, char *out, size_t cap) {   /* realpath; if the file does not exist, resolves the folder */
    char tmp[PATH_MAX], dir[2048];
    const char *b = strrchr(path, '/');
    if (realpath(path, tmp)) { snprintf(out, cap, "%s", tmp); return; }
    if (b) {
        size_t l = (size_t)(b - path);
        memcpy(dir, path, l); dir[l] = 0;
        if (realpath(l ? dir : "/", tmp)) { path_join(out, cap, tmp, b + 1); return; }
    }
    snprintf(out, cap, "%s", path);
}
static int under(const char *path, const char *dir) {
    size_t l = strlen(dir);
    while (l > 1 && dir[l - 1] == '/') l--;
    return strncmp(path, dir, l) == 0 && (path[l] == '/' || path[l] == 0);
}
int vfs_host_protected(const char *host) {
    char r[2048], g[2048], w[2048], c[2048], tok[256];
    const char *s;
    real_of(host, r, sizeof r);
    mkdir(g_write, 0777);
    real_of(g_write, w, sizeof w);
    if (under(r, w)) return 0;
    real_of(g_game, g, sizeof g); real_of(g_cd, c, sizeof c);
    if (under(r, g) || under(r, c)) return 1;
    for (s = r; *s;) {                                          /* any folder named "original" */
        size_t t = 0;
        while (*s == '/') s++;
        while (*s && *s != '/' && t + 1 < sizeof tok) tok[t++] = *s++;
        tok[t] = 0;
        if (t && gstr_cmp_ci(tok, "original") == 0) return 1;
    }
    return 0;
}
int vfs_host_to_dos(const char *host, char *dos, size_t cap) {
    char r[2048], w[2048], g[2048];
    const char *rel = 0;
    size_t l;
    real_of(host, r, sizeof r);
    mkdir(g_write, 0777);
    real_of(g_write, w, sizeof w); real_of(g_game, g, sizeof g);
    if (under(r, w)) { l = strlen(w); while (l > 1 && w[l - 1] == '/') l--; rel = r + l; snprintf(dos, cap, "C:\\GPM2"); }
    else if (under(r, g)) { l = strlen(g); while (l > 1 && g[l - 1] == '/') l--; rel = r + l; snprintf(dos, cap, "C:\\GPM2"); }
    else { rel = r; snprintf(dos, cap, "Z:"); }
    for (size_t i = strlen(dos), k = 0; rel[k] && i + 1 < cap; k++) dos[i++] = rel[k] == '/' ? '\\' : rel[k], dos[i] = 0;
    return 0;
}
/* (host) folder of the current directory in the writable layer; creates it if needed */
int vfs_cwd_host(char *out, size_t cap) {
    char dos[300];
    snprintf(dos, sizeof dos, "%s", cwd);
    vfs_resolve(dos, 0, out, cap);
    {   struct stat st;
        char wr[2048];
        /* prefers the writable layer (where saves end up), creating the folder */
        const char *r = cwd;
        Path p;
        p.n = 0;
        if (r[0] && r[1] == ':') r += 2;
        split_path(&p, r);
        if (p.n && gstr_cmp_ci(p.c[0], "gpm2") == 0) {
            ci_walk(g_write, &p, 1, wr, sizeof wr);
            if (stat(wr, &st) != 0) { char t[2100]; snprintf(t, sizeof t, "%s/x", wr); mkdirs_for(t); }
            if (stat(wr, &st) == 0) snprintf(out, cap, "%s", wr);
        }
    }
    return 0;
}

int vfs_resolve(const char *dos, int write, char *out, size_t cap) {
    Path p;
    const char *rest = dos;
    char rd[2048], wr[2048];
    const char *root;
    int from, inw, ino;
    p.n = 0;
    if ((dos[0] == 'Z' || dos[0] == 'z') && dos[1] == ':') {   /* synthetic drive: host root */
        split_path(&p, dos + 2);
        ci_walk("", &p, 0, rd, sizeof rd);
        if (write && vfs_host_protected(rd)) {                  /* never writes to original/ or the game folder */
            const char *b = strrchr(rd, '/');
            snprintf(wr, sizeof wr, "%s/_blocked/%s", g_write, b ? b + 1 : rd);
            mkdirs_for(wr);
            snprintf(out, cap, "%s", wr);
        } else snprintf(out, cap, "%s", rd);
        return (int)strlen(out) >= (int)cap;
    }
    if (rest[0] && rest[1] == ':') rest += 2;
    if (rest[0] != '\\' && rest[0] != '/') {            /* relative: prepends the current directory */
        const char *c = cwd;
        if (c[0] && c[1] == ':') c += 2;
        split_path(&p, c);
    }
    split_path(&p, rest);
    if (p.n && gstr_cmp_ci(p.c[0], "gpm2") == 0) { root = g_game; from = 1; }
    else { root = g_cd; from = 0; }
    ino = ci_walk(root, &p, from, rd, sizeof rd);
    inw = ci_walk(g_write, &p, from, wr, sizeof wr);
    if (write) {
        if (!inw && ino) { if (copy_file(rd, wr) != 0) mkdirs_for(wr); }
        else mkdirs_for(wr);
        snprintf(out, cap, "%s", wr);
    } else {
        snprintf(out, cap, "%s", inw ? wr : rd);
    }
    return (int)strlen(out) >= (int)cap;
}

int vfs_stat_exists(const char *dos) {
    char path[2048];
    struct stat st;
    vfs_resolve(dos, 0, path, sizeof path);
    return stat(path, &st) == 0;
}

static int wild_match(const char *p, const char *n) {
    if (!*p) return !*n;
    if (*p == '*') { for (;; n++) { if (wild_match(p + 1, n)) return 1; if (!*n) return 0; } }
    if (!*n) return 0;
    if (*p != '?') {
        int a = *p, b = *n;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return wild_match(p + 1, n + 1);
}

/* lists the names of `dir_dos\pattern` (writable + original layers, no repeats), sorted
 * case-insensitively; "." and ".." included as in FindFirstFile. Returns the count. */
int vfs_list(const char *dos_pattern, char names[][64], int max) {
    char dirdos[600], pat[128], hp[2048];
    const char *bs = strrchr(dos_pattern, '\\');
    int n = 0;
    if (!bs) bs = strrchr(dos_pattern, '/');
    if (bs) { size_t l = (size_t)(bs - dos_pattern); memcpy(dirdos, dos_pattern, l); dirdos[l] = 0; snprintf(pat, sizeof pat, "%s", bs + 1); }
    else { snprintf(dirdos, sizeof dirdos, "."); snprintf(pat, sizeof pat, "%s", dos_pattern); }
    if (dirdos[0] == 0) snprintf(dirdos, sizeof dirdos, "\\");
    for (int layer = 0; layer < 2; layer++) {
        Path p;
        char root[2048];
        const char *r = dirdos;
        DIR *d;
        struct dirent *e;
        p.n = 0;
        if (r[0] && r[1] == ':') r += 2;
        if (r[0] != '\\' && r[0] != '/') { const char *c = cwd; if (c[0] && c[1] == ':') c += 2; split_path(&p, c); }
        split_path(&p, r);
        if (p.n && gstr_cmp_ci(p.c[0], "gpm2") == 0) ci_walk(layer ? g_game : g_write, &p, 1, root, sizeof root);
        else ci_walk(layer ? g_cd : g_write, &p, 0, root, sizeof root);
        d = opendir(root);
        if (!d) continue;
        while ((e = readdir(d)) != NULL && n < max) {
            int dup = 0;
            if (!wild_match(pat, e->d_name) || strlen(e->d_name) >= 64) continue;
            if (e->d_name[0] == '.' && strcmp(pat, "*") != 0 && strcmp(pat, "*.*") != 0) continue;
            for (int i = 0; i < n; i++) if (gstr_cmp_ci(names[i], e->d_name) == 0) dup = 1;
            if (!dup) snprintf(names[n++], 64, "%s", e->d_name);
        }
        closedir(d);
    }
    (void)hp;
    /* simple alphabetical order (insertion) */
    for (int i = 1; i < n; i++) {
        char t[64];
        int j = i;
        snprintf(t, sizeof t, "%s", names[i]);
        while (j > 0 && gstr_cmp_ci(names[j - 1], t) > 0) { snprintf(names[j], 64, "%s", names[j - 1]); j--; }
        snprintf(names[j], 64, "%s", t);
    }
    return n;
}
