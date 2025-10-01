#include "apps/echo_component.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

    /* ===== Параметры неблокирующих обработчиков ===== */
#ifndef ECHO_ACCEPT_BUDGET
#  define ECHO_ACCEPT_BUDGET 16
#endif
#ifndef ECHO_READ_BUDGET
#  define ECHO_READ_BUDGET (32*1024)
#endif
#ifndef ECHO_WRITE_BUDGET
#  define ECHO_WRITE_BUDGET (32*1024)
#endif

typedef struct EchoConn EchoConn;
struct EchoConn {
    struct Echo* echo;
    net_fd_t fd;
    int want_wr;           /* есть данные для записи */
    size_t out_off, out_len;
    char   out_buf[64*1024];
    int is_client;         /* 1 — наш исходящий клиент; печатаем вход в консоль */
    int connecting;        /* 1 — ещё завершаем connect() */
};

struct Echo {
    NetPoller*   np;
    ConsoleSink* sink;
    net_fd_t     listen_fd;
    int          listening;
    EchoConn**   conns;
    int          nconns, cap;
};

static void s_log(ConsoleSink* sink, const char* s){
    if (sink && s) con_sink_append_line(sink, -1, s);
}

static void s_conn_free(Echo* e, EchoConn* c){
    if (!e || !c) return;
    net_poller_del(e->np, c->fd);
    net_close_fd(c->fd);
    free(c);
}

static void s_conn_detach(Echo* e, EchoConn* c){
    if (!e || !c) return;
    for (int i=0;i<e->nconns;i++){
        if (e->conns[i] == c){
            for (int j=i+1;j<e->nconns;j++) e->conns[j-1]=e->conns[j];
            e->nconns--; break;
        }
    }
}

static void s_conn_close(Echo* e, EchoConn* c){
    s_conn_detach(e, c);
    s_conn_free(e, c);
}

static void s_set_interest(Echo* e, EchoConn* c, int rd, int wr){
    int mask = 0;
    if (rd) mask |= NET_RD;
    if (wr) mask |= NET_WR;
    mask |= NET_ERR;
    net_poller_add(e->np, c->fd, mask, /*cb set ниже*/ NULL, NULL); /* будем перезаписывать cb отдельно */
}

/* ---- IO helpers ---- */
static int s_nb_recv(net_fd_t fd, char* buf, int cap){
#if defined(_WIN32)
    int rc = recv(fd, buf, cap, 0);
#else
    int rc = (int)recv(fd, buf, (size_t)cap, 0);
#endif
    return rc;
}
static int s_nb_send(net_fd_t fd, const char* buf, int n){
#if defined(_WIN32)
    int rc = send(fd, buf, n, 0);
#else
    int rc = (int)send(fd, buf, (size_t)n, 0);
#endif
    return rc;
}

/* ====== callbacks ====== */
static void s_on_conn(void* user, net_fd_t fd, int ev);
static void s_on_listen(void* user, net_fd_t fd, int ev);
static void s_on_client_connecting(void* user, net_fd_t fd, int ev);

static void s_enable_cb(Echo* e, EchoConn* c, NetFdCb cb, int mask){
    net_poller_add(e->np, c->fd, mask|NET_ERR, cb, c);
}

