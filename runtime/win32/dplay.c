/* DPLAYX: DirectPlayCreate/DirectPlayEnumerate + IDirectPlay2A with a vtable in guest memory and host methods.
 * A single provider, "TCP/IP (Internet)". The protocol and networking live in dplay_eng.c; here only the guest <-> engine conversion. */
#include "win32.h"
#include "dplay_eng.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DPERR(c) (0x88770000u + (c))
#define E_NOTIMPL_ 0x80004001u
#define E_NOINTERFACE_ 0x80004002u
#define MAXOBJ 8

static const uint8_t TCP_GUID[16] = { 0xE0, 0x5E, 0xE9, 0x36, 0x77, 0x85, 0xCF, 0x11, 0x96, 0x0C, 0x00, 0x80, 0xC7, 0x53, 0x4E, 0x82 };
static const char TCP_NAME[] = "TCP/IP (Internet)";

static struct { DpEng *e; uint32_t obj; int asked, cancelled; char addr[64]; DpeSess found[16]; int nfound; } objs[MAXOBJ];
static uint32_t vtbl;

static int obj_of(uint32_t this) {
    int i = (int)R32(this + 8);
    if (i < 0 || i >= MAXOBJ || objs[i].obj != this) win32_die("DirectPlay: invalid object 0x%08x", this);
    return i;
}
#define ENG(i) (objs[i].e)

