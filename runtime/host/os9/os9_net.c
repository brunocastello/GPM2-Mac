/* Host socket layer over Open Transport (Mac OS 9, CarbonLib) for Link Play. Same interface as host/net.h.
 * Synchronous, non-blocking endpoints; waits (connect, send with a full flow) pump events with os9_pump.
 * The "fds" are index+1 into a table of EndpointRef. */
#include <OpenTransport.h>
#include <OpenTransportProviders.h>
#include <MacErrors.h>
#include <Gestalt.h>

#include "os9_compat.h"
#include "../net.h"
#include "../host.h"

#define MAXEP 32
static struct { EndpointRef ep; int kind; } gEp[MAXEP];          /* kind: 0 free, 1 TCP listening, 2 TCP connection, 3 UDP */
static OTClientContextPtr gCtx;
static int gInit;                                                /* 0 = not tried, 1 = ok, -1 = no Open Transport */

static int ot_ready(void) {
    long r = 0;
    if (gInit) return gInit > 0;
    gInit = -1;
    if (Gestalt(gestaltOpenTpt, &r) != noErr || !(r & 1)) return 0;
    if (InitOpenTransportInContext(kInitOTForApplicationMask, &gCtx) != noErr) return 0;
    gInit = 1;
    return 1;
}
static int slot_new(EndpointRef ep, int kind) {
    for (int i = 0; i < MAXEP; i++) if (!gEp[i].kind) { gEp[i].ep = ep; gEp[i].kind = kind; return i + 1; }
    return -1;
}
static EndpointRef ep_of(int fd) { return fd >= 1 && fd <= MAXEP && gEp[fd - 1].kind ? gEp[fd - 1].ep : NULL; }
static EndpointRef ep_open(const char *proto) {
    OSStatus err = noErr;
    EndpointRef ep = OTOpenEndpointInContext(OTCreateConfiguration(proto), 0, NULL, &err, gCtx);
    if (err != noErr || !ep) return NULL;
    OTSetSynchronous(ep);
    OTSetNonBlocking(ep);
    return ep;
}
static int ep_bind(EndpointRef ep, unsigned short port, int qlen) {
    InetAddress a;
    TBind req;
    OTInitInetAddress(&a, port, kOTAnyInetAddress);
    memset(&req, 0, sizeof req);
    req.addr.buf = (UInt8 *)&a; req.addr.len = sizeof a; req.addr.maxlen = sizeof a;
    req.qlen = (UInt32)qlen;
    return OTBind(ep, &req, NULL) == noErr;
}
static unsigned long parse_ip(const char *ip) {                  /* "a.b.c.d" -> host byte order; 0xFFFFFFFF if invalid */
    unsigned a[4];
    if (!ip) return 0xFFFFFFFFUL;                                /* NULL = LAN broadcast */
    if (sscanf(ip, "%u.%u.%u.%u", &a[0], &a[1], &a[2], &a[3]) != 4 || a[0] > 255 || a[1] > 255 || a[2] > 255 || a[3] > 255) return 0xFFFFFFFFUL;
    return ((unsigned long)a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3];
}
static void set_opt(EndpointRef ep, UInt32 name, UInt32 val) {   /* IP_BROADCAST, IP_REUSEADDR (best effort) */
    struct { TOptionHeader h; UInt32 v; } o;
    TOptMgmt req, ret;
    o.h.len = sizeof o; o.h.level = INET_IP; o.h.name = name; o.h.status = 0; o.v = val;
    req.opt.buf = (UInt8 *)&o; req.opt.len = sizeof o; req.opt.maxlen = sizeof o; req.flags = T_NEGOTIATE;
    ret = req;
    OTOptionManagement(ep, &req, &ret);
}

