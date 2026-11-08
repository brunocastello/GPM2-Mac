/* DirectPlay engine: session, players, messages over TCP + UDP discovery. No guest memory:
 * the COM layer in dplay.c converts. C99 + pthread. */
#ifndef GPM2_DPLAY_ENG_H
#define GPM2_DPLAY_ENG_H
#include <stdint.h>

#define DPE_PORT 47624u            /* default DirectPlay port (TCP and UDP); GPM2_NET_PORT overrides it */
#define DPE_MAXMSG 16384

typedef struct {
    uint8_t inst[16], app[16];     /* guidInstance, guidApplication */
    uint32_t flags, maxp, curp, user[4];
    char name[64];
    char ip[16]; unsigned short port;    /* only in sessions found by EnumSessions */
} DpeSess;
typedef struct { uint32_t id, flags; int local; char name[48]; } DpePlayer;
enum { DPE_MSG_DATA = 0, DPE_MSG_CREATE = 1, DPE_MSG_DESTROY = 2, DPE_MSG_LOST = 3 };
typedef struct { int kind; uint32_t from, to, len, flags; uint8_t data[DPE_MAXMSG]; char name[48]; } DpeMsg;

/* returned codes (DirectPlay HRESULT) */
#define DPE_OK 0
#define DPE_NOMESSAGES 0x88770000u + 190u
#define DPE_BUFFERTOOSMALL 0x88770000u + 30u
#define DPE_INVALIDPLAYER 0x88770000u + 150u
#define DPE_NOSESSIONS 0x88770000u + 220u
#define DPE_NOCONNECTION 0x88770000u + 170u
#define DPE_CANTCREATESESSION 0x88770000u + 70u
#define DPE_CANTCREATEPLAYER 0x88770000u + 60u
#define DPE_TIMEOUT 0x88770000u + 240u
#define DPE_ACCESSDENIED 0x88770000u + 10u
#define DPE_GENERIC 0x80004005u
#define DPE_SENDTOOBIG 0x88770000u + 230u

typedef struct DpEng DpEng;
DpEng *dpe_new(void);
void dpe_free(DpEng *e);
void dpe_service_all(void);            /* threadless host: network of the open engines */
/* `addr` ("ip" or "ip:port", may be NULL) = in addition to the LAN broadcast. Returns the number of sessions (<= max). */
int dpe_enum_sessions(DpEng *e, const char *addr, int timeout_ms, DpeSess *out, int max);
uint32_t dpe_open_create(DpEng *e, const DpeSess *s);
uint32_t dpe_open_join(DpEng *e, const DpeSess *s);          /* s = a session returned by dpe_enum_sessions */
void dpe_close(DpEng *e);
int dpe_is_open(DpEng *e);
uint32_t dpe_create_player(DpEng *e, const char *name, uint32_t flags, uint32_t *id);
uint32_t dpe_destroy_player(DpEng *e, uint32_t id);
int dpe_enum_players(DpEng *e, DpePlayer *out, int max);
int dpe_player(DpEng *e, uint32_t id, DpePlayer *out);
void dpe_session(DpEng *e, DpeSess *out);
uint32_t dpe_send(DpEng *e, uint32_t from, uint32_t to, uint32_t flags, const void *buf, uint32_t len);
/* removes (or peeks at) the next message; to_filter != 0: only those addressed to that player (system ones always pass) */
uint32_t dpe_receive(DpEng *e, uint32_t from_filter, uint32_t to_filter, int peek, DpeMsg *m);
uint32_t dpe_msg_count(DpEng *e);
void dpe_set_data(DpEng *e, uint32_t id, const void *d, uint32_t n);
uint32_t dpe_get_data(DpEng *e, uint32_t id, void *d, uint32_t cap);   /* total size */
#endif
