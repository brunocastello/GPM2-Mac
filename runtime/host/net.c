/* Host sockets. See net.h. */
#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void nosigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
    signal(SIGPIPE, SIG_IGN);
#endif
}
static void nonblock(int fd, int on) {
    int f = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
}
static struct sockaddr_in addr_of(const char *ip, unsigned short port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = ip ? inet_addr(ip) : htonl(INADDR_BROADCAST);
    return a;
}

int hn_tcp_listen(unsigned short port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = addr_of("0.0.0.0", port);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 8) < 0) { close(fd); return -1; }
    nonblock(fd, 1);
    return fd;
}
int hn_accept(int lfd) {
    int fd = accept(lfd, NULL, NULL), one = 1;
    if (fd < 0) return -1;
    nonblock(fd, 0); nosigpipe(fd);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}
int hn_tcp_connect(const char *ip, unsigned short port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = addr_of(ip, port);
    struct pollfd p;
    if (fd < 0) return -1;
    nosigpipe(fd);
    nonblock(fd, 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    p.fd = fd; p.events = POLLOUT; p.revents = 0;
    if (poll(&p, 1, timeout_ms) <= 0) { close(fd); return -1; }
    { int err = 0; socklen_t l = sizeof err; getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l); if (err) { close(fd); return -1; } }
    nonblock(fd, 0);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}
int hn_udp_open(unsigned short port, int reuse, int broadcast) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
    struct sockaddr_in a = addr_of("0.0.0.0", port);
    if (fd < 0) return -1;
    if (reuse) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    }
    if (broadcast) setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    nonblock(fd, 1);
    return fd;
}
int hn_udp_send(int fd, const char *ip, unsigned short port, const void *buf, int n) {
    struct sockaddr_in a = addr_of(ip, port);
    return (int)sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&a, sizeof a);
}
int hn_udp_recv(int fd, void *buf, int cap, char *ip_out, unsigned short *port_out) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    ssize_t r = recvfrom(fd, buf, (size_t)cap, 0, (struct sockaddr *)&a, &l);
    if (r <= 0) return 0;
    if (ip_out) { const char *s = inet_ntoa(a.sin_addr); strncpy(ip_out, s, 15); ip_out[15] = 0; }
    if (port_out) *port_out = ntohs(a.sin_port);
    return (int)r;
}
int hn_send(int fd, const void *buf, int n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t r = send(fd, p, (size_t)n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (int)r;
    }
    return 0;
}
int hn_recv(int fd, void *buf, int cap) {
    ssize_t r = recv(fd, buf, (size_t)cap, MSG_DONTWAIT);
    if (r > 0) return (int)r;
    if (r == 0) return -1;
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
}
void hn_close(int fd) { if (fd >= 0) close(fd); }
