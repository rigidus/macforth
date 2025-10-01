// === file: src/net/net_stub_emscripten.c ===
#ifdef __EMSCRIPTEN__
#include "net.h"
#include <stdlib.h>

struct NetPoller { int _; };

NetPoller* net_poller_create(void){ return (NetPoller*)calloc(1,sizeof(struct NetPoller)); }
void       net_poller_destroy(NetPoller* np){ free(np); }
int  net_poller_add(NetPoller* np, net_fd_t fd, int mask, NetFdCb cb, void* user){ (void)np;(void)fd;(void)mask;(void)cb;(void)user; return -1; }
void net_poller_mod(NetPoller* np, net_fd_t fd, int new_mask){ (void)np;(void)fd;(void)new_mask; }
void net_poller_del(NetPoller* np, net_fd_t fd){ (void)np;(void)fd; }
void net_poller_tick(NetPoller* np, uint32_t now_ms, int budget_ms){ (void)np;(void)now_ms;(void)budget_ms; }
int  net_set_nonblocking(net_fd_t fd, int nonblocking){ (void)fd;(void)nonblocking; return -1; }

/* Эмулятор сокет-API для wasm: всё «не поддерживается». */
net_fd_t net_socket_tcp(void){ return (net_fd_t)-1; }
int      net_close_fd(net_fd_t fd){ (void)fd; return -1; }
int      net_connect_nb(net_fd_t fd, const struct sockaddr* addr, int addrlen){
    (void)fd; (void)addr; (void)addrlen; return NET_ERR_GENERIC;
}
int      net_connect_finished(net_fd_t fd){ (void)fd; return NET_ERR_GENERIC; }
int      net_last_error(void){ return -1; }
int      net_err_would_block(int err){ (void)err; return 0; }
int      net_get_so_error(net_fd_t fd){ (void)fd; return -1; }

#endif /* __EMSCRIPTEN__ */
