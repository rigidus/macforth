// === file: src/net/net.h ===
#pragma once
#include <stdint.h>
/* forward-decl, чтобы не тянуть системные сокетные заголовки в header */
struct sockaddr;

#ifdef __cplusplus
extern "C" {
#endif

    /* Платформенно-правильный тип дескриптора */
#if defined(_WIN32)
#  include <winsock2.h>
    typedef SOCKET net_fd_t;
#else
    typedef int net_fd_t;
#endif

    typedef struct NetPoller NetPoller;
    typedef void (*NetFdCb)(void* user, net_fd_t fd, int events);

    enum { NET_RD = 1, NET_WR = 2, NET_ERR = 4 };

    /* Создание/удаление */
    NetPoller* net_poller_create(void);
    void       net_poller_destroy(NetPoller*);

    /* Управление интересами */
    int  net_poller_add(NetPoller*, net_fd_t fd, int mask, NetFdCb cb, void* user);
    void net_poller_mod(NetPoller*, net_fd_t fd, int new_mask);
    void net_poller_del(NetPoller*, net_fd_t fd);

    /* Неблокирующий опрос; budget_ms — желаемый максимум времени (0 = сразу вернуть) */
    void net_poller_tick(NetPoller*, uint32_t now_ms, int budget_ms);

    /* Установить/снять неблокирующий режим на сокете */
    int  net_set_nonblocking(net_fd_t fd, int nonblocking);


    /* ===== Неблокирующее подключение (этап 2.4) =====
       Протокол:
       - сокет уже создан (обычно TCP) и переведён в nonblocking;
       - net_connect_nb(fd, addr, addrlen):
       return  0  -> соединение установлено синхронно;
       return  1  -> EINPROGRESS/WOULDBLOCK: ждём NET_WR и затем проверяем net_connect_finished();
       return -1  -> ошибка сразу.
       - при событии NET_WR -> net_connect_finished(fd):
       return  0  -> успех, можно переключаться на чтение/запись;
       return -1  -> ошибка (код можно получить через net_get_so_error()).
    */
    enum { NET_OK = 0, NET_INPROGRESS = 1, NET_ERR_GENERIC = -1 };

    /* Создать TCP-сокет (AF_INET, SOCK_STREAM). Возврат: fd или -1/INVALID_SOCKET. */
    net_fd_t net_socket_tcp(void);
    /* Закрыть сокет. Возврат 0 при успехе. */
    int      net_close_fd(net_fd_t fd);
    /* Старт неблокирующего connect(). */
    int      net_connect_nb(net_fd_t fd, const struct sockaddr* addr, int addrlen);
    /* Завершение connect() после события WRITABLE (getsockopt(SO_ERROR)). */
    int      net_connect_finished(net_fd_t fd);
    /* Последняя платформа-зависимая ошибка (errno/WSAGetLastError). */
    int      net_last_error(void);
    /* true, если err — «временно недоступно/в процессе» (EAGAIN/EWOULDBLOCK/EINPROGRESS). */
    int      net_err_would_block(int err);
    /* Получить SO_ERROR для сокета (0 если нет ошибки). */
    int      net_get_so_error(net_fd_t fd);

#ifdef __cplusplus
}
#endif
