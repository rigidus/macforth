#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* Важно: ConOp здесь «неизвестный» тип, чтобы не тянуть внутренних деталей.
       Любой модуль, который вызывает apply(), уже знает, что такое ConOp. */
    typedef struct ConOp ConOp;

    typedef struct TypeVt {
        const char* name; /* человекочитаемое имя типа (диагностика) */
        /* Инициализация состояния из снапшота (может быть NULL).
           schema — версия формата blob’а. */
        void (*init_from_blob)(void* user, uint32_t schema, const void* blob, size_t len);
        /* Применение операции (CRDT/OT/…); обязателен. */
        void (*apply)(void* user, const ConOp* op);
           /* Сериализация состояния в снапшот (может быть NULL).
           Контракт: компонент отдаёт «blob+schema», а место,
           где снапшот нужен (Hub/mesh/leader/client), само упакует его в ConOp:
           - op.topic = topic;
           - op.console_id = topic.inst_id;
           - op.schema = out_schema;
           - op.init_blob/op.init_size = blob/len;
           - опционально op.tag = "snapshot";
           - op.op_id == 0 как маркер «это снапшот».
           - возвращает 0 — успех, !=0 — «нет снапшота/ошибка»;
           - заполняет out_schema (версия формата) и out_blob/out_len;
           - память под out_blob выделяет сам компонент через malloc(),
           освобождает вызывающий через free(). */
        int  (*snapshot)(void* user, uint32_t* out_schema, void** out_blob, size_t* out_len);
    } TypeVt;

    typedef struct TypeRegistry TypeRegistry;

    /* Дефолтный (глобальный) реестр. */
    TypeRegistry* type_registry_default(void);
    void          type_registry_reset(TypeRegistry*);

    /* Регистрация/поиск. user — произвольный указатель компонента/контекста. */
    int  type_registry_register(TypeRegistry*, uint64_t type_id, const TypeVt* vt, void* user);
    /* Возвращает vt и user, или NULL если не найден. */
    const TypeVt* type_registry_get(TypeRegistry*, uint64_t type_id, void** out_user);

    /* Удобные обёртки над дефолтным реестром. */
    static inline int  type_registry_register_default(uint64_t type_id, const TypeVt* vt, void* user){
        return type_registry_register(type_registry_default(), type_id, vt, user);
    }
    static inline const TypeVt* type_registry_get_default(uint64_t type_id, void** out_user){
        return type_registry_get(type_registry_default(), type_id, out_user);
    }

#ifdef __cplusplus
} /* extern "C" */
#endif
