#pragma once
#ifndef __EMSCRIPTEN__
#include <stddef.h>
#include <stdint.h>
#include "net.h"
#include "net/conop_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct Cow1Tcp Cow1Tcp;

    typedef void (*Cow1TcpOnOp)(void* user,
                                const ConOp* op,
                                const char* tag,
                                const void* data, size_t data_len,
                                const void* init, size_t init_len);

    /* Обёртка над неблокирующим fd: читает/пишет COW1 кадры. */
    Cow1Tcp* cow1tcp_create(NetPoller* np, net_fd_t fd, Cow1TcpOnOp on_op, void* user);
    void     cow1tcp_destroy(Cow1Tcp*);

    /* Очередь на отправку одного ConOp (внутри encode → send partial). Возврат 0 — ок. */
    int      cow1tcp_send(Cow1Tcp*, const ConOp* op);

#ifdef __cplusplus
}
#endif
#endif /* !__EMSCRIPTEN__ */
