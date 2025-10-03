// src/apps/console_processor_ext_stubs.c
#include <stddef.h>
#include <stdint.h>
#include "console/processor.h"
#include "console/store.h"

/* Оpaque-тип для сигнатуры фабрики виджетов */
typedef struct ConsoleWidget ConsoleWidget;

/* Заглушка: пока хранилище недоступно */
ConsoleStore* con_processor_get_store(ConsoleProcessor* self) {
    (void)self;
    return NULL;
}

/* Заглушка сериализации: возвращаем "не получилось" */
int con_store_serialize(ConsoleStore* st, void** out_blob, size_t* out_len) {
    (void)st;
    if (out_blob) *out_blob = NULL;
    if (out_len)  *out_len  = 0;
    return 0; // failure
}

/* Заглушка фабрики виджетов: виджет не создаётся */
ConsoleWidget* con_ext_make_widget(uint32_t kind, const void* init_blob, size_t init_size) {
    (void)kind; (void)init_blob; (void)init_size;
    return NULL;
}
