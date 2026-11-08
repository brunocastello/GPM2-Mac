/* DirectPlay engine: own protocol over TCP (session) + UDP (discovery).
 * Star topology: the session creator is the host and routes; one I/O thread per open session. */
#include "dplay_eng.h"
#include "../host/net.h"

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXC 8
#define MAXP 32
#define SESS_WIRE 124
enum { T_HELLO = 1, T_WELCOME, T_CREATEP, T_PNEW, T_DELP, T_PDEL, T_DATA, T_BYE };

typedef struct { DpePlayer p; int owner; uint8_t *data; uint32_t dlen; int used; } Pl;
typedef struct { int fd, joined; uint8_t buf[DPE_MAXMSG + 64]; int n; } Conn;
typedef struct Q { struct Q *next; DpeMsg m; } Q;

struct DpEng {
    pthread_mutex_t mu;
    pthread_t th;
    int threaded;                     /* 0: no I/O thread, see dpe_service_all */
    int running, mode;                 /* mode: 0 closed, 1 host, 2 client */
    DpeSess sess;
    unsigned short port;
    int lfd, ufd;
    Conn c[MAXC];
    Pl pl[MAXP];
    uint32_t next_id;
    Q *qh, *qt; uint32_t qn;
    volatile int welcome, pend_tag_done;
    uint32_t pend_tag, pend_id;
    int lost;                          /* 1 = dropped (notify), 2 = already notified */
};

