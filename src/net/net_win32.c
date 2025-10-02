// === file: src/net/net_win32.c ===
#include "net.h"
#include "wire_tcp.h" /* header присутствует, но сам модуль используется опционально */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#pragma comment(lib, "Ws2_32.lib")

typedef struct NetEntry {
    net_fd_t fd; int mask; NetFdCb cb; void* user; int alive;
} NetEntry;
typedef enum { OP_ADD, OP_MOD, OP_DEL } OpKind;
typedef struct PendingOp { OpKind kind; net_fd_t fd; int mask; NetFdCb cb; void* user; } PendingOp;

struct NetPoller {
    NetEntry* entries; size_t len, cap;
    PendingOp* ops; size_t olen, ocap;
    int in_tick;
    int wsa_inited;
};

/* Глобальный инициализатор WSA для функций вне поллера */
static int g_wsa_once = 0;
static void ensure_wsa_global(void){ if (!g_wsa_once){ WSADATA d; WSAStartup(MAKEWORD(2,2), &d); g_wsa_once = 1; } }

static void ensure_wsa(struct NetPoller* np){
    if (!np->wsa_inited){ WSADATA d; WSAStartup(MAKEWORD(2,2), &d); np->wsa_inited = 1; }
}
static NetEntry* find_entry(struct NetPoller* np, net_fd_t fd){
    for (size_t i=0;i<np->len;i++) if (np->entries[i].alive && np->entries[i].fd==fd) return &np->entries[i];
    return NULL;
}
static void push_op(struct NetPoller* np, PendingOp op){
    if (np->olen==np->ocap){ size_t n=np->ocap?np->ocap*2:8; np->ops=(PendingOp*)realloc(np->ops,n*sizeof(PendingOp)); np->ocap=n; }
    np->ops[np->olen++] = op;
}
static void apply_ops(struct NetPoller* np){
    for (size_t i=0;i<np->olen;i++){
        PendingOp* op = &np->ops[i];
        if (op->kind==OP_ADD){
            NetEntry* e = find_entry(np, op->fd);
            if (!e){
                if (np->len==np->cap){ size_t n=np->cap?np->cap*2:16; np->entries=(NetEntry*)realloc(np->entries,n*sizeof(NetEntry)); np->cap=n; }
                NetEntry ne; ne.fd=op->fd; ne.mask=op->mask; ne.cb=op->cb; ne.user=op->user; ne.alive=1;
                np->entries[np->len++] = ne;
            } else { e->mask=op->mask; e->cb=op->cb; e->user=op->user; e->alive=1; }
        } else if (op->kind==OP_MOD){
            NetEntry* e = find_entry(np, op->fd); if (e) e->mask = op->mask;
        } else {
            NetEntry* e = find_entry(np, op->fd); if (e) e->alive = 0;
        }
    }
    size_t w=0; for (size_t i=0;i<np->len;i++){ if (np->entries[i].alive){ if(w!=i) np->entries[w]=np->entries[i]; w++; } }
    np->len=w; np->olen=0;
}

NetPoller* net_poller_create(void){ struct NetPoller* np=(struct NetPoller*)calloc(1,sizeof(*np)); ensure_wsa(np); return np; }
void net_poller_destroy(NetPoller* np){ if(!np) return; free(np->entries); free(np->ops); if(np->wsa_inited) WSACleanup(); free(np); }

int  net_poller_add(NetPoller* np, net_fd_t fd, int mask, NetFdCb cb, void* user){ if(!np||!cb) return -1; PendingOp op={OP_ADD,fd,mask,cb,user}; if(np->in_tick) push_op(np,op); else { push_op(np,op); apply_ops(np);} return 0; }
void net_poller_mod(NetPoller* np, net_fd_t fd, int new_mask){ if(!np) return; PendingOp op={OP_MOD,fd,new_mask,0,0}; if(np->in_tick) push_op(np,op); else { push_op(np,op); apply_ops(np);} }
void net_poller_del(NetPoller* np, net_fd_t fd){ if(!np) return; PendingOp op={OP_DEL,fd,0,0,0}; if(np->in_tick) push_op(np,op); else { push_op(np,op); apply_ops(np);} }

static short to_poll_events(int mask){ short ev=0; if (mask&NET_RD) ev|=POLLRDNORM; if (mask&NET_WR) ev|=POLLWRNORM; return ev; }
static int from_poll_revents(short rev){ int ev=0; if (rev&(POLLRDNORM|POLLPRI)) ev|=NET_RD; if (rev&POLLWRNORM) ev|=NET_WR; if (rev&(POLLERR|POLLHUP)) ev|=NET_ERR; return ev; }

void net_poller_tick(NetPoller* np, uint32_t now_ms, int budget_ms){
    (void)now_ms;
    if(!np) return;
    if(np->olen) apply_ops(np);
    if(np->len==0) return;
    WSAPOLLFD* pfds=(WSAPOLLFD*)_alloca(np->len*sizeof(WSAPOLLFD));
    for(size_t i=0;i<np->len;i++){ pfds[i].fd=np->entries[i].fd; pfds[i].events=to_poll_events(np->entries[i].mask); pfds[i].revents=0; }
    int timeout = budget_ms>0 ? budget_ms : 0;
    np->in_tick=1;
    int rc=WSAPoll(pfds,(ULONG)np->len,timeout);
    if(rc>0){ for(size_t i=0;i<np->len;i++){ if(!pfds[i].revents) continue; int ev=from_poll_revents(pfds[i].revents); NetEntry* e=&np->entries[i]; if(e->cb) e->cb(e->user, e->fd, ev); } }
    np->in_tick=0;
    if(np->olen) apply_ops(np);
}

int net_set_nonblocking(net_fd_t fd, int nonblocking){ u_long mode = nonblocking ? 1u : 0u; return ioctlsocket(fd, FIONBIO, &mode); }


/* ==================== неблокирующие сокеты/коннект ==================== */

net_fd_t net_socket_tcp(void){
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    return (s==INVALID_SOCKET) ? (net_fd_t)INVALID_SOCKET : (net_fd_t)s;
}

int net_close_fd(net_fd_t fd){
    return closesocket(fd);
}

int net_last_error(void){
    return WSAGetLastError();
}

int net_err_would_block(int err){
    return (err==WSAEWOULDBLOCK) || (err==WSAEINPROGRESS) || (err==WSAEALREADY);
}

/* Старт неблокирующего connect() (сокет уже переведён в nonblocking). */
int net_connect_nb(net_fd_t fd, const struct sockaddr* addr, int addrlen){
    if (fd==INVALID_SOCKET || !addr || addrlen<=0) return NET_ERR_GENERIC;
    int rc = connect(fd, addr, addrlen);
    if (rc == 0) return NET_OK;
    int e = WSAGetLastError();
    if (e==WSAEWOULDBLOCK || e==WSAEINPROGRESS || e==WSAEALREADY) return NET_INPROGRESS;
    return NET_ERR_GENERIC;
}

/* Проверка результата после события WRITABLE: SO_ERROR должен быть 0. */
int net_get_so_error(net_fd_t fd){
    int err = 0;
    int len = (int)sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &len) != 0){
        int e = WSAGetLastError();
        return e ? e : -1;
    }
    return err; /* это уже WSA-код ошибки по Winsock, 0 = ok */
}

int net_connect_finished(net_fd_t fd){
    int err = net_get_so_error(fd);
    if (err == 0) return NET_OK;
    return NET_ERR_GENERIC;
}