static void s_on_conn(void* user, net_fd_t fd, int ev){
    (void)fd;
    EchoConn* c = (EchoConn*)user; if (!c || !c->echo) return;
    Echo* e = c->echo;
    if (ev & NET_ERR){
        s_conn_close(e, c);
        return;
    }
    if (ev & NET_RD){
        int budget = ECHO_READ_BUDGET;
        char buf[4096];
        for (;;){
            if (budget <= 0) break;
            int chunk = (budget > (int)sizeof(buf)) ? (int)sizeof(buf) : budget;
            int rc = s_nb_recv(c->fd, buf, chunk);
            if (rc > 0){
                budget -= rc;
                /* сервер эхо: отдать назад; клиент: печатать входящие */
                if (c->is_client){
                    /* печатаем как есть (одной строкой на пакет) */
                    char line[4608];
                    int n = (rc >= (int)sizeof(line)-7)? (int)sizeof(line)-7 : rc;
                    memcpy(line, buf, n); line[n]=0;
                    char head[48]; snprintf(head,sizeof(head),"echo: ");
                    char out[4680]; snprintf(out,sizeof(out),"%s%s", head, line);
                    s_log(e->sink, out);
                } else {
                    /* сервер: помещаем в out-буфер для записи */
                    if (c->out_len + (size_t)rc <= sizeof(c->out_buf)){
                        memcpy(c->out_buf + c->out_len, buf, rc);
                        c->out_len += (size_t)rc;
                        c->want_wr = 1;
                    }
                }
            } else if (rc == 0){
                /* close by peer */
                s_conn_close(e, c);
                return;
            } else {
                /* rc < 0 */
#if defined(_WIN32)
                int err = WSAGetLastError();
#else
                int err = errno;
#endif
                if (net_err_would_block(err)) break;
                s_conn_close(e, c);
                return;
            }
        }
    }
    if (ev & NET_WR){
        int budget = ECHO_WRITE_BUDGET;
        while (c->out_len > 0 && budget > 0){
            int to_send = (int)c->out_len;
            if (to_send > budget) to_send = budget;
            int rc = s_nb_send(c->fd, c->out_buf + c->out_off, to_send);
            if (rc > 0){
                c->out_off += (size_t)rc;
                c->out_len -= (size_t)rc;
                budget -= rc;
                if (c->out_len == 0){ c->out_off = 0; c->want_wr = 0; break; }
            } else {
#if defined(_WIN32)
                int err = WSAGetLastError();
#else
                int err = errno;
#endif
                if (net_err_would_block(err)) break;
                s_conn_close(e, c);
                return;
            }
        }
    }
    /* обновить интересы по WR */
    int mask = NET_RD | (c->want_wr ? NET_WR : 0) | NET_ERR;
    net_poller_add(e->np, c->fd, mask, s_on_conn, c);
}

static void s_on_listen(void* user, net_fd_t fd, int ev){
    Echo* e = (Echo*)user; if (!e) return;
    if (ev & NET_ERR) return;
    if (!(ev & NET_RD)) return;
    int acc = ECHO_ACCEPT_BUDGET;
    while (acc-- > 0){
        struct sockaddr_in ra; socklen_t rl = (socklen_t)sizeof(ra);
#if defined(_WIN32)
        SOCKET cfd = accept(fd, (struct sockaddr*)&ra, &rl);
        if (cfd == INVALID_SOCKET){
            int err = WSAGetLastError();
            if (net_err_would_block(err)) break;
            break;
        }
#else
        int cfd = accept(fd, (struct sockaddr*)&ra, &rl);
        if (cfd < 0){
            if (net_err_would_block(errno)) break;
            break;
        }
#endif
        net_set_nonblocking((net_fd_t)cfd, 1);
        /* аллоцируем соединение */
        EchoConn* c = (EchoConn*)calloc(1, sizeof(EchoConn));
        c->echo = e; c->fd = (net_fd_t)cfd; c->is_client = 0; c->connecting = 0;
        /* добавить в список */
        if (e->nconns == e->cap){
            int ncap = e->cap? e->cap*2 : 8;
            e->conns = (EchoConn**)realloc(e->conns, ncap*sizeof(EchoConn*));
            e->cap = ncap;
        }
        e->conns[e->nconns++] = c;
        /* подписать в поллер */
        net_poller_add(e->np, c->fd, NET_RD|NET_ERR, s_on_conn, c);
    }
}

