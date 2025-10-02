#ifndef __EMSCRIPTEN__
#include "net/wire_tcp.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t* buf;
    size_t   len, cap;
    size_t   off; /* продвинутая позиция для send() */
    int      want_wr;
} OutQ;

struct Cow1Tcp {
    NetPoller* np;
    net_fd_t   fd;
    Cow1TcpOnOp on_op;
    void*      user;
    Cow1Decoder dec;
    OutQ        out;
};

static int ensure_cap(OutQ* q, size_t need){
    if (q->cap >= need) return 1;
    size_t n = q->cap ? q->cap*2 : 8192;
    while (n < need) n *= 2;
    void* nb = realloc(q->buf, n);
    if (!nb) return 0;
    q->buf = (uint8_t*)nb; q->cap = n; return 1;
}

static int outq_push_frame(OutQ* q, const uint8_t* frame, size_t n){
    if (!ensure_cap(q, q->len + n)) return 0;
    memcpy(q->buf + q->len, frame, n);
    q->len += n;
    q->want_wr = 1;
    return 1;
}

/* platform-neutral nb recv/send */
#if defined(_WIN32)
#  include <winsock2.h>
static int s_recv(net_fd_t fd, void* b, int cap){ return recv(fd, (char*)b, cap, 0); }
static int s_send(net_fd_t fd, const void* b, int n){ return send(fd, (const char*)b, n, 0); }
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <errno.h>
static int s_recv(net_fd_t fd, void* b, int cap){ return (int)recv(fd, b, (size_t)cap, 0); }
static int s_send(net_fd_t fd, const void* b, int n){ return (int)send(fd, b, (size_t)n, 0); }
#endif

static void s_on_fd(void* user, net_fd_t fd, int ev){
    Cow1Tcp* c = (Cow1Tcp*)user; (void)fd;
    if (!c) return;
    if (ev & NET_ERR){
        /* Сигнализируем владельцу вне этого слоя; здесь просто перестанем слушать WR */
        net_poller_mod(c->np, c->fd, NET_RD|NET_ERR);
        return;
    }
    if (ev & NET_RD){
        /* читаем порциями */
        uint8_t tmp[4096];
        for (;;){
            int rc = s_recv(c->fd, tmp, (int)sizeof(tmp));
            if (rc > 0){
                cow1_decoder_consume(&c->dec, tmp, (size_t)rc);
                /* попытаться извлечь все накопленные кадры */
                for (;;){
                    ConOp op; char* tag=NULL; void* data=NULL; size_t dlen=0; void* init=NULL; size_t ilen=0;
                    int k = cow1_decoder_take_next(&c->dec, &op, &tag, &data, &dlen, &init, &ilen);
                    if (k <= 0) break;
                    if (c->on_op) c->on_op(c->user, &op, tag, data, dlen, init, ilen);
                    conop_wire_free_decoded(tag, data, init);
                }
            } else if (rc == 0){
                /* закрыто peer'ом */
                net_poller_mod(c->np, c->fd, NET_ERR); /* перестанем читать */
                break;
            } else {
#if defined(_WIN32)
                int err = WSAGetLastError();
                if (err==WSAEWOULDBLOCK || err==WSAEINPROGRESS || err==WSAEALREADY) break;
#else
                if (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINPROGRESS) break;
#endif
                net_poller_mod(c->np, c->fd, NET_ERR);
                break;
            }
        }
    }
    if (ev & NET_WR){
        while (c->out.want_wr && c->out.off < c->out.len){
            size_t left = c->out.len - c->out.off;
            int to_send = (left > (size_t)INT_MAX) ? INT_MAX : (int)left;
            int rc = s_send(c->fd, c->out.buf + c->out.off, to_send);
            if (rc > 0){
                c->out.off += (size_t)rc;
            } else {
#if defined(_WIN32)
                int err = WSAGetLastError();
                if (err==WSAEWOULDBLOCK || err==WSAEINPROGRESS || err==WSAEALREADY) break;
#else
                if (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINPROGRESS) break;
#endif
                net_poller_mod(c->np, c->fd, NET_ERR);
                return;
            }
        }
        if (c->out.off >= c->out.len){
            c->out.off = c->out.len = 0;
            c->out.want_wr = 0;
        }
    }
    /* обновим интересы по WR в зависимости от очереди */
    int mask = NET_RD | NET_ERR | (c->out.want_wr ? NET_WR : 0);
    net_poller_mod(c->np, c->fd, mask);
}

Cow1Tcp* cow1tcp_create(NetPoller* np, net_fd_t fd, Cow1TcpOnOp on_op, void* user){
    if (!np) return NULL;
    Cow1Tcp* c = (Cow1Tcp*)calloc(1, sizeof(Cow1Tcp));
    if (!c) return NULL;
    c->np = np; c->fd = fd; c->on_op = on_op; c->user = user;
    cow1_decoder_init(&c->dec);
    c->out.buf = NULL; c->out.len = c->out.cap = c->out.off = 0; c->out.want_wr = 0;
    net_poller_add(np, fd, NET_RD | NET_ERR, s_on_fd, c);
    return c;
}

void cow1tcp_destroy(Cow1Tcp* c){
    if (!c) return;
    net_poller_del(c->np, c->fd);
    cow1_decoder_reset(&c->dec);
    free(c->out.buf);
    free(c);
}

int cow1tcp_send(Cow1Tcp* c, const ConOp* op){
    if (!c || !op) return -1;
    uint8_t* buf = NULL; size_t len = 0;
    if (conop_wire_encode(op, &buf, &len) != 0) return -1;
    int ok = outq_push_frame(&c->out, buf, len);
    free(buf);
    if (!ok) return -1;
    /* Попросим WR-интересы у поллера */
    net_poller_mod(c->np, c->fd, NET_RD | NET_WR | NET_ERR);
    return 0;
}
#endif /* !__EMSCRIPTEN__ */
