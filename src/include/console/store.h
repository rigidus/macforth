#pragma once
#include <stddef.h>
#include <stdint.h>
#include "console/widget.h"

#define CON_POS_MAX_DEPTH 8

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


    /* ===== CRDT позиция (Logoot/LSEQ-подобная) =====
       Идентификатор позиции — лексикографический список компонентов (digit,actor).
       depth — фактическая длина (<= CON_POS_MAX_DEPTH). */
    typedef struct {
        uint8_t depth;
        struct {
            uint16_t digit;   /* 0..65535 */
            uint32_t actor;   /* кто «поставил» компонент */
        } comp[CON_POS_MAX_DEPTH];
    } ConPosId;

    /* Сравнение позиций: <0 если a<b, >0 если a>b, 0 если равны. */
    int  con_pos_cmp(const ConPosId* a, const ConPosId* b);

    /* Сгенерировать позицию между элементами с ID left/right (0 — отсутствие),
       используя actor_id эмитента. Детали: расширяет глубину при нехватке «места». */
    ConPosId con_store_gen_between(const ConsoleStore*, ConItemId left, ConItemId right, uint32_t actor_id);

    /* Вставки по заданной позиции/ID (идемпотентно: если id уже существует — игнор). */
    ConItemId con_store_insert_text_at(ConsoleStore*, ConItemId forced_id, const ConPosId* pos, const char* s);
    ConItemId con_store_insert_widget_at(ConsoleStore*, ConItemId forced_id, const ConPosId* pos, ConsoleWidget* w);


    /* Тип записи ленты */
    typedef enum {
        CON_ENTRY_TEXT   = 1,
        CON_ENTRY_WIDGET = 2
    } ConEntryType;

    /* Создание/удаление */
    ConsoleStore* con_store_create(void);
    void          con_store_destroy(ConsoleStore*);

    /* Подписка на изменения состояния (простая, без снятия подписи) */
    void con_store_subscribe(ConsoleStore*, ConsoleStoreListener cb, void* user);

    /* Операции редактирования */
    void con_store_append_line(ConsoleStore*, const char* s); /* добавляет готовую строку в историю */
    /* возвращаем присвоенный ID вставленного элемента */
    ConItemId con_store_append_widget(ConsoleStore*, ConsoleWidget* w);   /* вставляет виджет в историю */

    void con_store_notify_changed(ConsoleStore*); /* оповестить слушателей об изменениях состояния */

    /* Доступ для отрисовки */
    int         con_store_count(const ConsoleStore*);               /* кол-во строк в истории */
    const char* con_store_get_line(const ConsoleStore*, int index); /* index: 0..count-1 (0 — самая старая) */
    int         con_store_get_line_len(const ConsoleStore*, int index);
    ConsoleWidget*  con_store_get_widget(const ConsoleStore*, int index); /* NULL если не виджет */
    /* Тип записи (TEXT/WIDGET) — по видимому индексу */
    ConEntryType    con_store_get_type(const ConsoleStore*, int index);
    /* ID <-> индекс */
    ConItemId       con_store_get_id(const ConsoleStore*, int index);
    int             con_store_find_index_by_id(const ConsoleStore*, ConItemId id); /* -1 если нет */
    /* М4: адресные сообщения к виджетам по ID. Возвращает 1, если виджет изменён. */
    int             con_store_widget_message(ConsoleStore*, ConItemId id, const char* tag, const void* data, size_t size);
    /* (Опционально) Вставка текстовой строки между двумя элементами (по их ID).
       Если left==0 — вставка в начало; если right==0 — в конец.
       Возвращает присвоенный ID либо CON_ITEMID_INVALID. */
    ConItemId       con_store_insert_text_between(ConsoleStore*, ConItemId left, ConItemId right, const char* s);
    /* Вспомогательное: получить последний видимый ID (0, если пусто). */
    ConItemId       con_store_last_id(const ConsoleStore*);


#ifdef __cplusplus
}
#endif
