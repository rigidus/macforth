#pragma once
#include <stddef.h>
#include <stdint.h>
#include "console/widget.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Общие лимиты */

#ifndef CON_MAX_LINE
#define CON_MAX_LINE   4096
#endif

#ifndef CON_BUF_LINES
#define CON_BUF_LINES  1024
#endif

    /*  стабильные ID элементов (для адресации в сети/CRDT) */
    typedef uint64_t ConItemId;

#   define CON_ITEMID_INVALID ((ConItemId)0)

    typedef struct ConsoleStore ConsoleStore;
    typedef void (*ConsoleStoreListener)(void* user);

    /* Создание/удаление */
    ConsoleStore* con_store_create(void);
    void          con_store_destroy(ConsoleStore*);

    /* Подписка на изменения состояния (простая, без снятия подписи) */
    void con_store_subscribe(ConsoleStore*, ConsoleStoreListener cb, void* user);

    /* Операции редактирования (одна общая «редактируемая строка» М1) */
    void con_store_put_text(ConsoleStore*, const char* utf8); /* добавляет байты в edit */
    void con_store_backspace(ConsoleStore*);                  /* удаляет 1 символ с конца (если есть) */
    void con_store_commit(ConsoleStore*);                     /* переносит edit в историю */
    void con_store_append_line(ConsoleStore*, const char* s); /* добавляет готовую строку в историю */
    /* возвращаем присвоенный ID вставленного элемента */
    ConItemId con_store_append_widget(ConsoleStore*, ConsoleWidget* w);   /* вставляет виджет в историю */

    void con_store_notify_changed(ConsoleStore*); /* оповестить слушателей об изменениях состояния */

    /* Доступ для отрисовки */
    int         con_store_count(const ConsoleStore*);               /* кол-во строк в истории */
    const char* con_store_get_line(const ConsoleStore*, int index); /* index: 0..count-1 (0 — самая старая) */
    int         con_store_get_line_len(const ConsoleStore*, int index);
    int         con_store_get_edit(const ConsoleStore*, char* out, int cap); /* копирует текущий edit */
    ConsoleWidget*  con_store_get_widget(const ConsoleStore*, int index); /* NULL если не виджет */
    /* ID ↔ индекс */
    ConItemId       con_store_get_id(const ConsoleStore*, int index);
    int             con_store_find_index_by_id(const ConsoleStore*, ConItemId id); /* -1 если нет */
    /* М4: адресные сообщения к виджетам по ID. Возвращает 1, если виджет изменён. */
    int             con_store_widget_message(ConsoleStore*, ConItemId id, const char* tag, const void* data, size_t size);

#ifdef __cplusplus
}
#endif
