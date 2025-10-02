#define _POSIX_C_SOURCE 200112L  /* откроет getaddrinfo/addrinfo в netdb.h */

#if defined(_WIN32)
// ВАЖНО: winsock2.h должен идти раньше windows.h
#include <winsock2.h>
#include <ws2tcpip.h>   // addrinfo, getaddrinfo/freeaddrinfo, AI_*
// На старых SDK может не быть AI_ADDRCONFIG
#ifndef AI_ADDRCONFIG
#define AI_ADDRCONFIG 0
#endif
#else
#include <sys/types.h>
#include <sys/socket.h> // socket, connect
#include <netdb.h>      // struct addrinfo, getaddrinfo/freeaddrinfo
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#endif

#include <stdio.h>   // snprintf
#include <string.h>  // memset/strlen и т.п.

#include "net/tcp.h"
#include <string.h>

#ifdef __EMSCRIPTEN__
/* В wasm сети нет — отдаём заглушки совместимые с API. */
int tcp_init(void){ return -1; }
tcp_fd_t tcp_listen(uint16_t port, int backlog){ (void)port; (void)backlog; return (tcp_fd_t)TCP_INVALID_FD; }
int tcp_accept(tcp_fd_t lf, int nb, tcp_fd_t* out, uint32_t* ip, uint16_t* port){
    (void)lf;(void)nb;(void)out;(void)ip;(void)port; return -1;
}
int tcp_connect(const char* host, uint16_t port, int nb, tcp_fd_t* out_fd){
    (void)host;(void)port;(void)nb;(void)out_fd; return NET_ERR_GENERIC;
}
int tcp_set_nonblock(tcp_fd_t fd, int nb){ (void)fd;(void)nb; return -1; }
int tcp_close(tcp_fd_t fd){ (void)fd; return -1; }
tcp_ssize_t tcp_readv(tcp_fd_t fd, const tcp_iovec* iov, int iovcnt){ (void)fd;(void)iov;(void)iovcnt; return -1; }
int tcp_write_all(tcp_fd_t fd, const void* buf, size_t len, size_t* out_written){
    (void)fd;(void)buf;(void)len; if(out_written)*out_written=0; return -1;
}
#else

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <malloc.h>
static int g_wsa_once = 0;
int tcp_init(void){ if (!g_wsa_once){ WSADATA d; if (WSAStartup(MAKEWORD(2,2), &d)!=0) return -1; g_wsa_once=1; } return 0; }
static int s_set_nb(tcp_fd_t fd, int nb){ u_long m = nb?1u:0u; return ioctlsocket(fd, FIONBIO, &m); }
static int s_close(tcp_fd_t fd){ return closesocket(fd); }
static int s_last_err(void){ return WSAGetLastError(); }
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/uio.h>
int tcp_init(void){ return 0; }
static int s_set_nb(tcp_fd_t fd, int nb){
    int f = fcntl(fd, F_GETFL, 0); if (f<0) return -1;
    return fcntl(fd, F_SETFL, nb ? (f|O_NONBLOCK) : (f & ~O_NONBLOCK));
}
static int s_close(tcp_fd_t fd){ return close(fd); }
static int s_last_err(void){ return errno; }
#endif

/* helpers */
static int s_nb_would_block(int err){ return net_err_would_block(err); }

int tcp_set_nonblock(tcp_fd_t fd, int nonblock){
    return s_set_nb(fd, nonblock);
}

int tcp_close(tcp_fd_t fd){
    if ((intptr_t)fd < 0) return -1;
    return s_close(fd);
}

tcp_fd_t tcp_listen(uint16_t port, int backlog){
    if (tcp_init()!=0) return (tcp_fd_t)TCP_INVALID_FD;
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s==INVALID_SOCKET) return (tcp_fd_t)TCP_INVALID_FD;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return (tcp_fd_t)TCP_INVALID_FD;
#endif
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, (socklen_t)sizeof(yes));

    struct sockaddr_in a; memset(&a,0,sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*)&a, (socklen_t)sizeof(a)) != 0){
        s_close(s); return (tcp_fd_t)TCP_INVALID_FD;
    }
    if (listen(s, (backlog>0? backlog:64)) != 0){
        s_close(s); return (tcp_fd_t)TCP_INVALID_FD;
    }
    (void)s_set_nb(s, 1);
    return (tcp_fd_t)s;
}