/* ---- IUnknown ----------------------------------------------------------------------------------------------------- */
static uint32_t dp_QueryInterface_impl(void) {
    uint32_t this = ARG(0), iid = ARG(1), out = ARG(2);
    uint8_t g8[16];
    static const uint8_t iunk[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    gmem_get(g8, iid, 16);
    (void)obj_of(this);
    if (memcmp(g8 + 6, TCP_GUID + 6, 10) == 0 || memcmp(g8, iunk, 16) == 0) {      /* IDirectPlay* family (same suffix) or IUnknown */
        W32(out, this); W32(this + 4, R32(this + 4) + 1);
        return 0;
    }
    W32(out, 0);
    return E_NOINTERFACE_;
}
static uint32_t dp_AddRef_impl(void) { uint32_t t = ARG(0), n = R32(t + 4) + 1; (void)obj_of(t); W32(t + 4, n); return n; }
static uint32_t dp_Release_impl(void) {
    uint32_t t = ARG(0), n;
    int i = obj_of(t);
    n = R32(t + 4) - 1; W32(t + 4, n);
    if (n == 0) { dpe_free(objs[i].e); objs[i].e = NULL; objs[i].obj = 0; heap_free(t); }
    return n;
}

/* ---- helpers ---------------------------------------------------------------------------------------------------------- */
static void name_out(uint32_t base, uint32_t strp, const char *s) {       /* ANSI DPNAME at `base`; the string at `strp` */
    W32(base, 16); W32(base + 4, 0); W32(base + 8, strp); W32(base + 12, strp);
    gstr_put(strp, s);
}
static void sess_out(uint32_t d, uint32_t strp, const DpeSess *s) {       /* DPSESSIONDESC2 (80 bytes) */
    gmem_put(d, "\0\0\0\0", 0);
    W32(d, 80); W32(d + 4, s->flags);
    gmem_put(d + 8, s->inst, 16); gmem_put(d + 24, s->app, 16);
    W32(d + 40, s->maxp); W32(d + 44, s->curp);
    W32(d + 48, strp); W32(d + 52, 0); W32(d + 56, 0); W32(d + 60, 0);
    for (int i = 0; i < 4; i++) W32(d + 64 + 4 * i, s->user[i]);
    gstr_put(strp, s->name);
}

static uint32_t dp_Close_impl(void) { int i = obj_of(ARG(0)); dpe_close(ENG(i)); win32_log("DirectPlay: Close"); return 0; }

static uint32_t dp_CreatePlayer_impl(void) {
    int i = obj_of(ARG(0));
    uint32_t pid = ARG(1), nm = ARG(2), data = ARG(4), dsz = ARG(5), id = 0, r;
    char name[48] = "";
    if (nm && R32(nm + 8)) gstr_get(name, sizeof name, R32(nm + 8));
    r = dpe_create_player(ENG(i), name, ARG(6), &id);
    if (r == DPE_OK) {
        W32(pid, id);
        if (data && dsz && dsz <= 4096) { uint8_t b[4096]; gmem_get(b, data, dsz); dpe_set_data(ENG(i), id, b, dsz); }
    }
    win32_log("DirectPlay: CreatePlayer('%s') -> id %u, 0x%08x", name, id, r);
    return r;
}
static uint32_t dp_DestroyPlayer_impl(void) { int i = obj_of(ARG(0)); return dpe_destroy_player(ENG(i), ARG(1)); }

static uint32_t dp_EnumPlayers_impl(void) {
    int i = obj_of(ARG(0)), n;
    uint32_t cb = ARG(2), ctx = ARG(3), fl = ARG(4), buf;
    DpePlayer pl[32];
    n = dpe_enum_players(ENG(i), pl, 32);
    buf = heap_alloc(16 + 48);
    for (int k = 0; k < n; k++) {
        if ((fl & 8u) && !pl[k].local) continue;                                 /* DPENUMPLAYERS_LOCAL */
        if ((fl & 16u) && pl[k].local) continue;                                 /* DPENUMPLAYERS_REMOTE */
        name_out(buf, buf + 16, pl[k].name);
        if (!guest_call(cb, 5, pl[k].id, 1u, buf, pl[k].flags | (pl[k].local ? 0u : 0u), ctx)) break;
    }
    heap_free(buf);
    return 0;
}

static uint32_t dp_EnumSessions_impl(void) {
    int i = obj_of(ARG(0)), n;
    uint32_t desc = ARG(1), tmo = ARG(2), cb = ARG(3), ctx = ARG(4), fl = ARG(5), d, tp;
    DpeSess s[16];
    uint8_t want[16], zero[16] = { 0 };
    if (!objs[i].asked) {
        objs[i].asked = 1;
        objs[i].cancelled = !host_net_ask_address(objs[i].addr, sizeof objs[i].addr);
    }
    if (objs[i].cancelled) { objs[i].asked = 0; return DPERR(280); }              /* DPERR_USERCANCEL: asks again next time */
    if (desc) gmem_get(want, desc + 24, 16); else memset(want, 0, 16);
    n = dpe_enum_sessions(ENG(i), objs[i].addr, tmo && tmo < 2000 ? (int)tmo : 800, s, 16);
    win32_log("DirectPlay: EnumSessions(addr '%s') -> %d session(s)", objs[i].addr, n);
    objs[i].nfound = 0;
    d = heap_alloc(80 + 64); tp = heap_alloc(4);
    for (int k = 0; k < n; k++) {
        if (memcmp(want, zero, 16) && memcmp(want, s[k].app, 16)) continue;
        objs[i].found[objs[i].nfound++] = s[k];
        sess_out(d, d + 80, &s[k]);
        W32(tp, tmo);
        if (!guest_call(cb, 4, d, tp, 0u, ctx)) goto done;
    }
    W32(tp, tmo);
    guest_call(cb, 4, 0u, tp, 1u, ctx);                                           /* DPESC_TIMEDOUT: end of the list */
done:
    heap_free(d); heap_free(tp);
    (void)fl;
    return 0;
}

static uint32_t dp_GetCaps_impl(void) {
    uint32_t c = ARG(1);
    obj_of(ARG(0));
    W32(c, 40); W32(c + 4, 0); W32(c + 8, 16384); W32(c + 12, 0); W32(c + 16, 65536); W32(c + 20, 0);
    W32(c + 24, 50); W32(c + 28, 65536); W32(c + 32, 40); W32(c + 36, 500);
    return 0;
}
static uint32_t dp_GetMessageCount_impl(void) { int i = obj_of(ARG(0)); W32(ARG(2), dpe_msg_count(ENG(i))); return 0; }

static uint32_t dp_GetPlayerData_impl(void) {
    int i = obj_of(ARG(0));
    uint32_t id = ARG(1), buf = ARG(2), szp = ARG(3), n;
    DpePlayer p;
    uint8_t b[4096];
    if (!dpe_player(ENG(i), id, &p)) return DPERR(150);
    n = dpe_get_data(ENG(i), id, NULL, 0);
    if (!buf || R32(szp) < n) { W32(szp, n); return n ? DPERR(30) : 0; }
    if (n <= sizeof b) { dpe_get_data(ENG(i), id, b, n); gmem_put(buf, b, n); }
    W32(szp, n);
    return 0;
}
static uint32_t dp_SetPlayerData_impl(void) {
    int i = obj_of(ARG(0));
    uint32_t n = ARG(3);
    uint8_t b[4096];
    DpePlayer p;
    if (!dpe_player(ENG(i), ARG(1), &p)) return DPERR(150);
    if (n > sizeof b) return DPERR(230);
    if (n) gmem_get(b, ARG(2), n);
    dpe_set_data(ENG(i), ARG(1), b, n);
    return 0;
}
static uint32_t dp_GetPlayerName_impl(void) {
    int i = obj_of(ARG(0));
    uint32_t buf = ARG(2), szp = ARG(3), need;
    DpePlayer p;
    if (!dpe_player(ENG(i), ARG(1), &p)) return DPERR(150);
    need = 16 + (uint32_t)strlen(p.name) + 1;
    if (!buf || R32(szp) < need) { W32(szp, need); return DPERR(30); }
    name_out(buf, buf + 16, p.name);
    W32(szp, need);
    return 0;
}
static uint32_t dp_GetSessionDesc_impl(void) {
    int i = obj_of(ARG(0));
    uint32_t buf = ARG(1), szp = ARG(2), need;
    DpeSess s;
    if (!dpe_is_open(ENG(i))) return DPERR(170);
    dpe_session(ENG(i), &s);
    need = 80 + (uint32_t)strlen(s.name) + 1;
    if (!buf || R32(szp) < need) { W32(szp, need); return DPERR(30); }
    sess_out(buf, buf + 80, &s);
    W32(szp, need);
    return 0;
}

static uint32_t dp_Open_impl(void) {
    int i = obj_of(ARG(0));
    uint32_t d = ARG(1), fl = ARG(2), r;
    DpeSess s;
    memset(&s, 0, sizeof s);
    if (fl & 2u) {                                                                  /* DPOPEN_CREATE (JOIN = 1) */
        char nm[64] = "";
        s.flags = R32(d + 4); s.maxp = R32(d + 40);
        gmem_get(s.app, d + 24, 16);
        if (R32(d + 48)) gstr_get(nm, sizeof nm, R32(d + 48));
        memcpy(s.name, nm, sizeof s.name);
        for (int k = 0; k < 4; k++) s.user[k] = R32(d + 64 + 4 * k);
        r = dpe_open_create(ENG(i), &s);
        win32_log("DirectPlay: Open(CREATE '%s', max %u) -> 0x%08x", s.name, s.maxp, r);
    } else {                                                                        /* DPOPEN_JOIN */
        uint8_t inst[16];
        int k;
        gmem_get(inst, d + 8, 16);
        for (k = 0; k < objs[i].nfound; k++) if (!memcmp(objs[i].found[k].inst, inst, 16)) break;
        if (k == objs[i].nfound) { win32_log("DirectPlay: Open(JOIN) of unknown session"); return DPERR(220); }
        r = dpe_open_join(ENG(i), &objs[i].found[k]);
        win32_log("DirectPlay: Open(JOIN '%s' at %s:%u) -> 0x%08x", objs[i].found[k].name, objs[i].found[k].ip, objs[i].found[k].port, r);
    }
    return r;
}

static uint32_t dp_Receive_impl(void) {
    int i = obj_of(ARG(0));
    uint32_t fromp = ARG(1), top = ARG(2), fl = ARG(3), buf = ARG(4), szp = ARG(5), need, bp = 0, bn = 0, r;
    static DpeMsg m;                                                                /* only the guest thread gets here */
    uint32_t ff = (fl & 4u) ? R32(fromp) : 0, tf = (fl & 2u) ? R32(top) : 0;
    r = dpe_receive(ENG(i), ff, tf, 1, &m);
    if (r != DPE_OK) return r;
    if (m.kind == DPE_MSG_DATA) need = m.len;
    else if (m.kind == DPE_MSG_CREATE) need = 48 + (uint32_t)strlen(m.name) + 1;
    else if (m.kind == DPE_MSG_DESTROY) need = 52 + (uint32_t)strlen(m.name) + 1;
    else need = 4;
    if (!buf || R32(szp) < need) { W32(szp, need); return DPERR(30); }
    (void)bp; (void)bn;
    if (!(fl & 8u)) dpe_receive(ENG(i), ff, tf, 0, &m);
    if (m.kind == DPE_MSG_DATA) gmem_put(buf, m.data, m.len);
    else {
        uint32_t id;
        memcpy(&id, m.data, 4);
        if (m.kind == DPE_MSG_CREATE) {                    /* DPMSG_CREATEPLAYERORGROUP + name */
            memset(guest_ptr(buf, 48), 0, 48);
            W32(buf, 3); W32(buf + 4, 1); W32(buf + 8, id); W32(buf + 12, 0);
            name_out(buf + 24, buf + 48, m.name);
            W32(buf + 44, m.flags);
        } else if (m.kind == DPE_MSG_DESTROY) {            /* DPMSG_DESTROYPLAYERORGROUP + name */
            memset(guest_ptr(buf, 52), 0, 52);
            W32(buf, 5); W32(buf + 4, 1); W32(buf + 8, id);
            name_out(buf + 28, buf + 52, m.name);
            W32(buf + 48, m.flags);
        } else { W32(buf, 0x31); }                          /* DPMSG_SESSIONLOST */
    }
    if (getenv("GPM2_DPLAY_TRACE")) { fprintf(stderr, "[dpmsg] RECV kind=%d from=%u n=%u:", m.kind, m.from, m.len); if (m.kind == DPE_MSG_DATA) for (uint32_t k = 0; k < m.len && k < 32; k++) fprintf(stderr, " %02x", m.data[k]); fprintf(stderr, "\n"); }
    W32(szp, need);
    W32(fromp, m.from); W32(top, m.to);
    return 0;
}

static uint32_t dp_Send_impl(void) {
    int i = obj_of(ARG(0));
    uint32_t n = ARG(5), r;
    static uint8_t b[DPE_MAXMSG];
    if (n > DPE_MAXMSG) return DPERR(230);
    if (n) gmem_get(b, ARG(4), n);
    r = dpe_send(ENG(i), ARG(1), ARG(2), ARG(3), b, n);
    if (getenv("GPM2_DPLAY_TRACE")) { fprintf(stderr, "[dpmsg] SEND to=%u n=%u:", ARG(2), n); for (uint32_t k = 0; k < n && k < 32; k++) fprintf(stderr, " %02x", b[k]); fprintf(stderr, "\n"); }
    if (r) win32_log("DirectPlay: Send(%u -> %u, %u bytes) failed 0x%08x", ARG(1), ARG(2), n, r);
    return r;
}
static uint32_t dp_unsup_impl(void) { return E_NOTIMPL_; }

/* GPM2_DPLAY_TRACE=1: each method with its first 6 (guest) arguments and the returned HRESULT */
static int dp_trace = -1;
#define DP_WRAP(n) static uint32_t dp_##n(void) { \
    uint32_t r = dp_##n##_impl(); \
    if (dp_trace < 0) dp_trace = getenv("GPM2_DPLAY_TRACE") != NULL; \
    if (dp_trace) fprintf(stderr, "[dplay] " #n "(%08x,%08x,%08x,%08x,%08x,%08x) -> %08x\n", ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), r); \
    return r; }
DP_WRAP(QueryInterface)
DP_WRAP(AddRef)
DP_WRAP(Release)
DP_WRAP(Close)
DP_WRAP(CreatePlayer)
DP_WRAP(DestroyPlayer)
DP_WRAP(EnumPlayers)
DP_WRAP(EnumSessions)
DP_WRAP(GetCaps)
DP_WRAP(GetMessageCount)
DP_WRAP(GetPlayerData)
DP_WRAP(GetPlayerName)
DP_WRAP(GetSessionDesc)
DP_WRAP(Open)
DP_WRAP(Receive)
DP_WRAP(Send)
DP_WRAP(SetPlayerData)
DP_WRAP(unsup)

/* ---- DirectPlayCreate / DirectPlayEnumerate ----------------------------------------------------------------------------- */
static void build_vtbl(void) {
    /* IDirectPlay2: order of the interface methods (offset = index * 4) */
    static const struct { int idx; Win32Fn fn; unsigned pop; } m[] = {
        {0, dp_QueryInterface, 12}, {1, dp_AddRef, 4}, {2, dp_Release, 4}, {4, dp_Close, 4}, {6, dp_CreatePlayer, 28},
        {9, dp_DestroyPlayer, 8}, {12, dp_EnumPlayers, 20}, {13, dp_EnumSessions, 24}, {14, dp_GetCaps, 12},
        {17, dp_GetMessageCount, 12}, {20, dp_GetPlayerData, 20}, {21, dp_GetPlayerName, 16}, {22, dp_GetSessionDesc, 12},
        {24, dp_Open, 12}, {25, dp_Receive, 24}, {26, dp_Send, 24}, {29, dp_SetPlayerData, 20},
    };
    uint32_t unsup = win32_thunk(dp_unsup, 4);
    vtbl = heap_alloc(32 * 4);
    for (int i = 0; i < 32; i++) W32(vtbl + 4u * (uint32_t)i, unsup);
    for (unsigned k = 0; k < sizeof m / sizeof m[0]; k++) W32(vtbl + 4u * (uint32_t)m[k].idx, win32_thunk(m[k].fn, m[k].pop));
}

static uint32_t dpl_DirectPlayCreate(void) {
    uint32_t g_ = ARG(0), out = ARG(1);
    int i;
    if (g_) { uint8_t b[16]; gmem_get(b, g_, 16); if (memcmp(b, TCP_GUID, 16) != 0) return DPERR(250); }   /* DPERR_UNAVAILABLE */
    if (!vtbl) build_vtbl();
    for (i = 0; i < MAXOBJ; i++) if (!objs[i].obj) break;
    if (i == MAXOBJ) return DPERR(250);
    memset(&objs[i], 0, sizeof objs[i]);
    objs[i].e = dpe_new();
    objs[i].obj = heap_alloc(16);
    W32(objs[i].obj, vtbl); W32(objs[i].obj + 4, 1); W32(objs[i].obj + 8, (uint32_t)i);
    W32(out, objs[i].obj);
    win32_log("DirectPlayCreate -> 0x%08x", objs[i].obj);
    if (getenv("GPM2_DPLAY_TRACE")) fprintf(stderr, "[dplay] DirectPlayCreate(%08x,%08x) -> 0\n", g_, out);
    return 0;
}

static uint32_t dpl_DirectPlayEnumerate(void) {
    uint32_t cb = ARG(0), ctx = ARG(1), b = heap_alloc(16 + sizeof TCP_NAME);
    gmem_put(b, TCP_GUID, 16);
    gstr_put(b + 16, TCP_NAME);
    if (getenv("GPM2_DPLAY_TRACE")) fprintf(stderr, "[dplay] DirectPlayEnumerate(%08x,%08x)\n", cb, ctx);
    guest_call(cb, 5, b, b + 16, 6u, 0u, ctx);
    heap_free(b);
    return 0;
}

void dplay_register(void) {
    win32_register("DPLAYX.dll!#1", dpl_DirectPlayCreate);
    win32_register("DPLAYX.dll!#2", dpl_DirectPlayEnumerate);
}
