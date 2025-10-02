// === file: src/net/net_posix.c ===
#include "net.h"
#include "wire_tcp.h" /* header присутствует, но сам модуль используется опционально */
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <alloca.h>
/* сокеты/опции/адреса */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>

typedef struct NetEntry {
    net_fd_t fd;
    int      mask;
    NetFdCb  cb;
    void*    user;
    int      alive;
} NetEntry;

typedef enum { OP_ADD, OP_MOD, OP_DEL } OpKind;

typedef struct PendingOp {
    OpKind  kind;
    net_fd_t fd;
    int     mask;
    NetFdCb cb;
    void*   user;
} PendingOp;

struct NetPoller {
    NetEntry*  entries; size_t len, cap;
    PendingOp* ops;     size_t olen, ocap;
    int        in_tick;
};

static NetEntry* s_find(struct NetPoller* np, net_fd_t fd){
    for (size_t i=0;i<np->len;i++) if (np->entries[i].alive && np->entries[i].fd==fd) return &np->entries[i];
    return NULL;
}
static void s_push_op(struct NetPoller* np, PendingOp op){
    if (np->olen==np->ocap){ size_t n=np->ocap?np->ocap*2:8; np->ops=(PendingOp*)realloc(np->ops,n*sizeof(PendingOp)); np->ocap=n; }
    np->ops[np->olen++] = op;
}
static void s_apply_ops(struct NetPoller* np){
    for (size_t i=0;i<np->olen;i++){
        PendingOp* op = &np->ops[i];
        if (op->kind==OP_ADD){
            NetEntry* e = s_find(np, op->fd);
            if (!e){
                if (np->len==np->cap){ size_t n=np->cap?np->cap*2:16; np->entries=(NetEntry*)realloc(np->entries,n*sizeof(NetEntry)); np->cap=n; }
                NetEntry ne; ne.fd=op->fd; ne.mask=op->mask; ne.cb=op->cb; ne.user=op->user; ne.alive=1;
                np->entries[np->len++] = ne;
            } else {
                e->mask=op->mask; e->cb=op->cb; e->user=op->user; e->alive=1;
            }
        } else if (op->kind==OP_MOD){
            NetEntry* e = s_find(np, op->fd); if (e) e->mask = op->mask;
        } else { /* OP_DEL */
            NetEntry* e = s_find(np, op->fd); if (e) e->alive = 0;
        }
    }
    /* compact */
    size_t w=0;
    for (size_t i=0;i<np->len;i++){ if (np->entries[i].alive){ if (w!=i) np->entries[w]=np->entries[i]; w++; } }
    np->len = w;
    np->olen = 0;
}

NetPoller* net_poller_create(void){
    struct NetPoller* np = (struct NetPoller*)calloc(1,sizeof(*np));
    return np;
}
void net_poller_destroy(NetPoller* np){
    if (!np) return;
    free(np->entries);
    free(np->ops);
    free(np);
}

int net_poller_add(NetPoller* np, net_fd_t fd, int mask, NetFdCb cb, void* user){
    if (!np || !cb) return -1;
    PendingOp op = (PendingOp){ OP_ADD, fd, mask, cb, user };
    if (np->in_tick) s_push_op(np, op); else { s_push_op(np, op); s_apply_ops(np); }
    return 0;
}
void net_poller_mod(NetPoller* np, net_fd_t fd, int new_mask){
    if (!np) return;
    PendingOp op = (PendingOp){ OP_MOD, fd, new_mask, 0, 0 };
    if (np->in_tick) s_push_op(np, op); else { s_push_op(np, op); s_apply_ops(np); }
}
void net_poller_del(NetPoller* np, net_fd_t fd){
    if (!np) return;
    PendingOp op = (PendingOp){ OP_DEL, fd, 0, 0, 0 };
    if (np->in_tick) s_push_op(np, op); else { s_push_op(np, op); s_apply_ops(np); }
}

static short to_poll_events(int mask){
    short ev = 0;
    if (mask & NET_RD) ev |= POLLIN;
    if (mask & NET_WR) ev |= POLLOUT;
    return ev;
}
static int from_poll_revents(short rev){
    int ev = 0;
    if (rev & (POLLIN|POLLPRI)) ev |= NET_RD;
    if (rev & POLLOUT) ev |= NET_WR;
    if (rev & (POLLERR|POLLHUP|POLLNVAL)) ev |= NET_ERR;
    return ev;
}

void net_poller_tick(NetPoller* np, uint32_t now_ms, int budget_ms){
    (void)now_ms;
    if (!np) return;
    if (np->olen) s_apply_ops(np);
    if (np->len == 0) return;

    struct pollfd* pfds = (struct pollfd*)alloca(np->len * sizeof(struct pollfd));
    for (size_t i=0;i<np->len;i++){
        pfds[i].fd = np->entries[i].fd;
        pfds[i].events = to_poll_events(np->entries[i].mask);
        pfds[i].revents = 0;
    }
    int timeout = (budget_ms > 0) ? budget_ms : 0; /* неблокирующий по умолчанию */
    np->in_tick = 1;
    int rc = poll(pfds, (nfds_t)np->len, timeout);
    if (rc > 0){
        for (size_t i=0;i<np->len;i++){
            if (!pfds[i].revents) continue;
            int ev = from_poll_revents(pfds[i].revents);
            NetEntry* e = &np->entries[i];
            if (e->cb) e->cb(e->user, e->fd, ev);
        }
    }
    np->in_tick = 0;
    if (np->olen) s_apply_ops(np);
}

int net_set_nonblocking(net_fd_t fd, int nonblocking){
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    if (nonblocking) return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    else return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}


int net_get_so_error(net_fd_t fd){
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return errno; /* не смогли прочитать — вернём текущий errno */
    }
    return err; /* 0 == успех */
}

/* ==================== неблокирующие сокеты/коннект ==================== */

net_fd_t net_socket_tcp(void){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    return (fd < 0) ? (net_fd_t)-1 : (net_fd_t)fd;
}

int net_close_fd(net_fd_t fd){
    return close(fd);
}

int net_last_error(void){
    return errno;
}

int net_err_would_block(int err){
    return (err==EAGAIN) || (err==EWOULDBLOCK) || (err==EINPROGRESS);
}

/* Старт неблокирующего connect(). Предполагается, что сокет уже O_NONBLOCK. */
int net_connect_nb(net_fd_t fd, const struct sockaddr* addr, int addrlen){
    if (fd < 0 || !addr || addrlen<=0) return NET_ERR_GENERIC;
    int rc = connect(fd, addr, (socklen_t)addrlen);
    if (rc == 0) return NET_OK;
    int e = errno;
    if (e == EINPROGRESS || e == EWOULDBLOCK) return NET_INPROGRESS;
    return NET_ERR_GENERIC;
}

int net_connect_finished(net_fd_t fd){
    int err = net_get_so_error(fd);
    if (err == 0) return NET_OK;
    return NET_ERR_GENERIC;
}