int tcp_accept(tcp_fd_t listen_fd, int set_nonblock,
               tcp_fd_t* out_client, uint32_t* out_ipv4_be, uint16_t* out_port_be)
{
    if (out_client) *out_client = (tcp_fd_t)TCP_INVALID_FD;
    struct sockaddr_in ra; socklen_t rl = (socklen_t)sizeof(ra);
#if defined(_WIN32)
    SOCKET c = accept(listen_fd, (struct sockaddr*)&ra, &rl);
    if (c == INVALID_SOCKET){
        int err = s_last_err();
        return s_nb_would_block(err) ? 1 : -1;
    }
#else
    int c = accept(listen_fd, (struct sockaddr*)&ra, &rl);
    if (c < 0){
        int err = s_last_err();
        return s_nb_would_block(err) ? 1 : -1;
    }
#endif
    if (set_nonblock) s_set_nb(c, 1);
    if (out_client) *out_client = (tcp_fd_t)c;
    if (out_ipv4_be) *out_ipv4_be = ra.sin_addr.s_addr;
    if (out_port_be) *out_port_be = ra.sin_port;
    return 0;
}

int tcp_connect(const char* host, uint16_t port, int set_nb, tcp_fd_t* out_fd){
    if (out_fd) *out_fd = (tcp_fd_t)TCP_INVALID_FD;
    if (!host || !*host) return NET_ERR_GENERIC;
    if (tcp_init()!=0) return NET_ERR_GENERIC;

    /* resolve */
    char port_str[16]; port_str[0]=0;
#if defined(_WIN32)
    _snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
#else
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
#endif
    struct addrinfo hints;
    memset(&hints,0,sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
#ifdef AI_ADDRCONFIG
    hints.ai_flags    = AI_ADDRCONFIG;
#endif
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo* res = NULL;
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0 || !res) return NET_ERR_GENERIC;

    /* try first result only (IPv4) */
    int rc = NET_ERR_GENERIC;
#if defined(_WIN32)
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET){ freeaddrinfo(res); return NET_ERR_GENERIC; }
#else
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0){ freeaddrinfo(res); return NET_ERR_GENERIC; }
#endif
    if (set_nb) s_set_nb(s, 1);
    int c = connect(s, res->ai_addr, (int)res->ai_addrlen);
    if (c == 0){
        rc = NET_OK;
        if (out_fd) *out_fd = (tcp_fd_t)s;
    } else {
        int err = s_last_err();
        if (s_nb_would_block(err)){
            rc = NET_INPROGRESS;
            if (out_fd) *out_fd = (tcp_fd_t)s;
        } else {
            s_close(s);
            rc = NET_ERR_GENERIC;
        }
    }
    freeaddrinfo(res);
    return rc;
}

/* ====== IO ====== */
tcp_ssize_t tcp_readv(tcp_fd_t fd, const tcp_iovec* iov, int iovcnt){
    if (!iov || iovcnt<=0) return 0;
#if defined(_WIN32)
    /* WSARecv с WSABUF */
    if (iovcnt > 16) iovcnt = 16; /* разумный предел для стека */
    WSABUF bufs[16];
    for (int i=0;i<iovcnt;i++){ bufs[i].buf = (CHAR*)iov[i].base; bufs[i].len = (ULONG)iov[i].len; }
    DWORD recvd = 0;
    DWORD flags = 0;
    int rc = WSARecv(fd, bufs, (DWORD)iovcnt, &recvd, &flags, NULL, NULL);
    if (rc == 0) return (tcp_ssize_t)recvd;
    int err = s_last_err();
    if (s_nb_would_block(err)) return -1; /* будет EWOULDBLOCK через net_last_error() */
    return -1;
#else
    /* POSIX readv */
    struct iovec v[16];
    if (iovcnt > 16) iovcnt = 16;
    for (int i=0;i<iovcnt;i++){ v[i].iov_base = iov[i].base; v[i].iov_len = iov[i].len; }
    ssize_t n = readv(fd, v, iovcnt);
    return (tcp_ssize_t)n;
#endif
}

int tcp_write_all(tcp_fd_t fd, const void* buf, size_t len, size_t* out_written){
    size_t off = 0;
    const char* p = (const char*)buf;
    while (off < len){
#if defined(_WIN32)
        int rc = send(fd, p + off, (int)(len - off), 0);
#else
        int rc = (int)send(fd, p + off, (size_t)(len - off), 0);
#endif
        if (rc > 0){ off += (size_t)rc; continue; }
        int err = s_last_err();
        if (s_nb_would_block(err)){ if (out_written) *out_written = off; return 1; }
        if (out_written) *out_written = off;
        return -1;
    }
    if (out_written) *out_written = off;
    return 0;
}

#endif /* !__EMSCRIPTEN__ */
