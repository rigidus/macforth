#pragma once
#include <stddef.h>
#include <stdint.h>
#include "console/widget.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Максимум «известных» изменённых элементов, которые можно слить за один кадр.
   Если изменений больше или менялась структура (вставки/удаления/пересортировка) — придёт флаг all. */
#ifndef CON_STORE_CHANGES_MAX
#define CON_STORE_CHANGES_MAX 64
#endif

/* Общие лимиты */

#ifndef CON_MAX_LINE
#define CON_MAX_LINE   4096
#endif

#ifndef CON_BUF_LINES
#define CON_BUF_LINES  1024
#endif

#define CON_POS_MAX_DEPTH 8

/* Лимит пользователей для промптов/индикаторов (держим синхронно с WM_MAX_USERS) */
#ifndef CON_MAX_USERS
#define CON_MAX_USERS 8
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

    /* Тип записи ленты */
    typedef enum {
        CON_ENTRY_TEXT   = 1,
        CON_ENTRY_WIDGET = 2,
        CON_ENTRY_SNAPSHOT = 3   /* агрегат «свёртки хвоста» */
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

    /* ==== CRDT-вставки с указанием автора (user_id) ==== */
    /* Идемпотентные вставки по позиции с заданным ID: */
    ConItemId       con_store_insert_text_at(ConsoleStore*, ConItemId forced_id, const ConPosId* pos, const char* s, int user_id);
    ConItemId       con_store_insert_widget_at(ConsoleStore*, ConItemId forced_id, const ConPosId* pos, ConsoleWidget* w, int user_id);

    /* Геттер автора записи (user_id, либо -1 если системная/без автора) по видимому индексу. */
    int             con_store_get_user(const ConsoleStore*, int index);

    /* ====== Извещение об изменении для инкрементальной перерисовки ======
       После получения коллбэка subscribe(), вьюха может «слить» последние изменения.
       Возвращает количество скопированных id (<= cap). Если *out_all_flag != 0 — изменилась
       структура (вставки/компакция/пересортировка), и безопаснее перерисовать всё окно. */
    int  con_store_drain_changes(ConsoleStore*, ConItemId* out_ids, int cap, int* out_all_flag);

    /* ===== Снапшоты =====
       Для типа CON_ENTRY_SNAPSHOT можно запросить число свёрнутых строк.
       Вернёт >=0 (кол-во свёрнутых строк) либо -1, если по индексу не снапшот. */
    int  con_store_get_snapshot_dropped(const ConsoleStore*, int index);
    /* Вспомогательное: последний видимый ID (0, если пусто). */

    /* ====== Состояние промптов - хранится в Store ======
       - Локальный узел держит ПОЛНЫЙ текст каждого user_id, но UI других вьюх не показывает его.
       - Сеть/репликация передаёт только метаданные: edits++ и nonempty (1/0). */

    /* Локальные правки буфера промпта (обновляют edits при bump_counter!=0) */
    void  con_store_prompt_insert(ConsoleStore*, int user_id, const char* utf8, int bump_counter);
    void  con_store_prompt_backspace(ConsoleStore*, int user_id, int bump_counter);
    /* Забрать строку и очистить буфер. Возвращает кол-во байт скопировано. */
    int   con_store_prompt_take(ConsoleStore*, int user_id, char* out, int cap);
    /* Подглядеть текущее содержимое (для своего user_id, UI решает, что показывать) */
    int   con_store_prompt_peek(const ConsoleStore*, int user_id, char* out, int cap);
    /* Текущая длина буфера */
    int   con_store_prompt_len(const ConsoleStore*, int user_id);
    /* Метаданные для UI (для любого user_id): nonempty (0/1) и счётчик правок */
    void  con_store_prompt_get_meta(const ConsoleStore*, int user_id, int* out_nonempty, int* out_edits);
    /* Применить «удалённую» метку (репликация): увеличить edits на inc и установить nonempty */
    void  con_store_prompt_apply_meta(ConsoleStore*, int user_id, int edits_inc, int nonempty_flag);

#ifdef __cplusplus
}
#endif
