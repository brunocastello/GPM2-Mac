/* Thin host socket layer: BSD sockets, portable (macOS/Linux), nothing from Apple.
 * Everything returns -1 on error. TCP: blocking fd for sending, non-blocking reads. */
#ifndef GPM2_NET_H
#define GPM2_NET_H
#include <stddef.h>

int hn_tcp_listen(unsigned short port);                 /* non-blocking fd */
int hn_accept(int lfd);                                  /* fd, or -1 if there is no pending connection */
int hn_tcp_connect(const char *ip, unsigned short port, int timeout_ms);
int hn_udp_open(unsigned short port, int reuse, int broadcast);   /* port 0 = ephemeral */
/* ip NULL = LAN broadcast (255.255.255.255) */
int hn_udp_send(int fd, const char *ip, unsigned short port, const void *buf, int n);
/* >0 = bytes read (ip_out >= 16 bytes receives the sender), 0 = nothing */
int hn_udp_recv(int fd, void *buf, int cap, char *ip_out, unsigned short *port_out);
int hn_send(int fd, const void *buf, int n);             /* sends everything; -1 if the connection dropped */
int hn_recv(int fd, void *buf, int cap);                 /* >0 bytes, 0 = nothing, -1 = closed */
void hn_close(int fd);
#endif
