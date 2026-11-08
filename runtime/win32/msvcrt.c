/* MSVCRT20 (cdecl) over guest memory.
 * MSVC CRT semantics: rand = LCG 214013/2531011, MSVCRT qsort (cutoff 8),
 * text-mode stdio translates CRLF and treats Ctrl-Z as EOF, %e/%g with 3-digit exponent. */
#include "win32.h"
#include "../host/host.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void win32_exit(int code);

/* ---- MSVC formatting ------------------------------------------------------------------------ */
static void fix_exp(char *s) {
    char *e = s, tmp[128];
    for (; *e; e++) {
        if ((*e == 'e' || *e == 'E') && (e[1] == '+' || e[1] == '-')) {
            char *d = e + 2;
            int n = 0;
            while (d[n] >= '0' && d[n] <= '9') n++;
            if (n == 2) {                      /* host: 2 digits; MSVC: 3 */
                snprintf(tmp, sizeof tmp, "%s", d);
                d[0] = '0'; memcpy(d + 1, tmp, strlen(tmp) + 1);
            }
            return;
        }
    }
}

int gformat(char *out, size_t cap, const char *f, uint32_t ap) {
    size_t o = 0;
#define EMIT(c) do { if (o + 1 < cap) out[o] = (char)(c); o++; } while (0)
    while (*f) {
        char flags[8], spec[64], body[8192];
        int nf = 0, width = -1, prec = -1, lng = 0, hh = 0, i64 = 0;
        char c;
        size_t bl, pad;
        if (*f != '%') { EMIT(*f); f++; continue; }
        f++;
        if (*f == '%') { EMIT('%'); f++; continue; }
        for (; *f == '-' || *f == '+' || *f == ' ' || *f == '#' || *f == '0'; f++)
            if (nf < 7) flags[nf++] = *f;
        flags[nf] = 0;
        if (*f == '*') { width = (int)R32(ap); ap += 4u; f++; if (width < 0) { width = -width; if (nf < 7) { flags[nf++] = '-'; flags[nf] = 0; } } }
        else if (*f >= '0' && *f <= '9') { width = 0; while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0'); }
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = (int)R32(ap); ap += 4u; f++; if (prec < 0) prec = -1; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        for (;;) {
            if (*f == 'l') { lng = 1; f++; }
            else if (*f == 'h') { hh = 1; f++; }
            else if (*f == 'L' || *f == 'w') { f++; }
            else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { i64 = 1; f += 3; }
            else break;
        }
        c = *f;
        if (!c) break;
        f++;
        body[0] = 0;
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            long long v;
            if (i64) { v = (long long)(((uint64_t)R32(ap + 4u) << 32) | R32(ap)); ap += 8u; }
            else {
                uint32_t w = R32(ap); ap += 4u;
                if (c == 'd' || c == 'i') v = hh ? (long long)(int16_t)w : (long long)(int32_t)w;
                else v = hh ? (long long)(uint16_t)w : (long long)(uint32_t)w;
            }
            (void)lng;
            /* manual conversion: Retro68's newlib (OS 9) does not print %lld */
            { char dg[72]; int nd = 0, neg = 0, plus = 0, sp = 0, alt = 0, k;
              uint64_t u;
              unsigned base = (c == 'x' || c == 'X') ? 16u : c == 'o' ? 8u : 10u;
              const char *dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
              size_t bn = 0;
              for (k = 0; flags[k]; k++) { if (flags[k] == '+') plus = 1; if (flags[k] == ' ') sp = 1; if (flags[k] == '#') alt = 1; }
              if ((c == 'd' || c == 'i') && v < 0) { neg = 1; u = (uint64_t)(-(v + 1)) + 1u; } else u = (uint64_t)v;
              do { dg[nd++] = dig[u % base]; u /= base; } while (u && nd < 70);
              if (!(prec == 0 && nd == 1 && dg[0] == '0')) { } else nd = 0;
              while (prec > nd && nd < 70) dg[nd++] = '0';
              if (neg) body[bn++] = '-'; else if ((c == 'd' || c == 'i') && plus) body[bn++] = '+'; else if ((c == 'd' || c == 'i') && sp) body[bn++] = ' ';
              if (alt && v != 0 && base == 16u) { body[bn++] = '0'; body[bn++] = c; }
              else if (alt && base == 8u && (nd == 0 || dg[nd - 1] != '0')) body[bn++] = '0';
              while (nd) body[bn++] = dg[--nd];
              body[bn] = 0;
            }
            break;
        }
        case 'c':
            body[0] = (char)(R32(ap) & 0xFFu); body[1] = 0; ap += 4u;
            break;
        case 's': {
            uint32_t a = R32(ap);
            ap += 4u;
            if (!a) snprintf(body, sizeof body, "(null)");
            else {
                size_t n = 0;
                while (n + 1 < sizeof body && R8(a + (uint32_t)n) && (prec < 0 || (int)n < prec)) { body[n] = (char)R8(a + (uint32_t)n); n++; }
                body[n] = 0;
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G': {
            double d = R64D(ap);
            ap += 8u;
            snprintf(spec, sizeof spec, "%%%s", flags);
            if (prec >= 0) snprintf(spec + strlen(spec), sizeof spec - strlen(spec), ".%d", prec);
            snprintf(spec + strlen(spec), sizeof spec - strlen(spec), "%c", c);
            snprintf(body, sizeof body, spec, d);
            if (c != 'f') fix_exp(body);
            break;
        }
        case 'p':
            snprintf(body, sizeof body, "%08X", R32(ap)); ap += 4u;
            break;
        case 'n':
            ap += 4u;
            break;
        default:
            body[0] = c; body[1] = 0;
            break;
        }
        bl = strlen(body);
        pad = (width > 0 && (size_t)width > bl) ? (size_t)width - bl : 0;
        { int left = 0, zero = 0, k;
          for (k = 0; flags[k]; k++) { if (flags[k] == '-') left = 1; if (flags[k] == '0') zero = 1; }
          if (c == 's' || c == 'c') zero = 0;
          if ((c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'o') && prec >= 0) zero = 0;
          if (!left && !zero) for (size_t k2 = 0; k2 < pad; k2++) EMIT(' ');
          if (!left && zero) {
              size_t s0 = 0;
              if (body[0] == '-' || body[0] == '+' || body[0] == ' ') { EMIT(body[0]); s0 = 1; }
              if ((body[s0] == '0') && (body[s0 + 1] == 'x' || body[s0 + 1] == 'X')) { EMIT(body[s0]); EMIT(body[s0 + 1]); s0 += 2; }
              for (size_t k2 = 0; k2 < pad; k2++) EMIT('0');
              for (size_t k2 = s0; k2 < bl; k2++) EMIT(body[k2]);
          } else {
              for (size_t k2 = 0; k2 < bl; k2++) EMIT(body[k2]);
          }
          if (left) for (size_t k2 = 0; k2 < pad; k2++) EMIT(' ');
        }
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return (int)o;
#undef EMIT
}

/* ---- stdio over host fd -------------------------------------------------------------------- */
typedef struct {
    int used, fd, text, eof, err, writing;
    uint32_t gptr;                    /* FILE* guest */
    uint8_t buf[4096];
    int bpos, blen;
    long raw_off;                     /* raw offset of the buffer start */
} HFile;

#define MAX_FILES 128
static HFile files[MAX_FILES];
static uint32_t v_fmode_ptr_;
#define crt_fmode_text (v_fmode_ptr_ ? ((R32(v_fmode_ptr_) & 0x8000u) == 0) : 1)
static uint32_t gerrno;

static void set_errno(int e) { if (gerrno) W32(gerrno, (uint32_t)e); }

static int fill(HFile *f) {
    ssize_t n;
    f->raw_off += f->blen;
    n = read(f->fd, f->buf, sizeof f->buf);
    f->bpos = 0;
    f->blen = n > 0 ? (int)n : 0;
    if (n <= 0) { if (n < 0) f->err = 1; return 0; }
    return 1;
}

/* reads one logical byte (translates CRLF and Ctrl-Z in text mode); -1 = EOF */
static int getb(HFile *f) {
    int c;
    if (f->eof) return -1;
    if (f->bpos >= f->blen && !fill(f)) { f->eof = 1; return -1; }
    c = f->buf[f->bpos++];
    if (f->text) {
        if (c == 0x1A) { f->eof = 1; f->bpos--; return -1; }
        if (c == '\r') {
            if (f->bpos >= f->blen) fill(f);
            if (f->bpos < f->blen && f->buf[f->bpos] == '\n') c = f->buf[f->bpos++];
        }
    }
    return c;
}

static long raw_tell(HFile *f) { return f->raw_off + f->bpos; }

static int hf_write(HFile *f, const uint8_t *p, size_t n) {
    uint8_t tmp[8192];
    size_t i, o = 0;
    if (f->blen) { lseek(f->fd, raw_tell(f), SEEK_SET); f->raw_off = raw_tell(f); f->bpos = f->blen = 0; }
    for (i = 0; i < n; i++) {
        if (f->text && p[i] == '\n') tmp[o++] = '\r';
        tmp[o++] = p[i];
        if (o + 2 > sizeof tmp) { if (write(f->fd, tmp, o) != (ssize_t)o) return -1; o = 0; }
    }
    if (o && write(f->fd, tmp, o) != (ssize_t)o) return -1;
    return 0;
}

static HFile *alloc_file(void) {
    for (int i = 0; i < MAX_FILES; i++)
        if (!files[i].used) {
            memset(&files[i], 0, sizeof files[i]);
            files[i].used = 1;
            files[i].gptr = heap_alloc(32);
            return &files[i];
        }
    return 0;
}
static HFile *file_of(uint32_t gp) {
    for (int i = 0; i < MAX_FILES; i++) if (files[i].used && files[i].gptr == gp) return &files[i];
    return 0;
}

static uint32_t m_fopen(void) {
    char path[512], mode[16], hp[2048];
    int fl, mread, mplus, text = crt_fmode_text;
    HFile *f;
    gstr_get(path, sizeof path, ARG(0));
    gstr_get(mode, sizeof mode, ARG(1));
    mread = mode[0] == 'r';
    mplus = strchr(mode, '+') != NULL;
    if (strchr(mode, 'b')) text = 0;
    if (strchr(mode, 't')) text = 1;
    fl = mread ? (mplus ? O_RDWR : O_RDONLY)
               : mode[0] == 'w' ? ((mplus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC)
                                : ((mplus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND);
    vfs_resolve(path, !mread || mplus, hp, sizeof hp);
    { int fd = open(hp, fl, 0666);
      if (fd < 0) { set_errno(errno == ENOENT ? 2 : 13); return 0; }
      f = alloc_file();
      if (!f) { close(fd); return 0; }
      f->fd = fd; f->text = text;
    }
    return f->gptr;
}
static uint32_t m_fclose(void) {
    HFile *f = file_of(ARG(0));
    if (!f) return 0xFFFFFFFFu;
    close(f->fd);
    heap_free(f->gptr);
    f->used = 0;
    return 0;
}
static uint32_t m_fgets(void) {
    uint32_t buf = ARG(0), n = ARG(1), i = 0;
    HFile *f = file_of(ARG(2));
    if (!f || n == 0) return 0;
    while (i + 1 < n) {
        int c = getb(f);
        if (c < 0) break;
        W8(buf + i++, (uint32_t)c);
        if (c == '\n') break;
    }
    if (i == 0) return 0;
    W8(buf + i, 0);
    return buf;
}
static uint32_t m_fread(void) {
    uint32_t buf = ARG(0), sz = ARG(1), cnt = ARG(2), total = sz * cnt, i = 0;
    HFile *f = file_of(ARG(3));
    if (!f || !total) return 0;
    if (!f->text) {                       /* binary: uses the block buffer */
        while (i < total) {
            uint32_t k;
            if (f->bpos >= f->blen && !fill(f)) { f->eof = 1; break; }
            k = (uint32_t)(f->blen - f->bpos);
            if (k > total - i) k = total - i;
            gmem_put(buf + i, f->buf + f->bpos, k);
            f->bpos += (int)k; i += k;
        }
    } else {
        while (i < total) {
            int c = getb(f);
            if (c < 0) break;
            W8(buf + i++, (uint32_t)c);
        }
    }
    return i / sz;
}
static uint32_t m_fwrite(void) {
    HFile *f = file_of(ARG(3));
    uint32_t total = ARG(1) * ARG(2);
    if (!f || !total) return 0;
    return hf_write(f, guest_ptr(ARG(0), total), total) == 0 ? ARG(2) : 0;
}
static uint32_t m_fprintf(void) {
    static char out[65536];
    HFile *f = file_of(ARG(0));
    char fmt[4096];
    int n;
    gstr_get(fmt, sizeof fmt, ARG(1));
    n = gformat(out, sizeof out, fmt, g.esp + 12u);
    if (!f) return 0xFFFFFFFFu;
    if (n >= (int)sizeof out) n = (int)sizeof out - 1;
    hf_write(f, (const uint8_t *)out, (size_t)n);
    return (uint32_t)n;
}
static uint32_t m_fflush(void) { return 0; }
static uint32_t m_fseek(void) {
    HFile *f = file_of(ARG(0));
    int wh = (int)ARG(2);
    long off = (long)(int32_t)ARG(1), r;
    if (!f) return 0xFFFFFFFFu;
    if (wh == 1) { off += raw_tell(f); wh = 0; }
    r = (long)lseek(f->fd, off, wh == 2 ? SEEK_END : SEEK_SET);
    if (r < 0) return 0xFFFFFFFFu;
    f->raw_off = r; f->bpos = f->blen = 0; f->eof = 0;
    return 0;
}
static uint32_t m_ftell(void) {
    HFile *f = file_of(ARG(0));
    return f ? (uint32_t)raw_tell(f) : 0xFFFFFFFFu;
}

/* low-level fds (_open/_read/...): host fd + 3; text/binary per fd */
#define FD_BASE 3
static int fd_text[1024];
static uint32_t m_open(void) {
    char path[512], hp[2048];
    int of = (int)ARG(1), fl = 0, fd;
    gstr_get(path, sizeof path, ARG(0));
    fl = (of & 3) == 0 ? O_RDONLY : (of & 3) == 1 ? O_WRONLY : O_RDWR;
    if (of & 8) fl |= O_APPEND;
    if (of & 0x100) fl |= O_CREAT;
    if (of & 0x200) fl |= O_TRUNC;
    if (of & 0x400) fl |= O_EXCL;
    vfs_resolve(path, (of & 3) != 0 || (of & 0x300), hp, sizeof hp);
    fd = open(hp, fl, 0666);
    if (fd < 0) { set_errno(errno == ENOENT ? 2 : 13); return 0xFFFFFFFFu; }
    if (fd + FD_BASE >= 1024) { close(fd); return 0xFFFFFFFFu; }
    fd_text[fd] = (of & 0x8000) ? 0 : (of & 0x4000) ? 1 : crt_fmode_text;
    return (uint32_t)(fd + FD_BASE);
}
static int hfd(uint32_t fd) { return fd >= FD_BASE ? (int)fd - FD_BASE : -1; }
static uint32_t m_close(void) { int fd = hfd(ARG(0)); return fd >= 0 && close(fd) == 0 ? 0 : 0xFFFFFFFFu; }
static uint32_t m_read(void) {
    int fd = hfd(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2);
    ssize_t r;
    if (fd < 0) return 0xFFFFFFFFu;
    r = read(fd, guest_ptr(buf, n), n);
    if (r < 0) return 0xFFFFFFFFu;
    if (fd_text[fd]) {                              /* CRLF -> LF; Ctrl-Z ends */
        ssize_t i, o = 0;
        for (i = 0; i < r; i++) {
            uint8_t c = M[buf + (uint32_t)i];
            if (c == 0x1A) { lseek(fd, -(off_t)(r - i), SEEK_CUR); break; }
            if (c == '\r' && i + 1 < r && M[buf + (uint32_t)i + 1] == '\n') continue;
            M[buf + (uint32_t)o++] = c;
        }
        r = o;
    }
    return (uint32_t)r;
}
static uint32_t m_write(void) {
    int fd = hfd(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2);
    ssize_t r;
    if (fd < 0) return 0xFFFFFFFFu;
    if (fd_text[fd]) {
        uint8_t tmp[8192];
        uint32_t i, o = 0;
        for (i = 0; i < n; i++) {
            if (M[buf + i] == '\n') tmp[o++] = '\r';
            tmp[o++] = M[buf + i];
            if (o + 2 > sizeof tmp) { if (write(fd, tmp, o) != (ssize_t)o) return 0xFFFFFFFFu; o = 0; }
        }
        if (o && write(fd, tmp, o) != (ssize_t)o) return 0xFFFFFFFFu;
        return n;
    }
    r = write(fd, guest_ptr(buf, n), n);
    return r < 0 ? 0xFFFFFFFFu : (uint32_t)r;
}
static uint32_t m_filelength(void) {
    int fd = hfd(ARG(0));
    struct stat st;
    return fd >= 0 && fstat(fd, &st) == 0 ? (uint32_t)st.st_size : 0xFFFFFFFFu;
}
static uint32_t m_eof(void) {
    int fd = hfd(ARG(0));
    struct stat st;
    off_t p;
    if (fd < 0 || fstat(fd, &st) != 0) return 0xFFFFFFFFu;
    p = lseek(fd, 0, SEEK_CUR);
    return p >= st.st_size ? 1u : 0u;
}
static uint32_t m_unlink(void) {
    char path[512], hp[2048];
    gstr_get(path, sizeof path, ARG(0));
    vfs_resolve(path, 1, hp, sizeof hp);
    return unlink(hp) == 0 ? 0 : 0xFFFFFFFFu;
}
static uint32_t m_chmod(void) { return 0; }
static uint32_t m_stat(void) {
    char path[512], hp[2048];
    struct stat st;
    uint32_t b = ARG(1);
    gstr_get(path, sizeof path, ARG(0));
    vfs_resolve(path, 0, hp, sizeof hp);
    if (stat(hp, &st) != 0) { set_errno(2); return 0xFFFFFFFFu; }
    for (uint32_t i = 0; i < 36; i += 4) W32(b + i, 0);
    W16(b + 6, (S_ISDIR(st.st_mode) ? 0x4000u : 0x8000u) | 0x1A4u);
    W16(b + 8, 1);
    W32(b + 20, (uint32_t)st.st_size);
    W32(b + 24, (uint32_t)st.st_atime); W32(b + 28, (uint32_t)st.st_mtime); W32(b + 32, (uint32_t)st.st_ctime);
    return 0;
}
static uint32_t m_chdir(void) {
    char path[512], hp[2048];
    struct stat st;
    gstr_get(path, sizeof path, ARG(0));
    vfs_resolve(path, 0, hp, sizeof hp);
    if (stat(hp, &st) != 0 || !S_ISDIR(st.st_mode)) { set_errno(2); return 0xFFFFFFFFu; }
    vfs_set_cwd(path);
    return 0;
}
static uint32_t m_getdrive(void) { return 3; }
static uint32_t m_chdrive(void) { return 0; }
static uint32_t m_getdcwd(void) {
    uint32_t buf = ARG(1), n = ARG(2);
    const char *c = vfs_cwd();
    if (!buf) { n = (uint32_t)strlen(c) + 1u; buf = heap_alloc(n); }
    if (strlen(c) + 1 > n) return 0;
    gstr_put(buf, c);
    return buf;
}

/* ---- memory and strings ------------------------------------------------------------------------ */
static uint32_t m_malloc(void) { return heap_alloc(ARG(0)); }
static uint32_t m_calloc(void) {
    uint64_t t = (uint64_t)ARG(0) * ARG(1);
    uint32_t p;
    if (t > MEM_SIZE) return 0;
    p = heap_alloc((uint32_t)t);
    if (p) memset(M + p, 0, (size_t)t);
    return p;
}
static uint32_t m_free(void) { heap_free(ARG(0)); return 0; }
static uint32_t m_realloc(void) { return heap_realloc(ARG(0), ARG(1)); }
static uint32_t m_msize(void) { return heap_size(ARG(0)); }

static uint32_t m_memcpy(void) { memmove(guest_ptr(ARG(0), ARG(2)), guest_ptr(ARG(1), ARG(2)), ARG(2)); return ARG(0); }
static uint32_t m_memset(void) { memset(guest_ptr(ARG(0), ARG(2)), (int)ARG(1), ARG(2)); return ARG(0); }
static uint32_t m_memcmp(void) {
    int r = memcmp(guest_ptr(ARG(0), ARG(2)), guest_ptr(ARG(1), ARG(2)), ARG(2));
    return (uint32_t)(r < 0 ? -1 : r > 0 ? 1 : 0);
}
static uint32_t m_memchr(void) {
    uint32_t a = ARG(0), n = ARG(2), i;
    for (i = 0; i < n; i++) if (R8(a + i) == (ARG(1) & 0xFFu)) return a + i;
    return 0;
}
static uint32_t m_strlen(void) { return gstr_len(ARG(0)); }
static uint32_t m_strcpy(void) {
    uint32_t d = ARG(0), s = ARG(1), i = 0, c;
    do { c = R8(s + i); W8(d + i, c); i++; } while (c);
    return d;
}
static uint32_t m_strncpy(void) {
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2), i = 0;
    for (; i < n; i++) { uint32_t c = R8(s + i); W8(d + i, c); if (!c) break; }
    for (i++; i < n; i++) W8(d + i, 0);
    return d;
}
static uint32_t m_strcat(void) {
    uint32_t d = ARG(0) + gstr_len(ARG(0)), s = ARG(1), i = 0, c;
    do { c = R8(s + i); W8(d + i, c); i++; } while (c);
    return ARG(0);
}
static int cmp_bytes(uint32_t a, uint32_t b, uint32_t n, int ci) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint32_t x = R8(a + i), y = R8(b + i);
        if (ci) { if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; }
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
static uint32_t m_strcmp(void) { return (uint32_t)cmp_bytes(ARG(0), ARG(1), 0xFFFFFFFFu, 0); }
static uint32_t m_strncmp(void) { return (uint32_t)cmp_bytes(ARG(0), ARG(1), ARG(2), 0); }
static uint32_t m_stricmp(void) { return (uint32_t)cmp_bytes(ARG(0), ARG(1), 0xFFFFFFFFu, 1); }
static uint32_t m_strnicmp(void) { return (uint32_t)cmp_bytes(ARG(0), ARG(1), ARG(2), 1); }
static uint32_t m_strchr(void) {
    uint32_t a = ARG(0), ch = ARG(1) & 0xFFu;
    for (;; a++) { uint32_t c = R8(a); if (c == ch) return a; if (!c) return 0; }
}
static uint32_t m_strrchr(void) {
    uint32_t a = ARG(0), ch = ARG(1) & 0xFFu, r = 0;
    for (;; a++) { uint32_t c = R8(a); if (c == ch) r = a; if (!c) return r; }
}
static uint32_t m_strstr(void) {
    uint32_t a = ARG(0), b = ARG(1), nl = gstr_len(b), i;
    if (!nl) return a;
    for (;; a++) {
        if (!R8(a)) return 0;
        for (i = 0; i < nl && R8(a + i) == R8(b + i); i++) {}
        if (i == nl) return a;
    }
}
static uint32_t strtok_next;
static int in_set(uint32_t c, uint32_t set) {
    for (uint32_t i = 0; R8(set + i); i++) if (R8(set + i) == c) return 1;
    return 0;
}
static uint32_t m_strtok(void) {
    uint32_t s = ARG(0), d = ARG(1), start;
    if (!s) s = strtok_next;
    if (!s) return 0;
    while (R8(s) && in_set(R8(s), d)) s++;
    if (!R8(s)) { strtok_next = 0; return 0; }
    start = s;
    while (R8(s) && !in_set(R8(s), d)) s++;
    if (R8(s)) { W8(s, 0); strtok_next = s + 1u; } else strtok_next = 0;
    return start;
}
static uint32_t m_atoi(void) {
    char b[64];
    gstr_get(b, sizeof b, ARG(0));
    return (uint32_t)(int32_t)strtol(b, NULL, 10);
}
static uint32_t m_strtol(void) {
    char b[128], *e;
    long v;
    gstr_get(b, sizeof b, ARG(0));
    v = strtol(b, &e, (int)ARG(2));
    if (ARG(1)) W32(ARG(1), ARG(0) + (uint32_t)(e - b));
    if (v > 2147483647L) v = 2147483647L;
    if (v < -2147483647L - 1L) v = -2147483647L - 1L;
    return (uint32_t)(int32_t)v;
}
static uint32_t put_radix(uint32_t buf, long long v, int radix, int is_signed) {
    char t[72];
    int n = 0, neg = 0, i;
    unsigned long long u;
    if (is_signed && v < 0) { neg = 1; u = (unsigned long long)(-v); } else u = (unsigned long long)v;
    if (!is_signed) u &= 0xFFFFFFFFu;
    do { int d = (int)(u % (unsigned)radix); t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (unsigned)radix; } while (u);
    if (neg) t[n++] = '-';
    for (i = 0; i < n; i++) W8(buf + (uint32_t)i, (uint32_t)t[n - 1 - i]);
    W8(buf + (uint32_t)n, 0);
    return buf;
}
static uint32_t m_itoa(void) { return put_radix(ARG(1), (int32_t)ARG(0), (int)ARG(2), (int)ARG(2) == 10); }
static uint32_t m_ltoa(void) { return put_radix(ARG(1), (int32_t)ARG(0), (int)ARG(2), (int)ARG(2) == 10); }
static uint32_t m_toupper(void) { uint32_t c = ARG(0); return c >= 'a' && c <= 'z' ? c - 32 : c; }
static uint32_t m_tolower(void) { uint32_t c = ARG(0); return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static uint32_t m_abs(void) { int32_t v = (int32_t)ARG(0); return (uint32_t)(v < 0 ? -v : v); }
static uint32_t m_isctype(void) {
    uint32_t c = ARG(0), m = ARG(1), r = 0;
    if (c > 255) return 0;
    if (c >= 'A' && c <= 'Z') r |= 0x1u | 0x100u;
    if (c >= 'a' && c <= 'z') r |= 0x2u | 0x100u;
    if (c >= '0' && c <= '9') r |= 0x4u;
    if (c == ' ' || (c >= 9 && c <= 13)) r |= 0x8u;
    if (c > 32 && c < 127 && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) r |= 0x10u;
    if (c < 32 || c == 127) r |= 0x20u;
    if (c == ' ' || c == 9) r |= 0x40u;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) r |= 0x80u;
    return (r & m) != 0;
}

static uint32_t m_sprintf(void) {
    static char out[65536];
    char fmt[4096];
    int n;
    gstr_get(fmt, sizeof fmt, ARG(1));
    n = gformat(out, sizeof out, fmt, g.esp + 12u);
    if (n >= (int)sizeof out) n = (int)sizeof out - 1;
    gmem_put(ARG(0), out, (uint32_t)n + 1u);
    return (uint32_t)n;
}

/* ---- random, sorting ------------------------------------------------------------------------- */
static uint32_t rand_state = 1;
static uint32_t m_srand(void) { rand_state = ARG(0); return 0; }
static uint32_t m_rand(void) { rand_state = rand_state * 214013u + 2531011u; return (rand_state >> 16) & 0x7FFFu; }

static uint32_t qs_cmp, qs_w;
static int qs_c(uint32_t a, uint32_t b) { return (int32_t)guest_call_cdecl(qs_cmp, 2, a, b); }
static void qs_swap(uint32_t a, uint32_t b) {
    uint32_t i;
    if (a == b) return;
    for (i = 0; i < qs_w; i++) { uint8_t t = M[a + i]; M[a + i] = M[b + i]; M[b + i] = t; }
}
static void qs_short(uint32_t lo, uint32_t hi) {
    while (hi > lo) {
        uint32_t mx = lo, p = lo + qs_w;
        for (; p <= hi; p += qs_w) if (qs_c(p, mx) > 0) mx = p;
        qs_swap(mx, hi);
        hi -= qs_w;
    }
}
static uint32_t m_qsort(void) {
    uint32_t base = ARG(0), num = ARG(1), w = ARG(2), lo, hi;
    uint32_t lostk[64], histk[64];
    int sp = 0;
    if (num < 2 || w == 0) return 0;
    qs_cmp = ARG(3); qs_w = w;
    lo = base; hi = base + w * (num - 1u);
    for (;;) {
        int recurse = 0;
        uint32_t size = (hi - lo) / w + 1u;
        if (size <= 8) qs_short(lo, hi);
        else {
            uint32_t loguy, higuy;
            qs_swap(lo + (size / 2u) * w, lo);
            loguy = lo; higuy = hi + w;
            for (;;) {
                do { loguy += w; } while (loguy <= hi && qs_c(loguy, lo) <= 0);
                do { higuy -= w; } while (higuy > lo && qs_c(higuy, lo) >= 0);
                if (higuy < loguy) break;
                qs_swap(loguy, higuy);
            }
            qs_swap(lo, higuy);
            if (higuy - 1u - lo >= hi - loguy) {
                if (lo + w < higuy) { lostk[sp] = lo; histk[sp] = higuy - w; sp++; }
                if (loguy < hi) { lo = loguy; recurse = 1; }
            } else {
                if (loguy < hi) { lostk[sp] = loguy; histk[sp] = hi; sp++; }
                if (lo + w < higuy) { hi = higuy - w; recurse = 1; }
            }
        }
        if (recurse) continue;
        if (sp == 0) return 0;
        sp--; lo = lostk[sp]; hi = histk[sp];
    }
}

/* ---- time ------------------------------------------------------------------------------------- */
static uint32_t now_epoch(void) {
    return opt_fixed_time_set ? opt_fixed_time + win32_now_ms() / 1000u : (uint32_t)time(NULL);
}
static uint32_t tm_buf;
static uint32_t put_tm(const struct tm *t) {
    if (!tm_buf) tm_buf = heap_alloc(36);
    W32(tm_buf + 0, (uint32_t)t->tm_sec); W32(tm_buf + 4, (uint32_t)t->tm_min); W32(tm_buf + 8, (uint32_t)t->tm_hour);
    W32(tm_buf + 12, (uint32_t)t->tm_mday); W32(tm_buf + 16, (uint32_t)t->tm_mon); W32(tm_buf + 20, (uint32_t)t->tm_year);
    W32(tm_buf + 24, (uint32_t)t->tm_wday); W32(tm_buf + 28, (uint32_t)t->tm_yday); W32(tm_buf + 32, (uint32_t)t->tm_isdst);
    return tm_buf;
}
static uint32_t m_time(void) {
    uint32_t t = now_epoch();
    if (ARG(0)) W32(ARG(0), t);
    return t;
}
static uint32_t m_localtime(void) {
    time_t t = (time_t)R32(ARG(0));
    struct tm r;
    if (opt_fixed_time_set) gmtime_r(&t, &r); else localtime_r(&t, &r);
    return put_tm(&r);
}
static uint32_t m_gmtime(void) {
    time_t t = (time_t)R32(ARG(0));
    struct tm r;
    gmtime_r(&t, &r);
    return put_tm(&r);
}
static uint32_t m_strdate(void) {
    time_t t = (time_t)now_epoch();
    struct tm r;
    char b[16];
    if (opt_fixed_time_set) gmtime_r(&t, &r); else localtime_r(&t, &r);
    snprintf(b, sizeof b, "%02d/%02d/%02d", r.tm_mon + 1, r.tm_mday, r.tm_year % 100);
    gstr_put(ARG(0), b);
    return ARG(0);
}

/* ---- floating point (result in ST0) ------------------------------------------------------------ */
static uint32_t m_sqrt(void) { fx_push(sqrt(R64D(g.esp + 4u))); return 0; }
static uint32_t m_sin(void) { fx_push(sin(R64D(g.esp + 4u))); return 0; }
static uint32_t m_cos(void) { fx_push(cos(R64D(g.esp + 4u))); return 0; }
static uint32_t m_asin(void) { fx_push(asin(R64D(g.esp + 4u))); return 0; }
static uint32_t m_acos(void) { fx_push(acos(R64D(g.esp + 4u))); return 0; }
static uint32_t m_atan2(void) { fx_push(atan2(R64D(g.esp + 4u), R64D(g.esp + 12u))); return 0; }
static uint32_t m_ftol(void) {
    double v = fx_st0();
    int64_t r;
    fx_pop();
    if (v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0) r = INT64_MIN;
    else r = (int64_t)v;
    g.edx = (uint32_t)((uint64_t)r >> 32);
    return (uint32_t)r;
}
static uint32_t cw_state = 0x9001Fu;
static uint32_t m_controlfp(void) {
    uint32_t nw = ARG(0), mask = ARG(1);
    if (mask) cw_state = (cw_state & ~mask) | (nw & mask);
    return cw_state;
}

/* ---- startup ---------------------------------------------------------------------------------------------- */
static uint32_t v_fmode, v_commode, v_acmdln, v_cmdline;
static uint32_t m_p_fmode(void) { if (!v_fmode) { v_fmode = heap_alloc(4); W32(v_fmode, 0); v_fmode_ptr_ = v_fmode; } return v_fmode; }
static uint32_t m_p_commode(void) { if (!v_commode) v_commode = heap_alloc(4); return v_commode; }
static uint32_t m_p_acmdln(void) {
    if (!v_acmdln) {
        v_acmdln = heap_alloc(4);
        v_cmdline = heap_alloc(32);
        gstr_put(v_cmdline, "C:\\GPM2\\Gpm2.exe");
        W32(v_acmdln, v_cmdline);
    }
    return v_acmdln;
}
static uint32_t m_getmainargs(void) {
    uint32_t argv = heap_alloc(8), envp = heap_alloc(4), a0;
    m_p_acmdln();
    a0 = v_cmdline;
    W32(argv, a0); W32(argv + 4u, 0);
    W32(envp, 0);
    W32(ARG(0), 1); W32(ARG(1), argv); W32(ARG(2), envp);
    return 0;
}
static uint32_t m_initterm(void) {
    uint32_t p;
    for (p = ARG(0); p < ARG(1); p += 4u) {
        uint32_t fn = R32(p);
        if (fn) guest_call_cdecl(fn, 0);
    }
    return 0;
}
static uint32_t m_exit(void) { win32_exit((int)ARG(0)); return 0; }
static uint32_t m_errno(void) { if (!gerrno) gerrno = heap_alloc(4); return gerrno; }
static uint32_t m_onexit(void) { return ARG(0); }
static uint32_t m_dllonexit(void) { return ARG(0); }
static uint32_t m_zero(void) { return 0; }

void msvcrt_register(void) {
#define R(n, f) win32_register("MSVCRT20.dll!" n, f)
    R("fopen", m_fopen); R("fclose", m_fclose); R("fgets", m_fgets); R("fread", m_fread); R("fwrite", m_fwrite);
    R("fprintf", m_fprintf); R("fflush", m_fflush); R("fseek", m_fseek); R("ftell", m_ftell);
    R("_open", m_open); R("_close", m_close); R("_read", m_read); R("_write", m_write);
    R("_filelength", m_filelength); R("_eof", m_eof); R("_unlink", m_unlink); R("_chmod", m_chmod);
    R("_stat", m_stat); R("_chdir", m_chdir); R("_getdrive", m_getdrive); R("_chdrive", m_chdrive);
    R("_getdcwd", m_getdcwd);
    R("malloc", m_malloc); R("calloc", m_calloc); R("free", m_free); R("realloc", m_realloc); R("_msize", m_msize);
    R("memcpy", m_memcpy); R("memmove", m_memcpy); R("memset", m_memset); R("memcmp", m_memcmp); R("memchr", m_memchr);
    R("strlen", m_strlen); R("strcpy", m_strcpy); R("strncpy", m_strncpy); R("strcat", m_strcat);
    R("strcmp", m_strcmp); R("strncmp", m_strncmp); R("_stricmp", m_stricmp); R("_strnicmp", m_strnicmp);
    R("strchr", m_strchr); R("strrchr", m_strrchr); R("strstr", m_strstr); R("strtok", m_strtok);
    R("atoi", m_atoi); R("atol", m_atoi); R("strtol", m_strtol); R("_itoa", m_itoa); R("_ltoa", m_ltoa);
    R("toupper", m_toupper); R("tolower", m_tolower); R("abs", m_abs); R("_isctype", m_isctype);
    R("sprintf", m_sprintf);
    R("srand", m_srand); R("rand", m_rand); R("qsort", m_qsort);
    R("time", m_time); R("localtime", m_localtime); R("gmtime", m_gmtime); R("_strdate", m_strdate);
    R("sqrt", m_sqrt); R("sin", m_sin); R("cos", m_cos); R("asin", m_asin); R("acos", m_acos); R("atan2", m_atan2);
    R("_ftol", m_ftol); R("_controlfp", m_controlfp);
    R("__p__fmode", m_p_fmode); R("__p__commode", m_p_commode); R("__p__acmdln", m_p_acmdln);
    R("__getmainargs", m_getmainargs); R("_initterm", m_initterm);
    R("exit", m_exit); R("_exit", m_exit); R("_errno", m_errno);
    R("_onexit", m_onexit); R("__dllonexit", m_dllonexit);
    R("_except_handler3", m_zero); R("_XcptFilter", m_zero);
#undef R
}
