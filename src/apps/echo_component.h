#pragma once
#include <stdint.h>
#include "net/net.h"
#include "console/sink.h"

#ifdef __cplusplus
extern "C" {
#endif

    /* Эхо-компонент: лидер (TCP listen/accept/echo) и клиент (connect/печатает ответы).
       Неблокирующий, ограничивает объём IO за одно срабатывание. */

    typedef struct Echo Echo;

    /* Владелец поллера — main; Echo только регистрирует fd. */
    Echo* echo_create(NetPoller* np, ConsoleSink* sink);
    void  echo_destroy(Echo* e);

    /* Запуск режимов. Возврат 0 — успех. */
    int   echo_start_leader(Echo* e, uint16_t port);
    int   echo_start_client(Echo* e, const char* ip, uint16_t port);

#ifdef __cplusplus
}
#endif