static unsigned short net_port(void) {
    const char *e = getenv("GPM2_NET_PORT");
    return e && atoi(e) > 0 ? (unsigned short)atoi(e) : (unsigned short)DPE_PORT;
}
#ifdef __APPLE__                                   /* strnlen only exists from 10.7 (Tiger/Leopard do not have it) */
static size_t gpm_strnlen(const char *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
#define strnlen gpm_strnlen
#endif
static void p32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static uint32_t g32(const uint8_t *b) { return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }

static int sess_pack(const DpeSess *s, uint8_t *o) {
    memcpy(o, s->inst, 16); memcpy(o + 16, s->app, 16);
    p32(o + 32, s->flags); p32(o + 36, s->maxp); p32(o + 40, s->curp);
    for (int i = 0; i < 4; i++) p32(o + 44 + 4 * i, s->user[i]);
    memset(o + 60, 0, 64); memcpy(o + 60, s->name, strnlen(s->name, 63));
    return SESS_WIRE;
}
static void sess_unpack(DpeSess *s, const uint8_t *o) {
    memcpy(s->inst, o, 16); memcpy(s->app, o + 16, 16);
    s->flags = g32(o + 32); s->maxp = g32(o + 36); s->curp = g32(o + 40);
    for (int i = 0; i < 4; i++) s->user[i] = g32(o + 44 + 4 * i);
    memcpy(s->name, o + 60, 63); s->name[63] = 0;
}
static int send_frame(int fd, int type, const uint8_t *pay, int n) {
    uint8_t h[5];
    p32(h, (uint32_t)n + 1); h[4] = (uint8_t)type;
    if (hn_send(fd, h, 5) < 0) return -1;
    return n ? hn_send(fd, pay, n) : 0;
}

DpEng *dpe_new(void) {
    DpEng *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    pthread_mutex_init(&e->mu, NULL);
    e->lfd = e->ufd = -1;
    for (int i = 0; i < MAXC; i++) e->c[i].fd = -1;
    e->next_id = 0x1000u + (uint32_t)(clock() & 0xFFF);
    return e;
}

/* ---- queue ---------------------------------------------------------------------------------------- */
static void q_push(DpEng *e, const DpeMsg *m) {
    Q *q = malloc(sizeof *q);
    if (!q) return;
    q->next = NULL; q->m = *m;
    if (e->qt) e->qt->next = q; else e->qh = q;
    e->qt = q; e->qn++;
}
static uint32_t first_local(DpEng *e) {
    for (int i = 0; i < MAXP; i++) if (e->pl[i].used && e->pl[i].p.local) return e->pl[i].p.id;
    return 0;
}
static void q_sys(DpEng *e, int kind, uint32_t id, const char *name, uint32_t flags) {
    DpeMsg m;
    memset(&m, 0, sizeof m);
    m.kind = kind; m.to = first_local(e); m.flags = flags;
    if (name) snprintf(m.name, sizeof m.name, "%s", name);
    memcpy(m.data, &id, 4); m.len = 4;       /* player id: COM builds the DPMSG_* */
    q_push(e, &m);
}
static void q_free(DpEng *e) {
    while (e->qh) { Q *n = e->qh->next; free(e->qh); e->qh = n; }
    e->qt = NULL; e->qn = 0;
}

/* ---- players ---------------------------------------------------------------------------------------- */
static Pl *pl_find(DpEng *e, uint32_t id) {
    for (int i = 0; i < MAXP; i++) if (e->pl[i].used && e->pl[i].p.id == id) return &e->pl[i];
    return NULL;
}
static Pl *pl_add(DpEng *e, uint32_t id, uint32_t flags, const char *name, int local, int owner) {
    for (int i = 0; i < MAXP; i++) if (!e->pl[i].used) {
        Pl *p = &e->pl[i];
        memset(p, 0, sizeof *p);
        p->used = 1; p->owner = owner; p->p.id = id; p->p.flags = flags; p->p.local = local;
        snprintf(p->p.name, sizeof p->p.name, "%s", name);
        return p;
    }
    return NULL;
}
static void pl_del(Pl *p) { free(p->data); memset(p, 0, sizeof *p); }
static uint32_t count_pl(DpEng *e) { uint32_t n = 0; for (int i = 0; i < MAXP; i++) n += (uint32_t)e->pl[i].used; return n; }

static void bcast(DpEng *e, int type, const uint8_t *pay, int n, int except) {
    for (int i = 0; i < MAXC; i++)
        if (i != except && e->c[i].fd >= 0 && e->c[i].joined) send_frame(e->c[i].fd, type, pay, n);
}
static void pnew_pack(uint8_t *b, const Pl *p, uint32_t tag) {
    p32(b, p->p.id); p32(b + 4, p->p.flags); p32(b + 8, tag);
    memset(b + 12, 0, 48); memcpy(b + 12, p->p.name, strnlen(p->p.name, 47));
}

/* ---- data routing ------------------------------------------------------------------------------------ */
static void deliver_local(DpEng *e, uint32_t from, uint32_t to, uint32_t flags, const uint8_t *d, uint32_t n) {
    DpeMsg m;
    memset(&m, 0, sizeof m);
    m.kind = DPE_MSG_DATA; m.from = from; m.to = to; m.len = n; m.flags = flags;
    memcpy(m.data, d, n);
    q_push(e, &m);
}
/* host: routes (src_conn = -1 if it came from the host itself) */
static void host_route(DpEng *e, int src_conn, uint32_t from, uint32_t to, uint32_t flags, const uint8_t *d, uint32_t n) {
    uint8_t hdr[16 + DPE_MAXMSG];
    p32(hdr, from); p32(hdr + 4, to); p32(hdr + 8, flags); p32(hdr + 12, n); memcpy(hdr + 16, d, n);
    if (to == 0) {
        for (int i = 0; i < MAXP; i++)
            if (e->pl[i].used && e->pl[i].p.local && e->pl[i].p.id != from) deliver_local(e, from, e->pl[i].p.id, flags, d, n);
        for (int c = 0; c < MAXC; c++) {
            int has = 0;
            if (c == src_conn || e->c[c].fd < 0) continue;
            for (int i = 0; i < MAXP; i++) if (e->pl[i].used && e->pl[i].owner == c) has = 1;
            if (has) send_frame(e->c[c].fd, T_DATA, hdr, 16 + (int)n);
        }
    } else {
        Pl *p = pl_find(e, to);
        if (!p) return;
        if (p->p.local) deliver_local(e, from, to, flags, d, n);
        else if (p->owner >= 0 && e->c[p->owner].fd >= 0) send_frame(e->c[p->owner].fd, T_DATA, hdr, 16 + (int)n);
    }
}

static void drop_conn(DpEng *e, int ci) {
    if (e->c[ci].fd < 0) return;
    hn_close(e->c[ci].fd);
    e->c[ci].fd = -1; e->c[ci].joined = 0; e->c[ci].n = 0;
    for (int i = 0; i < MAXP; i++) {
        Pl *p = &e->pl[i];
        if (p->used && p->owner == ci) {
            uint8_t b[4];
            p32(b, p->p.id);
            bcast(e, T_PDEL, b, 4, ci);
            q_sys(e, DPE_MSG_DESTROY, p->p.id, p->p.name, p->p.flags);
            pl_del(p);
        }
    }
}

static void host_frame(DpEng *e, int ci, int type, const uint8_t *b, int n) {
    Conn *c = &e->c[ci];
    if (type == T_HELLO && n >= 16) {
        uint8_t w[1 + SESS_WIRE + 4 + MAXP * 56];
        int o = 0, np = 0;
        if (memcmp(b, e->sess.inst, 16) != 0 || (e->sess.flags & 1u)) {       /* another session / DPSESSION_NEWPLAYERSDISABLED */
            w[0] = 0; send_frame(c->fd, T_WELCOME, w, 1); return;
        }
        e->sess.curp = count_pl(e);
        w[o++] = 1; o += sess_pack(&e->sess, w + o);
        for (int i = 0; i < MAXP; i++) if (e->pl[i].used) np++;
        p32(w + o, (uint32_t)np); o += 4;
        for (int i = 0; i < MAXP; i++) if (e->pl[i].used) {
            p32(w + o, e->pl[i].p.id); p32(w + o + 4, e->pl[i].p.flags);
            memset(w + o + 8, 0, 48); memcpy(w + o + 8, e->pl[i].p.name, strnlen(e->pl[i].p.name, 47)); o += 56;
        }
        send_frame(c->fd, T_WELCOME, w, o);
        c->joined = 1;
    } else if (!c->joined) {
        return;
    } else if (type == T_CREATEP && n >= 56) {
        uint8_t r[60];
        char nm[48];
        uint32_t tag = g32(b), fl = g32(b + 4);
        Pl *p = NULL;
        memcpy(nm, b + 8, 47); nm[47] = 0;
        if (count_pl(e) < (e->sess.maxp ? e->sess.maxp : MAXP)) p = pl_add(e, e->next_id++, fl, nm, 0, ci);
        if (!p) { memset(r, 0, 60); p32(r + 8, tag); send_frame(c->fd, T_PNEW, r, 60); return; }
        pnew_pack(r, p, 0);
        bcast(e, T_PNEW, r, 60, ci);                                       /* the others: no tag */
        p32(r + 8, tag); send_frame(c->fd, T_PNEW, r, 60);                 /* the owner: with the tag */
        q_sys(e, DPE_MSG_CREATE, p->p.id, nm, fl);
    } else if (type == T_DELP && n >= 4) {
        Pl *p = pl_find(e, g32(b));
        if (p && p->owner == ci) {
            q_sys(e, DPE_MSG_DESTROY, p->p.id, p->p.name, p->p.flags);
            bcast(e, T_PDEL, b, 4, -1);
            pl_del(p);
        }
    } else if (type == T_DATA && n >= 16 && g32(b + 12) <= DPE_MAXMSG && (int)g32(b + 12) + 16 <= n) {
        Pl *f = pl_find(e, g32(b));
        if (f && f->owner == ci) host_route(e, ci, g32(b), g32(b + 4), g32(b + 8), b + 16, g32(b + 12));
    } else if (type == T_BYE) {
        drop_conn(e, ci);
    }
}

static void client_frame(DpEng *e, int type, const uint8_t *b, int n) {
    if (type == T_WELCOME && n >= 1) {
        if (b[0] && n >= 1 + SESS_WIRE + 4) {
            uint32_t np = g32(b + 1 + SESS_WIRE);
            DpeSess s;
            memset(&s, 0, sizeof s);
            sess_unpack(&s, b + 1);
            memcpy(s.ip, e->sess.ip, 16); s.port = e->sess.port; e->sess = s;
            for (uint32_t i = 0; i < np && i < MAXP && (int)(1 + SESS_WIRE + 4 + 56 * (i + 1)) <= n; i++) {
                const uint8_t *r = b + 1 + SESS_WIRE + 4 + 56 * i;
                char nm[48];
                memcpy(nm, r + 8, 47); nm[47] = 0;
                pl_add(e, g32(r), g32(r + 4), nm, 0, 0);
            }
            e->welcome = 1;
        } else e->welcome = -1;
    } else if (type == T_PNEW && n >= 60) {
        uint32_t id = g32(b), tag = g32(b + 8);
        char nm[48];
        memcpy(nm, b + 12, 47); nm[47] = 0;
        if (tag && tag == e->pend_tag) {
            if (id) { pl_add(e, id, g32(b + 4), nm, 1, -1); e->pend_id = id; }
            e->pend_tag_done = 1;
        } else if (id && !pl_find(e, id)) {
            pl_add(e, id, g32(b + 4), nm, 0, 0);
            q_sys(e, DPE_MSG_CREATE, id, nm, g32(b + 4));
        }
    } else if (type == T_PDEL && n >= 4) {
        Pl *p = pl_find(e, g32(b));
        if (p) { if (!p->p.local) q_sys(e, DPE_MSG_DESTROY, p->p.id, p->p.name, p->p.flags); pl_del(p); }
    } else if (type == T_DATA && n >= 16 && g32(b + 12) <= DPE_MAXMSG && (int)g32(b + 12) + 16 <= n) {
        uint32_t from = g32(b), to = g32(b + 4), fl = g32(b + 8), len = g32(b + 12);
        if (to) { if (pl_find(e, to)) deliver_local(e, from, to, fl, b + 16, len); }
        else for (int i = 0; i < MAXP; i++)
            if (e->pl[i].used && e->pl[i].p.local && e->pl[i].p.id != from) deliver_local(e, from, e->pl[i].p.id, fl, b + 16, len);
    } else if (type == T_BYE) {
        e->lost = e->lost ? e->lost : 1;
    }
}

static void conn_fail(DpEng *e, int ci) {
    if (e->mode == 1) { drop_conn(e, ci); return; }
    hn_close(e->c[ci].fd); e->c[ci].fd = -1;
    e->lost = e->lost ? e->lost : 1;
}

static void conn_read(DpEng *e, int ci) {
    Conn *c = &e->c[ci];
    for (;;) {
        int r = hn_recv(c->fd, c->buf + c->n, (int)sizeof c->buf - c->n);
        if (r < 0) { conn_fail(e, ci); return; }
        if (r == 0) break;
        c->n += r;
        while (c->n >= 5) {
            uint32_t len = g32(c->buf);
            if (len < 1 || len > sizeof c->buf - 4) { conn_fail(e, ci); return; }
            if ((uint32_t)c->n < len + 4) break;
            if (e->mode == 1) host_frame(e, ci, c->buf[4], c->buf + 5, (int)len - 1);
            else client_frame(e, c->buf[4], c->buf + 5, (int)len - 1);
            if (c->fd < 0) return;
            memmove(c->buf, c->buf + len + 4, (size_t)c->n - len - 4); c->n -= (int)(len + 4);
        }
        if (c->n >= (int)sizeof c->buf) break;
    }
}

static void pump(DpEng *e) {
    if (e->mode == 1) {
        int fd;
        while ((fd = hn_accept(e->lfd)) >= 0) {
            int ci = -1;
            for (int i = 0; i < MAXC; i++) if (e->c[i].fd < 0) { ci = i; break; }
            if (ci < 0) { hn_close(fd); continue; }
            e->c[ci].fd = fd; e->c[ci].n = 0; e->c[ci].joined = 0;
        }
        if (e->ufd >= 0) {
            uint8_t q[16], r[4 + SESS_WIRE + 2];
            char ip[16]; unsigned short pt;
            while (hn_udp_recv(e->ufd, q, sizeof q, ip, &pt) > 0) {
                if (memcmp(q, "GPMQ", 4) != 0) continue;
                e->sess.curp = count_pl(e);
                memcpy(r, "GPMR", 4); sess_pack(&e->sess, r + 4); r[4 + SESS_WIRE] = (uint8_t)e->port; r[5 + SESS_WIRE] = (uint8_t)(e->port >> 8);
                hn_udp_send(e->ufd, ip, pt, r, sizeof r);
            }
        }
    }
    for (int i = 0; i < MAXC; i++) if (e->c[i].fd >= 0) conn_read(e, i);
}

static void *io_thread(void *a) {
    DpEng *e = a;
    for (;;) {
        pthread_mutex_lock(&e->mu);
        if (!e->running) { pthread_mutex_unlock(&e->mu); break; }
        pump(e);
        pthread_mutex_unlock(&e->mu);
        usleep(4000);
    }
    return NULL;
}
/* No threads (Mac OS 9): the host calls dpe_service_all in its event pump */
#ifndef DPE_THREAD_CREATE
#define DPE_THREAD_CREATE pthread_create
#endif
enum { MAX_NOTHREAD = 8 };
static DpEng *nothread[MAX_NOTHREAD];
static void start_io(DpEng *e) {
    e->running = 1;
    if (DPE_THREAD_CREATE(&e->th, NULL, io_thread, e) == 0) { e->threaded = 1; return; }
    e->threaded = 0;
    for (int i = 0; i < MAX_NOTHREAD; i++) if (!nothread[i]) { nothread[i] = e; break; }
}
void dpe_service_all(void) {
    for (int i = 0; i < MAX_NOTHREAD; i++) {
        DpEng *e = nothread[i];
        if (!e || pthread_mutex_trylock(&e->mu) != 0) continue;  /* a call interrupted midway finishes first */
        if (e->running) pump(e);
        pthread_mutex_unlock(&e->mu);
    }
}

/* waits for `*flag` (under the mutex) to become != 0, up to timeout_ms; call with the mutex held */
static int wait_flag(DpEng *e, volatile int *flag, int timeout_ms) {
    for (int t = 0; t < timeout_ms && !*flag; t += 5) {
        pthread_mutex_unlock(&e->mu); usleep(5000); pthread_mutex_lock(&e->mu);
    }
    return *flag;
}

/* ---- sessions ------------------------------------------------------------------------------------------------- */
static void split_addr(const char *a, char *ip, unsigned short *port) {
    const char *c = strchr(a, ':');
    size_t n = c ? (size_t)(c - a) : strlen(a);
    if (n > 15) n = 15;
    memcpy(ip, a, n); ip[n] = 0;
    *port = c && atoi(c + 1) > 0 ? (unsigned short)atoi(c + 1) : net_port();
}

int dpe_enum_sessions(DpEng *e, const char *addr, int timeout_ms, DpeSess *out, int max) {
    int fd = hn_udp_open(0, 0, 1), n = 0;
    (void)e;
    if (fd < 0) return 0;
    hn_udp_send(fd, NULL, net_port(), "GPMQ", 4);
    if (addr && addr[0]) {
        char ip[16]; unsigned short pt;
        split_addr(addr, ip, &pt);
        hn_udp_send(fd, ip, pt, "GPMQ", 4);
    }
    for (int t = 0; t < timeout_ms; t += 10) {
        uint8_t r[SESS_WIRE + 16];
        char ip[16]; unsigned short pt;
        int k;
        while ((k = hn_udp_recv(fd, r, sizeof r, ip, &pt)) > 0) {
            DpeSess s;
            int dup = 0;
            if (k < 4 + SESS_WIRE + 2 || memcmp(r, "GPMR", 4) != 0) continue;
            memset(&s, 0, sizeof s);
            sess_unpack(&s, r + 4);
            memcpy(s.ip, ip, 16); s.port = (unsigned short)(r[4 + SESS_WIRE] | r[5 + SESS_WIRE] << 8);
            for (int i = 0; i < n; i++) if (!memcmp(out[i].inst, s.inst, 16)) { out[i] = s; dup = 1; }
            if (!dup && n < max) out[n++] = s;
        }
        usleep(10000);
    }
    hn_close(fd);
    return n;
}

static void rand_guid(uint8_t *g) {
    static unsigned seed;
    if (!seed) seed = (unsigned)time(NULL) ^ ((unsigned)getpid() << 8) ^ (unsigned)clock();
    for (int i = 0; i < 16; i++) { seed = seed * 1103515245u + 12345u; g[i] = (uint8_t)(seed >> 16); }
}

uint32_t dpe_open_create(DpEng *e, const DpeSess *s) {
    uint32_t r = DPE_CANTCREATESESSION;
    pthread_mutex_lock(&e->mu);
    if (e->mode == 0) {
        e->port = net_port();
        e->lfd = hn_tcp_listen(e->port);
        if (e->lfd >= 0) {
            e->ufd = hn_udp_open(e->port, 1, 0);     /* no discovery if the UDP port is busy: manual join still works */
            e->sess = *s; rand_guid(e->sess.inst); e->sess.curp = 0;
            e->mode = 1; e->lost = 0;
            start_io(e);
            r = DPE_OK;
        }
    }
    pthread_mutex_unlock(&e->mu);
    return r;
}

uint32_t dpe_open_join(DpEng *e, const DpeSess *s) {
    uint32_t r;
    int fd;
    if (e->mode != 0) return DPE_GENERIC;
    fd = hn_tcp_connect(s->ip, s->port, 3000);
    if (fd < 0) return DPE_NOCONNECTION;
    pthread_mutex_lock(&e->mu);
    e->c[0].fd = fd; e->c[0].n = 0; e->c[0].joined = 1;
    e->sess = *s; e->mode = 2; e->welcome = 0; e->lost = 0;
    start_io(e);
    send_frame(fd, T_HELLO, s->inst, 16);
    if (!wait_flag(e, &e->welcome, 5000)) r = DPE_TIMEOUT;
    else r = e->welcome > 0 ? DPE_OK : DPE_ACCESSDENIED;
    pthread_mutex_unlock(&e->mu);
    if (r != DPE_OK) dpe_close(e);
    return r;
}

void dpe_close(DpEng *e) {
    int run;
    pthread_mutex_lock(&e->mu);
    run = e->running;
    for (int i = 0; i < MAXC; i++) if (e->c[i].fd >= 0) { send_frame(e->c[i].fd, T_BYE, NULL, 0); hn_close(e->c[i].fd); e->c[i].fd = -1; }
    e->running = 0;
    pthread_mutex_unlock(&e->mu);
    if (run && e->threaded) pthread_join(e->th, NULL);
    for (int i = 0; i < MAX_NOTHREAD; i++) if (nothread[i] == e) nothread[i] = NULL;
    pthread_mutex_lock(&e->mu);
    hn_close(e->lfd); hn_close(e->ufd); e->lfd = e->ufd = -1;
    for (int i = 0; i < MAXP; i++) if (e->pl[i].used) pl_del(&e->pl[i]);
    q_free(e);
    e->mode = 0; e->welcome = 0; e->lost = 0;
    pthread_mutex_unlock(&e->mu);
}
void dpe_free(DpEng *e) { if (!e) return; dpe_close(e); pthread_mutex_destroy(&e->mu); free(e); }
int dpe_is_open(DpEng *e) { return e->mode != 0; }

/* ---- players / messages ----------------------------------------------------------------------------------------- */
uint32_t dpe_create_player(DpEng *e, const char *name, uint32_t flags, uint32_t *id) {
    uint32_t r = DPE_CANTCREATEPLAYER;
    pthread_mutex_lock(&e->mu);
    if (e->mode == 1) {
        if (count_pl(e) < (e->sess.maxp ? e->sess.maxp : MAXP)) {
            Pl *p = pl_add(e, e->next_id++, flags, name, 1, -1);
            if (p) {
                uint8_t b[60];
                pnew_pack(b, p, 0);
                bcast(e, T_PNEW, b, 60, -1);
                *id = p->p.id; r = DPE_OK;
            }
        }
    } else if (e->mode == 2 && !e->lost) {
        uint8_t b[56];
        e->pend_tag = (e->pend_tag + 2) | 1u; e->pend_tag_done = 0; e->pend_id = 0;
        p32(b, e->pend_tag); p32(b + 4, flags); memset(b + 8, 0, 48); memcpy(b + 8, name, strnlen(name, 47));
        send_frame(e->c[0].fd, T_CREATEP, b, 56);
        if (!wait_flag(e, &e->pend_tag_done, 5000)) r = DPE_TIMEOUT;
        else if (e->pend_id) { *id = e->pend_id; r = DPE_OK; }
    }
    pthread_mutex_unlock(&e->mu);
    return r;
}
uint32_t dpe_destroy_player(DpEng *e, uint32_t id) {
    uint32_t r = DPE_INVALIDPLAYER;
    pthread_mutex_lock(&e->mu);
    Pl *p = pl_find(e, id);
    if (p && p->p.local) {
        uint8_t b[4];
        p32(b, id);
        if (e->mode == 1) bcast(e, T_PDEL, b, 4, -1);
        else if (e->c[0].fd >= 0) send_frame(e->c[0].fd, T_DELP, b, 4);
        pl_del(p); r = DPE_OK;
    }
    pthread_mutex_unlock(&e->mu);
    return r;
}
int dpe_enum_players(DpEng *e, DpePlayer *out, int max) {
    int n = 0;
    pthread_mutex_lock(&e->mu);
    for (int i = 0; i < MAXP && n < max; i++) if (e->pl[i].used) out[n++] = e->pl[i].p;
    pthread_mutex_unlock(&e->mu);
    return n;
}
int dpe_player(DpEng *e, uint32_t id, DpePlayer *out) {
    int ok = 0;
    pthread_mutex_lock(&e->mu);
    Pl *p = pl_find(e, id);
    if (p) { *out = p->p; ok = 1; }
    pthread_mutex_unlock(&e->mu);
    return ok;
}
void dpe_session(DpEng *e, DpeSess *out) {
    pthread_mutex_lock(&e->mu);
    *out = e->sess; out->curp = count_pl(e);
    pthread_mutex_unlock(&e->mu);
}
uint32_t dpe_send(DpEng *e, uint32_t from, uint32_t to, uint32_t flags, const void *buf, uint32_t len) {
    uint32_t r = DPE_INVALIDPLAYER;
    if (len > DPE_MAXMSG) return DPE_SENDTOOBIG;
    pthread_mutex_lock(&e->mu);
    Pl *f = pl_find(e, from);
    if (e->mode == 0) r = DPE_GENERIC;
    else if (e->mode == 2 && e->lost) r = DPE_NOCONNECTION;
    else if (f && f->p.local && (to == 0 || pl_find(e, to))) {
        if (e->mode == 1) host_route(e, -1, from, to, flags, buf, len);
        else {
            uint8_t b[16 + DPE_MAXMSG];
            p32(b, from); p32(b + 4, to); p32(b + 8, flags); p32(b + 12, len); memcpy(b + 16, buf, len);
            send_frame(e->c[0].fd, T_DATA, b, 16 + (int)len);
        }
        r = DPE_OK;
    }
    pthread_mutex_unlock(&e->mu);
    return r;
}
uint32_t dpe_receive(DpEng *e, uint32_t from_filter, uint32_t to_filter, int peek, DpeMsg *m) {
    uint32_t r = DPE_NOMESSAGES;
    pthread_mutex_lock(&e->mu);
    if (e->lost == 1) {                           /* the session dropped: notifies once; Send returns an error until Close */
        DpeMsg l;
        memset(&l, 0, sizeof l);
        l.kind = DPE_MSG_LOST; l.to = first_local(e);
        q_push(e, &l);
        e->lost = 2;
    }
    for (Q *q = e->qh, *prev = NULL; q; prev = q, q = q->next) {
        if (q->m.kind == DPE_MSG_DATA && ((from_filter && q->m.from != from_filter) || (to_filter && q->m.to != to_filter))) continue;
        *m = q->m;
        if (!peek) {
            if (prev) prev->next = q->next; else e->qh = q->next;
            if (e->qt == q) e->qt = prev;
            e->qn--; free(q);
        }
        r = DPE_OK;
        break;
    }
    pthread_mutex_unlock(&e->mu);
    return r;
}
uint32_t dpe_msg_count(DpEng *e) { uint32_t n; pthread_mutex_lock(&e->mu); n = e->qn; pthread_mutex_unlock(&e->mu); return n; }
void dpe_set_data(DpEng *e, uint32_t id, const void *d, uint32_t n) {
    pthread_mutex_lock(&e->mu);
    Pl *p = pl_find(e, id);
    if (p && p->p.local) { free(p->data); p->data = n ? malloc(n) : NULL; if (p->data) memcpy(p->data, d, n); p->dlen = p->data ? n : 0; }
    pthread_mutex_unlock(&e->mu);
}
uint32_t dpe_get_data(DpEng *e, uint32_t id, void *d, uint32_t cap) {
    uint32_t n = 0;
    pthread_mutex_lock(&e->mu);
    Pl *p = pl_find(e, id);
    if (p) { n = p->dlen; if (d && cap >= n && n) memcpy(d, p->data, n); }
    pthread_mutex_unlock(&e->mu);
    return n;
}
