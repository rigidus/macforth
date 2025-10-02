#pragma once
#include <stdint.h>
#include <stddef.h>
#include "net.h"   /* net_fd_t, NET_OK/NET_INPROGRESS, helpers */

#ifdef __cplusplus
extern "C" {
#endif

    typedef net_fd_t tcp_fd_t;
#if defined(_WIN32)
#  include <basetsd.h>
    typedef SSIZE_T tcp_ssize_t;
#else
    typedef long tcp_ssize_t; /* ssize_t эквивалент; объявляем локально, чтобы не тянуть <sys/types.h> в публичный хедер */
#endif

#ifndef TCP_INVALID_FD
#  define TCP_INVALID_FD ((tcp_fd_t)(intptr_t)-1)
#endif

    /* Безопасно вызывать много раз (WSAStartup на Windows). */
    int      tcp_init(void);

    /* Слушающий неблокирующий сокет на 0.0.0.0:port. Возвращает fd или <0 при ошибке. */
    tcp_fd_t tcp_listen(uint16_t port, int backlog);

    /* Принять одно соединение.
       set_nonblock!=0 — перевести клиентский сокет в неблокирующий режим.
       out_ipv4_be/out_port_be — опционально вернуть peer IPv4/port в сетевом порядке (BE).
       Возвращает: 0 — успех; 1 — нет ожидающих (would-block); -1 — ошибка. */
    int      tcp_accept(tcp_fd_t listen_fd, int set_nonblock,
                        tcp_fd_t* out_client,
                        uint32_t* out_ipv4_be, uint16_t* out_port_be);

    /* Неблокирующий connect по host:port (IPv4/имя).
       set_nonblock!=0 — перевести сокет в O_NONBLOCK перед connect().
       Возврат: NET_OK / NET_INPROGRESS / NET_ERR_GENERIC. */
    int      tcp_connect(const char* host, uint16_t port, int set_nonblock, tcp_fd_t* out_fd);

    int      tcp_set_nonblock(tcp_fd_t fd, int nonblock);
    int      tcp_close(tcp_fd_t fd);

    /* Небольшой кроссплатформенный iovec для чтения. */
    typedef struct tcp_iovec { void* base; size_t len; } tcp_iovec;

    /* Векторное чтение: возвращает >=0 байт, 0 — закрыто, -1 — ошибка (см. net_last_error()/net_err_would_block()). */
    tcp_ssize_t tcp_readv(tcp_fd_t fd, const tcp_iovec* iov, int iovcnt);

    /* Попытаться записать "всё" (последовательностью send) без блокировок.
       out_written — сколько байт удалось отправить.
       Возврат: 0 — всё отправлено; 1 — упёрлись в EWOULDBLOCK (осталось дописать позже);
       -1 — ошибка (фатальная). */
    int      tcp_write_all(tcp_fd_t fd, const void* buf, size_t len, size_t* out_written);

#ifdef __cplusplus
}
#endif
