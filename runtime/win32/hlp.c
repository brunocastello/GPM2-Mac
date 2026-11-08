/* GPMHELP.HLP (WinHelp 3.x) -> cached HTML pages. Portable C port of tools/hlp2html.py (no images).
 * hlp_prepare() finds the .HLP in the game folder (case-insensitive), converts it once into <wdir>/Help and
 * regenerates when the .HLP changes (size+date stamp). The hosts open index.html with the browser/IC. */
#include "../pathjoin.h"
#include "win32.h"

#include <dirent.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

typedef struct { uint8_t *p; size_t n; } Buf;
typedef struct { char *p; size_t n, cap; } Sb;
static void sb_add(Sb *s, const char *t, size_t n) {
    if (s->n + n + 1 > s->cap) { s->cap = (s->n + n + 1) * 2 + 256; s->p = (char *)realloc(s->p, s->cap); }
    memcpy(s->p + s->n, t, n); s->n += n; s->p[s->n] = 0;
}
static void sb_str(Sb *s, const char *t) { sb_add(s, t, strlen(t)); }
static void sb_fmt(Sb *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void sb_fmt(Sb *s, const char *fmt, ...) {
    char t[256]; va_list ap;
    va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    sb_str(s, t);
}

/* safe read: outside the file it is 0 */
static unsigned u8at(Buf b, size_t o) { return o < b.n ? b.p[o] : 0; }
static unsigned u16at(Buf b, size_t o) { return u8at(b, o) | u8at(b, o + 1) << 8; }
static int s16at(Buf b, size_t o) { return (int16_t)u16at(b, o); }
static uint32_t u32at(Buf b, size_t o) { return u16at(b, o) | (uint32_t)u16at(b, o + 2) << 16; }

typedef struct { char name[48]; uint32_t off; } DirEnt;
static int read_dir(Buf d, DirEnt *out, int max) {
    size_t ds = u32at(d, 4), base = ds + 9 + 38, ps = u16at(d, ds + 9 + 4);
    int page = s16at(d, ds + 9 + 26), nlev = (int)u16at(d, ds + 9 + 32), cnt = 0, guard = 0;
    for (int i = 0; i < nlev - 1; i++) page = s16at(d, base + (size_t)page * ps + 4);
    while (page != -1 && guard++ < 100000) {
        size_t p = base + (size_t)page * ps, o = p + 8;
        unsigned n = u16at(d, p + 2);
        int nxt = s16at(d, p + 6);
        for (unsigned k = 0; k < n && o < d.n; k++) {
            size_t e = o;
            while (e < d.n && d.p[e]) e++;
            if (cnt < max) { size_t l = e - o; if (l > 47) l = 47; memcpy(out[cnt].name, d.p + o, l); out[cnt].name[l] = 0; out[cnt].off = u32at(d, e + 1); cnt++; }
            o = e + 5;
        }
        page = nxt;
    }
    return cnt;
}
static Buf getfile(Buf d, uint32_t off) {
    Buf f; size_t used = u32at(d, off + 4);
    if ((size_t)off + 9 > d.n) { f.p = d.p; f.n = 0; return f; }
    if ((size_t)off + 9 + used > d.n) used = d.n - off - 9;
    f.p = d.p + off + 9; f.n = used;
    return f;
}
static int findfile(Buf d, DirEnt *de, int n, const char *name, Buf *f) {
    for (int i = 0; i < n; i++) if (!strcmp(de[i].name, name)) { *f = getfile(d, de[i].off); return 1; }
    return 0;
}

/* WinHelp compressed integers */
typedef struct { Buf b; size_t o; } Rd;
static int rd_csl(Rd *r) {
    uint32_t v = u16at(r->b, r->o); r->o += 2;
    if (v & 1) { v |= (uint32_t)u16at(r->b, r->o) << 16; r->o += 2; return (int)(v >> 1) - 0x4000000; }
    return (int)(v >> 1) - 0x4000;
}
static void rd_cu(Rd *r) { unsigned b = u8at(r->b, r->o++); if (b & 1) r->o++; }

static uint8_t *lz77(const uint8_t *src, size_t n, size_t *outn) {
    size_t cap = n * 8 + 64, o = 0, i = 0;
    uint8_t *out = (uint8_t *)malloc(cap);
    while (i < n) {
        unsigned mask = src[i++];
        for (int b = 0; b < 8 && i < n; b++) {
            if (mask >> b & 1) {
                unsigned v, len, off;
                if (i + 1 >= n) { i = n; break; }
                v = src[i] | src[i + 1] << 8; i += 2;
                len = (v >> 12) + 3; off = (v & 0xFFF) + 1;
                if (off > o) { i = n; break; }
                if (o + len + 1 > cap) { cap = (o + len) * 2; out = (uint8_t *)realloc(out, cap); }
                while (len--) { out[o] = out[o - off]; o++; }
            } else {
                if (o + 2 > cap) { cap *= 2; out = (uint8_t *)realloc(out, cap); }
                out[o++] = src[i++];
            }
        }
    }
    *outn = o;
    return out;
}

typedef struct { int type; const uint8_t *d1; size_t n1; uint8_t *d2; size_t n2; uint32_t P; } Link;
typedef struct { uint32_t h, v; } Pair;
typedef struct { int attr, hp; unsigned rgb; } Font;
typedef struct { uint32_t P; int ti, ri; } RecPos;
typedef struct { int ti, ri, kind; uint32_t val; } Anchor;
typedef struct { int type; uint32_t a; const uint8_t *s; size_t n; } Cmd;

/* WinHelp 3.0/3.1 phrases (|Phrases); without them (the GPM2 case) the expansion is the identity */
static uint8_t *expand(const uint8_t *b, size_t n, Buf phr, size_t *outn) {
    size_t cnt = u16at(phr, 0), i = 0, o = 0, cap = n * 4 + 64;
    uint8_t *out = (uint8_t *)malloc(cap);
    while (i < n) {
        unsigned c = b[i++];
        if (phr.n && c > 0 && c < 16 && i < n) {
            unsigned ix = c * 256 + b[i++] - 256;
            if (ix / 2 < cnt) {
                size_t s = u16at(phr, 4 + 2 * (ix / 2)), e = u16at(phr, 4 + 2 * (ix / 2 + 1));
                if (e > s && 4 + e <= phr.n) {
                    if (o + (e - s) + 2 > cap) { cap = (o + e - s) * 2; out = (uint8_t *)realloc(out, cap); }
                    memcpy(out + o, phr.p + 4 + s, e - s); o += e - s;
                }
            }
            if (ix & 1) { if (o + 2 > cap) { cap *= 2; out = (uint8_t *)realloc(out, cap); } out[o++] = ' '; }
        } else {
            if (o + 2 > cap) { cap *= 2; out = (uint8_t *)realloc(out, cap); }
            out[o++] = (uint8_t)c;
        }
    }
    *outn = o;
    return out;
}

/* command stream starting at s; returns the count, or -1 if malformed */
static int commands(const uint8_t *d1, size_t len, size_t s, Cmd *out, int max) {
    size_t o = s; int n = 0;
    Buf b; b.p = (uint8_t *)d1; b.n = len;
    while (o < len) {
        unsigned c = d1[o++];
        Cmd *k = &out[n < max ? n : max - 1];
        k->type = (int)c; k->a = 0; k->s = NULL; k->n = 0;
        if (c == 0xFF) return o == len ? n : -1;
        if (c == 0x80) { if (o + 2 > len) return -1; k->a = u16at(b, o); o += 2; }
        else if (c == 0x81 || c == 0x82 || c == 0x83 || c == 0x89 || c == 0x8B || c == 0x8C) { }
        else if (c >= 0x86 && c <= 0x88) {
            Rd r; int l; r.b = b; r.o = o; l = rd_csl(&r);
            if (l < 0) return -1;
            k->n = (size_t)l; o = r.o + (size_t)l;
        }
        else if (c >= 0xE0 && c <= 0xE7) { if (o + 4 > len) return -1; k->a = u32at(b, o); o += 4; }
        else if (c == 0xC8 || c == 0xCC || c == 0xEA || c == 0xEB || c == 0xEE || c == 0xEF) {
            size_t l; if (o + 2 > len) return -1;
            l = u16at(b, o); o += 2 + l;
        }
        else return -1;
        if (n < max) n++;
    }
    return -1;
}

static const unsigned short cp1252[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
                                          0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
/* cp1252 -> ASCII-only HTML, escaping & < > : Apple Help 1.x (OS 9) understands neither UTF-8 nor entities such as &emsp; */
static int is_blank(const Sb *x) {                               /* only spaces or &nbsp; */
    size_t i = 0;
    while (i < x->n) {
        if (x->p[i] == ' ' || x->p[i] == '\t' || x->p[i] == '\r' || x->p[i] == '\n') i++;
        else if (x->n - i >= 6 && !strncmp(x->p + i, "&nbsp;", 6)) i += 6;
        else return 0;
    }
    return 1;
}

static int html_size(int hp) {                                   /* .hlp half-points -> <font size> */
    if (hp <= 20) return 2;
    if (hp <= 24) return 3;
    if (hp <= 28) return 4;
    if (hp <= 36) return 5;
    return 6;
}

static void esc(Sb *s, const uint8_t *t, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned c = t[i], u = c;
        if (c == '&') { sb_str(s, "&amp;"); continue; }
        if (c == '<') { sb_str(s, "&lt;"); continue; }
        if (c == '>') { sb_str(s, "&gt;"); continue; }
        if (c >= 0x80 && c < 0xA0) u = cp1252[c - 0x80];
        if (u == 0x2018 || u == 0x2019) { sb_str(s, "'"); continue; }
        if (u == 0x201C || u == 0x201D) { sb_str(s, "\""); continue; }
        if (u == 0x2013) { sb_str(s, "-"); continue; }
        if (u == 0x2014) { sb_str(s, "--"); continue; }
        if (u == 0x2026) { sb_str(s, "..."); continue; }
        if (u < 0x80) { char b = (char)u; sb_add(s, &b, 1); }
        else sb_fmt(s, "&#%u;", u);
    }
}

static int cmp_pair(const void *a, const void *b) { uint32_t x = ((const Pair *)a)->h, y = ((const Pair *)b)->h; return x < y ? -1 : x > y; }
static int cmp_rec(const void *a, const void *b) {
    const RecPos *x = (const RecPos *)a, *y = (const RecPos *)b;
    if (x->P != y->P) return x->P < y->P ? -1 : 1;
    if (x->ti != y->ti) return x->ti < y->ti ? -1 : 1;
    return x->ri < y->ri ? -1 : x->ri > y->ri;
}

static void render_rec(Sb *out, const Link *L, const Font *fn, int nf, const Pair *ctx, int nctx, const RecPos *rp, int nrp,
                       int *font, int *link, char *first, size_t fcap) {
    Cmd cmds[256];
    Rd r; int nc = -1;
    size_t s, lim;
    const uint8_t *d2 = L->d2, *cur;
    size_t left = L->n2;
    r.b.p = (uint8_t *)L->d1; r.b.n = L->n1; r.o = 0;
    rd_csl(&r); rd_cu(&r);
    lim = r.o + 16 < L->n1 ? r.o + 16 : L->n1;
    for (s = r.o; s < lim; s++) if ((nc = commands(L->d1, L->n1, s, cmds, 256)) >= 0) break;
    if (nc < 0) return;
    cur = d2;
    for (int ci = 0; ci <= nc; ci++) {
        int c = ci < nc ? cmds[ci].type : 0xFF;
        uint32_t a = ci < nc ? cmds[ci].a : 0;
        const uint8_t *t = cur; size_t tn = 0;
        if (cur <= d2 + left) {                                      /* next slice of d2 split by NUL */
            while (cur + tn < d2 + left && cur[tn]) tn++;
            cur += tn + 1;
        } else t = d2;
        if (tn) {
            Sb x = {0, 0, 0};
            const Font *fd = *font < nf ? &fn[*font] : NULL;
            esc(&x, t, tn);
            if (!first[0]) {
                size_t i = 0, e = x.n;
                while (i < e && (x.p[i] == ' ' || x.p[i] == '\t' || x.p[i] == '\r' || x.p[i] == '\n')) i++;
                while (e > i && (x.p[e - 1] == ' ' || x.p[e - 1] == '\t' || x.p[e - 1] == '\r' || x.p[e - 1] == '\n')) e--;
                if (e > i) { size_t l = e - i; if (l > fcap - 1) { l = fcap - 1; while (l && (x.p[i + l] & 0xC0) == 0x80) l--; } memcpy(first, x.p + i, l); first[l] = 0; }
            }
            /* The OS 9 Help Viewer ignores CSS: font, color and weight go in HTML 3.2 markup on every run.
               Viewers that understand CSS use the system font (font{} rule in css[]) and the exact size in pt. */
            if (is_blank(&x)) sb_add(out, x.p, x.n);                 /* blank run without markup: drop_empty_p recognizes it */
            else {
                int styled = fd && !*link;
                int hp = fd ? (fd->hp < 16 ? 16 : fd->hp) : 0;
                sb_fmt(out, "<font face=\"Geneva\" size=\"%d\"", html_size(hp));
                if (styled && fd->rgb) sb_fmt(out, " color=\"#%06x\"", fd->rgb);
                sb_str(out, ">");
                if (styled) {
                    if (fd->hp) sb_fmt(out, "<span style=\"font-size:%d%spt\">", hp / 2, (hp & 1) ? ".5" : "");
                    if (fd->attr & 1) sb_str(out, "<b>");
                    if (fd->attr & 2) sb_str(out, "<i>");
                    if (fd->attr & 4) sb_str(out, "<u>");
                }
                sb_add(out, x.p, x.n);
                if (styled) {
                    if (fd->attr & 4) sb_str(out, "</u>");
                    if (fd->attr & 2) sb_str(out, "</i>");
                    if (fd->attr & 1) sb_str(out, "</b>");
                    if (fd->hp) sb_str(out, "</span>");
                }
                sb_str(out, "</font>");
            }
            free(x.p);
        }
        if (c == 0x80) *font = (int)a;
        else if (c == 0x81) sb_str(out, "<br>");
        else if (c == 0x82) sb_str(out, "</p>\n<p>");
        else if (c == 0x83) sb_str(out, "&nbsp;&nbsp;&nbsp;&nbsp;");
        else if (c == 0x8B) sb_str(out, "&nbsp;");
        else if (c == 0x8C) sb_str(out, "-");
        else if (c >= 0xE0 && c <= 0xE7) {
            int ti = 0;
            Pair key, *f; key.h = a; key.v = 0;
            f = (Pair *)bsearch(&key, ctx, (size_t)nctx, sizeof *ctx, cmp_pair);
            if (f) {                                                 /* topic of position f->v (last record with P <= v) */
                int lo = 0, hi = nrp;
                while (lo < hi) { int m = (lo + hi) / 2; if (rp[m].P <= f->v) lo = m + 1; else hi = m; }
                ti = nrp ? rp[lo > 0 ? lo - 1 : 0].ti : 0;
            }
            sb_fmt(out, "<a href=\"topic%d.html#h%08x\"%s>", ti, (unsigned)a, (c == 0xE0 || c == 0xE2 || c == 0xE6) ? " class=\"popup\"" : "");
            *link = 1;
        }
        else if (c == 0x89 && *link) { sb_str(out, "</a>"); *link = 0; }
        else if (c >= 0x86 && c <= 0x88) sb_str(out, "<!-- image -->");
    }
}

/* strips empty "<p>" (only spaces or &nbsp;) like the original re.sub */
static void drop_empty_p(Sb *s) {
    size_t i = 0, o = 0;
    while (i < s->n) {
        if (!strncmp(s->p + i, "<p>", 3)) {
            size_t j = i + 3;
            for (;;) {
                if (s->p[j] == ' ' || s->p[j] == '\t' || s->p[j] == '\r' || s->p[j] == '\n') j++;
                else if (!strncmp(s->p + j, "&nbsp;", 6)) j += 6;
                else break;
            }
            if (!strncmp(s->p + j, "</p>", 4)) { j += 4; if (s->p[j] == '\n') j++; i = j; continue; }
        }
        s->p[o++] = s->p[i++];
    }
    s->n = o; s->p[o] = 0;
}

static const char *css =
    "body{font-family:-apple-system,\"Lucida Grande\",Helvetica,Arial,sans-serif;font-size:13px;margin:16px 20px;background:#fff;color:#000}"
    "a{color:#006400}a.popup{text-decoration:underline dotted}p{margin:0 0 6px}font{font-family:inherit;font-size:inherit}";

static int write_file(const char *path, const char *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(data, 1, n, f); fclose(f);
    return 1;
}

static int convert(Buf d, const char *outdir) {
    DirEnt de[256]; Buf sysf, topic, ctxf, omapf, fontf, phr, none;
    int nde = read_dir(d, de, 256), ok = 0;
    unsigned minor, flags;
    int comp, nblk = 0, nlinks = 0, ntop = 0, nrec = 0, nctx = 0, nomap = 0, nfont = 0, nanc = 0;
    size_t bs, tot = 0, pos, caplinks;
    size_t *starts = NULL, *cum = NULL;
    uint8_t *stream = NULL, **parts = NULL;
    size_t *plen = NULL;
    Link *links = NULL; Pair *ctx = NULL, *sctx = NULL, *omap = NULL; Font *fnt = NULL; RecPos *rp = NULL; Anchor *anc = NULL;
    int *tstart = NULL, *tcount = NULL, *recl = NULL;
    none.p = d.p; none.n = 0; phr = none;
    if (!findfile(d, de, nde, "|SYSTEM", &sysf) || !findfile(d, de, nde, "|TOPIC", &topic)) return 0;
    minor = u16at(sysf, 2); flags = u16at(sysf, 10);
    comp = minor > 16 && (flags == 4 || flags == 8);
    bs = minor > 16 ? 0x1000 : 0x800;
    findfile(d, de, nde, "|Phrases", &phr);
    /* |TOPIC: concatenated blocks (LZ77 per block) */
    nblk = (int)((topic.n + bs - 1) / bs);
    starts = (size_t *)calloc((size_t)nblk + 1, sizeof *starts); cum = (size_t *)calloc((size_t)nblk + 1, sizeof *cum);
    parts = (uint8_t **)calloc((size_t)nblk + 1, sizeof *parts); plen = (size_t *)calloc((size_t)nblk + 1, sizeof *plen);
    {
        int nb = 0;
        for (size_t o = 0; o < topic.n; o += bs) {
            size_t bl = topic.n - o < bs ? topic.n - o : bs;
            if (bl < 12) break;
            if (comp) parts[nb] = lz77(topic.p + o + 12, bl - 12, &plen[nb]);
            else { parts[nb] = (uint8_t *)malloc(bl); memcpy(parts[nb], topic.p + o + 12, bl - 12); plen[nb] = bl - 12; }
            starts[nb] = tot; tot += plen[nb]; nb++;
        }
        nblk = nb;
    }
    stream = (uint8_t *)malloc(tot + 32);
    for (int i = 0, o = 0; i < nblk; i++) { memcpy(stream + o, parts[i], plen[i]); o += (int)plen[i]; free(parts[i]); }
    free(parts); free(plen);
    /* record chain starting at 0xC */
    caplinks = tot / 21 + 2;
    links = (Link *)calloc(caplinks, sizeof *links);
    pos = 0xC;
    {
        Buf sb; sb.p = stream; sb.n = tot;
        while ((size_t)nlinks < caplinks) {
            size_t b = pos >> 14, i;
            int bsz, nxt, dl1;
            Link *k;
            size_t e1, e2;
            if ((int)b >= nblk) break;
            if ((pos & 0x3FFF) < 12) break;
            i = starts[b] + (pos & 0x3FFF) - 12;
            if (i + 21 > tot) break;
            bsz = (int)u32at(sb, i); nxt = (int)u32at(sb, i + 12); dl1 = (int)u32at(sb, i + 16);
            if (bsz < 21 || dl1 < 21) break;
            k = &links[nlinks++];
            e1 = i + (size_t)dl1; if (e1 > tot) e1 = tot;
            e2 = i + (size_t)bsz; if (e2 > tot) e2 = tot;
            if (e2 < e1) e2 = e1;
            k->type = stream[i + 20];
            k->d1 = stream + i + 21; k->n1 = e1 > i + 21 ? e1 - i - 21 : 0;
            k->d2 = expand(stream + e1, e2 - e1, phr, &k->n2);
            k->P = (uint32_t)((b << 15) + cum[b]);
            cum[b] += k->n2;
            if (nxt == -1 || nxt == 0) break;
            pos = (uint32_t)nxt;
        }
    }
    /* topics and positions */
    tstart = (int *)calloc((size_t)nlinks + 1, sizeof *tstart); tcount = (int *)calloc((size_t)nlinks + 1, sizeof *tcount);
    recl = (int *)calloc((size_t)nlinks + 1, sizeof *recl); rp = (RecPos *)calloc((size_t)nlinks + 1, sizeof *rp);
    for (int i = 0; i < nlinks; i++) {
        if (links[i].type == 2) { tstart[ntop] = nrec; tcount[ntop] = 0; ntop++; }
        else if ((links[i].type == 0x20 || links[i].type == 0x23) && ntop) {
            rp[nrec].P = links[i].P; rp[nrec].ti = ntop - 1; rp[nrec].ri = tcount[ntop - 1]++;
            recl[nrec] = i; nrec++;
        }
    }
    qsort(rp, (size_t)nrec, sizeof *rp, cmp_rec);
    /* |CONTEXT (B+tree L4), |CTXOMAP, |FONT */
    if (findfile(d, de, nde, "|CONTEXT", &ctxf) && ctxf.n > 38 && ctxf.p[6] == 'L') {
        size_t ps = u16at(ctxf, 4);
        int page = s16at(ctxf, 26), nlev = (int)u16at(ctxf, 32), guard = 0;
        ctx = (Pair *)calloc(u32at(ctxf, 34) + 1, sizeof *ctx);
        for (int i = 0; i < nlev - 1; i++) page = s16at(ctxf, 38 + (size_t)page * ps + 4);
        while (page != -1 && guard++ < 100000) {
            size_t p = 38 + (size_t)page * ps, o = p + 8;
            unsigned n = u16at(ctxf, p + 2);
            int nxt = s16at(ctxf, p + 6);
            for (unsigned i = 0; i < n && o + 8 <= ctxf.n && (uint32_t)nctx < u32at(ctxf, 34); i++, o += 8) { ctx[nctx].h = u32at(ctxf, o); ctx[nctx].v = u32at(ctxf, o + 4); nctx++; }
            page = nxt;
        }
        sctx = (Pair *)malloc(((size_t)nctx + 1) * sizeof *sctx); memcpy(sctx, ctx, (size_t)nctx * sizeof *sctx);
        qsort(sctx, (size_t)nctx, sizeof *sctx, cmp_pair);
    }
    if (findfile(d, de, nde, "|CTXOMAP", &omapf)) {
        nomap = (int)u16at(omapf, 0);
        if ((size_t)nomap * 8 + 2 > omapf.n) nomap = (int)((omapf.n - 2) / 8);
        omap = (Pair *)calloc((size_t)nomap + 1, sizeof *omap);
        for (int i = 0; i < nomap; i++) { omap[i].h = u32at(omapf, 2 + 8 * (size_t)i); omap[i].v = u32at(omapf, 6 + 8 * (size_t)i); }
    }
    if (findfile(d, de, nde, "|FONT", &fontf)) {
        unsigned nd = u16at(fontf, 2), dof = u16at(fontf, 6);
        fnt = (Font *)calloc((size_t)nd + 1, sizeof *fnt);
        for (unsigned i = 0; i < nd && dof + 11u * i + 8 <= fontf.n; i++) {
            size_t o = dof + 11u * i;
            fnt[nfont].attr = (int)u8at(fontf, o); fnt[nfont].hp = (int)u8at(fontf, o + 1);
            fnt[nfont].rgb = u8at(fontf, o + 5) << 16 | u8at(fontf, o + 6) << 8 | u8at(fontf, o + 7); nfont++;
        }
    }
    /* anchors: h<hash> (|CONTEXT) and ctx<N> (|CTXOMAP), in the original order */
    anc = (Anchor *)calloc((size_t)(nctx + nomap) + 1, sizeof *anc);
    for (int pass = 0; pass < 2; pass++) {
        int cnt = pass ? nomap : nctx;
        for (int i = 0; i < cnt; i++) {
            const Pair *pr = pass ? &omap[i] : &ctx[i];
            uint32_t v = pass ? pr->v : pr->v;
            int lo = 0, hi = nrec;
            while (lo < hi) { int m = (lo + hi) / 2; if (rp[m].P <= v) lo = m + 1; else hi = m; }
            if (nrec) { anc[nanc].ti = rp[lo > 0 ? lo - 1 : 0].ti; anc[nanc].ri = rp[lo > 0 ? lo - 1 : 0].ri; }
            anc[nanc].kind = pass; anc[nanc].val = pr->h; nanc++;
        }
    }
    /* pages */
    if (ntop > 0) {
        for (int t = 0; t < ntop; t++) {
            Sb pg = {0, 0, 0}, doc = {0, 0, 0};
            char first[256], name[1200], fn[64];
            int font = 0, link = 0;
            first[0] = 0;
            sb_str(&pg, "<p>");
            for (int ri = 0; ri < tcount[t]; ri++) {
                for (int a = 0; a < nanc; a++) if (anc[a].ti == t && anc[a].ri == ri) {
                    if (anc[a].kind) sb_fmt(&pg, "<a name=\"ctx%d\"></a>", (int)anc[a].val);
                    else sb_fmt(&pg, "<a name=\"h%08x\"></a>", (unsigned)anc[a].val);
                }
                render_rec(&pg, &links[recl[tstart[t] + ri]], fnt, nfont, sctx, nctx, rp, nrec, &font, &link, first, sizeof first);
            }
            if (link) sb_str(&pg, "</a>");
            sb_str(&pg, "</p>");
            drop_empty_p(&pg);
            sb_fmt(&doc, "<!DOCTYPE html>\n<html><head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\"><title>");
            if (first[0]) sb_str(&doc, first); else sb_fmt(&doc, "Topic %d", t + 1);
            sb_str(&doc, "</title><style><!--\n");                  /* sb_str: sb_fmt truncates at 256 bytes and would leave the comment open (blank page) */
            sb_str(&doc, css);
            sb_str(&doc, "\n--></style></head><body bgcolor=\"#ffffff\" text=\"#000000\" link=\"#006400\" vlink=\"#006400\" alink=\"#006400\">\n");
            sb_add(&doc, pg.p, pg.n);
            sb_str(&doc, "</body></html>\n");
            snprintf(fn, sizeof fn, "topic%d.html", t);
            path_join(name, sizeof name, outdir, fn);
            ok = write_file(name, doc.p, doc.n);
            if (t == 0) { path_join(name, sizeof name, outdir, "index.html"); ok &= write_file(name, doc.p, doc.n); }
            free(pg.p); free(doc.p);
            if (!ok) break;
        }
    }
    for (int i = 0; i < nlinks; i++) free(links[i].d2);
    free(links); free(stream); free(starts); free(cum); free(tstart); free(tcount); free(recl); free(rp); free(ctx); free(sctx); free(omap); free(fnt); free(anc);
    return ok;
}

static int find_hlp(const char *dir, char *out, size_t cap) {
    DIR *d = opendir(dir);
    struct dirent *e;
    int ok = 0;
    if (!d) return 0;
    while (!ok && (e = readdir(d))) if (!strcasecmp(e->d_name, "gpmhelp.hlp")) { path_join(out, cap, dir, e->d_name); ok = 1; }
    closedir(d);
    return ok;
}

int hlp_prepare(const char *game_dir, const char *wdir, char *index, size_t cap) {
    char hlp[1200], out[1200], stamp[1200], want[96], have[96];
    struct stat st;
    FILE *f;
    Buf d;
    int ok;
    if (!find_hlp(game_dir, hlp, sizeof hlp) || stat(hlp, &st) != 0) return 0;
    path_join(out, sizeof out, wdir, "Help");
    mkdir(out, 0755);
    path_join(stamp, sizeof stamp, out, "hlp.stamp");
    path_join(index, cap, out, "index.html");
    snprintf(want, sizeof want, "v7 %ld %ld", (long)st.st_size, (long)st.st_mtime);
    have[0] = 0;
    if ((f = fopen(stamp, "rb")) != NULL) { size_t n = fread(have, 1, sizeof have - 1, f); have[n] = 0; fclose(f); }
    if (!strcmp(have, want) && (f = fopen(index, "rb")) != NULL) { fclose(f); return 1; }
    f = fopen(hlp, "rb");
    if (!f) return 0;
    d.p = (uint8_t *)malloc((size_t)st.st_size + 16); d.n = fread(d.p, 1, (size_t)st.st_size, f); fclose(f);
    ok = d.n > 16 && u32at(d, 0) == 0x00035F3Fu && convert(d, out);
    free(d.p);
    if (ok) write_file(stamp, want, strlen(want));
    return ok;
}
