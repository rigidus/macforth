#pragma once
#include <stdint.h>
#include "replication/repl_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

    /* Ссылка на бэкенд для Hub с приоритетом выбора. */
    typedef struct {
        Replicator* r;
        int         priority; /* больше — предпочтительнее (LEADER>CRDT>LOCAL) */
    } ReplBackendRef;

    /* Создать ReplHub (Replicator совместимый).
       - backends: массив доступных бэкендов (может быть NULL/0 — будет только LOCAL позже);
       - required_caps: битмаска требований (0 — без требований);
       - adopt_backends: если !=0, Hub уничтожит бэкенды в destroy(). */
    Replicator* replicator_create_hub(const ReplBackendRef* backends, int n_backends,
                                      int required_caps, int adopt_backends);

    /**
     * Мягкое переключение топика на другой бэкенд.
     * - Блокирует publish(topик) и буферизует входящие операции.
     * - Если для type_id зарегистрирован snapshot(), берёт снапшот и
     *   публикует его в новый бэкенд как ConOp{ topic=..., console_id=inst_id, init_blob=... }.
     * - Перевешивает listener на новый бэкенд, разблокирует publish
     *   и досылает накопленный буфер по порядку.
     */
    void replhub_switch_backend(Replicator* hub, TopicId topic, int to_backend /* index в backends[] или <0 выбрать политикой */);


    void replhub_set_topic_caps(Replicator* hub, TopicId topic, int required_caps);
    void replhub_force_backend(Replicator* hub, TopicId topic, int backend_index /* -1 снять пин */);

    /* ===== Debug/информация (для UI/консоли) ===== */
    /* Количество известных Hub’у бэкендов. */
    int  replhub_debug_count_backends(Replicator* hub);
    /* Информация по бэкенду idx: priority/caps/health. Любой из out_* может быть NULL. */
    int  replhub_debug_backend_info(Replicator* hub, int idx, int* out_priority, int* out_caps, int* out_health);
    /* Текущий выбранный бэкенд для topic (или -1, если ещё не выбран/нет). */
    int  replhub_debug_route_backend(Replicator* hub, TopicId topic);

    /* Получить «живую» ручку бэкенда по индексу (для отладочных команд). */
    Replicator* replhub_debug_get_backend(Replicator* hub, int idx);

#ifdef __cplusplus
} /* extern "C" */
#endif