int hn_tcp_listen(unsigned short port) {
    EndpointRef ep;
    int fd;
    if (!ot_ready() || !(ep = ep_open(kTCPName))) return -1;
    set_opt(ep, IP_REUSEADDR, 1);
    if (!ep_bind(ep, port, 8) || (fd = slot_new(ep, 1)) < 0) { OTCloseProvider(ep); return -1; }
    return fd;
}
int hn_accept(int lfd) {
    EndpointRef l = ep_of(lfd), ep;
    TCall call;
    InetAddress from;
    OSStatus err;
    int fd;
    if (!l) return -1;
    memset(&call, 0, sizeof call);
    call.addr.buf = (UInt8 *)&from; call.addr.maxlen = sizeof from;
    err = OTListen(l, &call);
    if (err == kOTLookErr) {                                     /* pending event: clear it (disconnect from an earlier request) */
        if (OTLook(l) == T_DISCONNECT) OTRcvDisconnect(l, NULL);
        return -1;
    }
    if (err != noErr) return -1;
    if (!(ep = ep_open(kTCPName))) return -1;
    if (!ep_bind(ep, 0, 0)) { OTCloseProvider(ep); return -1; }
    OTSetBlocking(ep);
    OTSetBlocking(l);
    err = OTAccept(l, ep, &call);
    OTSetNonBlocking(l);
    OTSetNonBlocking(ep);
    if (err != noErr || (fd = slot_new(ep, 2)) < 0) {
        if (err == kOTLookErr && OTLook(l) == T_DISCONNECT) OTRcvDisconnect(l, NULL);
        OTCloseProvider(ep);
        return -1;
    }
    return fd;
}
int hn_tcp_connect(const char *ip, unsigned short port, int timeout_ms) {
    EndpointRef ep;
    InetAddress a;
    TCall snd;
    OSStatus err;
    uint32_t t0 = host_now_ms();
    int fd;
    if (!ot_ready() || !ip || !(ep = ep_open(kTCPName))) return -1;
    if (OTBind(ep, NULL, NULL) != noErr) { OTCloseProvider(ep); return -1; }
    OTInitInetAddress(&a, port, parse_ip(ip));
    memset(&snd, 0, sizeof snd);
    snd.addr.buf = (UInt8 *)&a; snd.addr.len = sizeof a; snd.addr.maxlen = sizeof a;
    err = OTConnect(ep, &snd, NULL);
    while (err == kOTNoDataErr || err == kOTLookErr || err == noErr) {
        if (err == noErr || (err = OTRcvConnect(ep, NULL)) == noErr) break;
        if (err == kOTLookErr) {
            if (OTLook(ep) == T_DISCONNECT) { OTRcvDisconnect(ep, NULL); err = -1; break; }
            err = kOTNoDataErr;
        }
        if (err != kOTNoDataErr) break;
        if ((int)(host_now_ms() - t0) > timeout_ms) { err = -1; break; }
        os9_pump(10);
    }
    if (err != noErr || (fd = slot_new(ep, 2)) < 0) { OTCloseProvider(ep); return -1; }
    return fd;
}
int hn_udp_open(unsigned short port, int reuse, int broadcast) {
    EndpointRef ep;
    int fd;
    if (!ot_ready() || !(ep = ep_open(kUDPName))) return -1;
    if (reuse) set_opt(ep, IP_REUSEADDR, 1);
    if (broadcast) set_opt(ep, IP_BROADCAST, 1);
    if (!ep_bind(ep, port, 0) || (fd = slot_new(ep, 3)) < 0) { OTCloseProvider(ep); return -1; }
    return fd;
}
int hn_udp_send(int fd, const char *ip, unsigned short port, const void *buf, int n) {
    EndpointRef ep = ep_of(fd);
    InetAddress a;
    TUnitData ud;
    if (!ep) return -1;
    OTInitInetAddress(&a, port, parse_ip(ip));
    memset(&ud, 0, sizeof ud);
    ud.addr.buf = (UInt8 *)&a; ud.addr.len = sizeof a;
    ud.udata.buf = (UInt8 *)buf; ud.udata.len = (UInt32)n;
    return OTSndUData(ep, &ud) == noErr ? n : -1;
}
int hn_udp_recv(int fd, void *buf, int cap, char *ip_out, unsigned short *port_out) {
    EndpointRef ep = ep_of(fd);
    InetAddress a;
    TUnitData ud;
    OTFlags fl = 0;
    OSStatus err;
    if (!ep) return 0;
    memset(&ud, 0, sizeof ud);
    ud.addr.buf = (UInt8 *)&a; ud.addr.maxlen = sizeof a;
    ud.udata.buf = (UInt8 *)buf; ud.udata.maxlen = (UInt32)cap;
    err = OTRcvUData(ep, &ud, &fl);
    if (err == kOTLookErr) { if (OTLook(ep) == T_UDERR) OTRcvUDErr(ep, NULL); return 0; }
    if (err != noErr || ud.udata.len == 0) return 0;
    if (ip_out) snprintf(ip_out, 16, "%u.%u.%u.%u", (unsigned)((a.fHost >> 24) & 255), (unsigned)((a.fHost >> 16) & 255), (unsigned)((a.fHost >> 8) & 255), (unsigned)(a.fHost & 255));
    if (port_out) *port_out = a.fPort;
    return (int)ud.udata.len;
}
int hn_send(int fd, const void *buf, int n) {
    EndpointRef ep = ep_of(fd);
    const char *p = (const char *)buf;
    uint32_t t0 = host_now_ms();
    if (!ep) return -1;
    while (n > 0) {
        OTResult r = OTSnd(ep, (void *)p, (OTByteCount)n, 0);
        if (r > 0) { p += r; n -= (int)r; t0 = host_now_ms(); continue; }
        if (r == kOTFlowErr || r == kOTNoDataErr || r == 0) {      /* flow full: wait a little */
            if (host_now_ms() - t0 > 5000u) return -1;
            os9_pump(5);
            continue;
        }
        if (r == kOTLookErr) { OTResult lk = OTLook(ep); if (lk == T_DISCONNECT || lk == T_ORDREL) return -1; os9_pump(5); continue; }
        return -1;
    }
    return 0;
}
int hn_recv(int fd, void *buf, int cap) {
    EndpointRef ep = ep_of(fd);
    OTFlags fl = 0;
    OTResult r;
    if (!ep) return -1;
    r = OTRcv(ep, buf, (OTByteCount)cap, &fl);
    if (r > 0) return (int)r;
    if (r == kOTNoDataErr || r == 0) return 0;
    if (r == kOTLookErr) {
        OTResult lk = OTLook(ep);
        if (lk == T_DISCONNECT) { OTRcvDisconnect(ep, NULL); return -1; }
        if (lk == T_ORDREL) { OTRcvOrderlyDisconnect(ep); return -1; }
        return 0;
    }
    return -1;
}
void hn_close(int fd) {
    EndpointRef ep = ep_of(fd);
    if (!ep) return;
    if (gEp[fd - 1].kind == 2) OTSndDisconnect(ep, NULL);
    OTCloseProvider(ep);
    gEp[fd - 1].kind = 0; gEp[fd - 1].ep = NULL;
}