static void s_on_client_connecting(void* user, net_fd_t fd, int ev){
    EchoConn* c = (EchoConn*)user; if (!c || !c->echo) return;
    Echo* e = c->echo;
    if (ev & NET_ERR){ s_conn_close(e, c); return; }
    if (!(ev & NET_WR)) return;
    /* Проверяем завершение connect() */
    int ok = net_connect_finished(fd);
    if (ok == NET_OK){
        c->connecting = 0;
        /* привет → в буфер */
        const char* hi = "hello from client\n";
        size_t n = strlen(hi);
        if (n <= sizeof(c->out_buf)){
            memcpy(c->out_buf, hi, n);
            c->out_off = 0; c->out_len = n; c->want_wr = 1;
        }
        c->is_client = 1;
        /* сменить callback на обычный обработчик соединения */
        int mask = NET_RD | NET_WR | NET_ERR;
        net_poller_add(e->np, c->fd, mask, s_on_conn, c);
        s_log(e->sink, "net: client connected");
    } else {
        s_conn_close(e, c);
    }
}

/* ===== API ===== */
Echo* echo_create(NetPoller* np, ConsoleSink* sink){
    if (!np) return NULL;
    Echo* e = (Echo*)calloc(1, sizeof(Echo));
    e->np = np; e->sink = sink; e->listen_fd = (net_fd_t)-1; e->listening = 0;
    return e;
}

void echo_destroy(Echo* e){
    if (!e) return;
    /* закрыть все соединения */
    for (int i=0;i<e->nconns;i++){
        if (e->conns[i]) s_conn_free(e, e->conns[i]);
    }
    free(e->conns);
    e->conns=NULL; e->nconns=e->cap=0;
    /* закрыть listen */
    if (e->listening){
        net_poller_del(e->np, e->listen_fd);
        net_close_fd(e->listen_fd);
        e->listening = 0;
    }
    free(e);
}

int echo_start_leader(Echo* e, uint16_t port){
    if (!e || !e->np) return -1;
    if (e->listening) return 0;
    net_fd_t s = net_socket_tcp();
    if ((intptr_t)s < 0){ s_log(e->sink,"net: socket() failed"); return -1; }
    /* SO_REUSEADDR */
    int yes=1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    struct sockaddr_in a; memset(&a,0,sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0){
        net_close_fd(s); s_log(e->sink, "net: bind failed"); return -1;
    }
    if (listen(s, 64) != 0){
        net_close_fd(s); s_log(e->sink, "net: listen failed"); return -1;
    }
    net_set_nonblocking(s, 1);
    e->listen_fd = s; e->listening = 1;
    net_poller_add(e->np, e->listen_fd, NET_RD|NET_ERR, s_on_listen, e);
    return 0;
}

int echo_start_client(Echo* e, const char* ip, uint16_t port){
    if (!e || !e->np || !ip || !*ip) return -1;
    net_fd_t s = net_socket_tcp();
    if ((intptr_t)s < 0){ s_log(e->sink,"net: socket() failed"); return -1; }
    net_set_nonblocking(s, 1);
    struct sockaddr_in a; memset(&a,0,sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1){
        net_close_fd(s); s_log(e->sink,"net: bad ip (use IPv4 like 127.0.0.1)"); return -1;
    }
    int rc = net_connect_nb(s, (struct sockaddr*)&a, (int)sizeof(a));
    EchoConn* c = (EchoConn*)calloc(1, sizeof(EchoConn));
    c->echo = e; c->fd = s; c->connecting = (rc == NET_INPROGRESS); c->is_client = 1;
    /* добавить в список (чтобы закрыть на destroy) */
    if (e->nconns == e->cap){ int ncap=e->cap?e->cap*2:8; e->conns=(EchoConn**)realloc(e->conns,ncap*sizeof(EchoConn*)); e->cap=ncap; }
    e->conns[e->nconns++] = c;
    if (rc == NET_OK){
        /* готово сразу */
        const char* hi="hello from client\n"; size_t n=strlen(hi);
        memcpy(c->out_buf, hi, n); c->out_off=0; c->out_len=n; c->want_wr=1;
        net_poller_add(e->np, c->fd, NET_RD|NET_WR|NET_ERR, s_on_conn, c);
        s_log(e->sink, "net: client connected");
        return 0;
    } else if (rc == NET_INPROGRESS){
        net_poller_add(e->np, c->fd, NET_WR|NET_ERR, s_on_client_connecting, c);
        return 0;
    }
    /* error */
    s_conn_close(e, c);
    s_log(e->sink, "net: connect failed");
    return -1;
}
