#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <inttypes.h>

#include "console/processor.h"
#include "console/store.h"   /* прототипы con_store_* и тип ConsoleStore */
#include "console/sink.h"

#include "replication/hub.h"
#include "replication/repl_types.h"
#include "replication/backends/client_tcp.h"
#include "replication/backends/crdt_mesh.h"

/* forward for widget pointer type (opaque is fine) */
typedef struct ConsoleWidget ConsoleWidget;

#if defined(_WIN32) && !defined(__MINGW32__)
#  define strtok_r(s,delim,saveptr) strtok_s((s),(delim),(saveptr))
#endif

/* --- маленькие утилиты вывода/парсинга --- */
static void out_line(ConsoleProcessor* p, const char* s){
    ConsoleSink* sink = con_processor_get_sink(p);
    if (sink && s) con_sink_append_line(sink, -1 /*system*/, s);
}
static void outf(ConsoleProcessor* p, const char* fmt, ...){
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out_line(p, buf);
}
static int split_tokens(char* s, char** argv, int maxv){
    int n=0; char* save=NULL;
    for (char* t = strtok_r(s, " \t\r\n", &save); t && n<maxv; t=strtok_r(NULL," \t\r\n",&save)) argv[n++]=t;
    return n;
}
static const char* caps_str(int caps, char* out, size_t cap){
    /* буквы: O=ORDERED, R=RELIABLE, B=BROADCAST, C=CRDT, D=DISCOVER */
    char tmp[16]={0}; int k=0;
    if (caps & REPL_ORDERED)   tmp[k++]='O';
    if (caps & REPL_RELIABLE)  tmp[k++]='R';
    if (caps & REPL_BROADCAST) tmp[k++]='B';
    if (caps & REPL_CRDT)      tmp[k++]='C';
    if (caps & REPL_DISCOVER)  tmp[k++]='D';
    tmp[k]=0;
    snprintf(out, cap, "%s(0x%X)", tmp[0]?tmp:"-", caps);
    return out;
}

/* --- поиск бэкендов определённого «вида» --- */
static int find_client_backend_idx(Replicator* hub){
    /* client_tcp: OR|REL (без BROADCAST/CRDT) */
    int n = replhub_debug_count_backends(hub);
    for (int i=0;i<n;i++){
        int pr=0, caps=0, hl=0;
        if (replhub_debug_backend_info(hub, i, &pr, &caps, &hl)==0){
            int want = (REPL_ORDERED|REPL_RELIABLE);
            if ((caps & want)==want && !(caps & (REPL_BROADCAST|REPL_CRDT))) return i;
        }
    }
    return -1;
}

static int find_crdt_backend_idx(Replicator* hub){
    int n = replhub_debug_count_backends(hub);
    for (int i=0;i<n;i++){
        int pr=0, caps=0, hl=0;
        if (replhub_debug_backend_info(hub, i, &pr, &caps, &hl)==0){
            if (caps & REPL_CRDT) return i;
        }
    }
    return -1;
}



int con_processor_ext_try_handle(ConsoleProcessor* proc, const char* line_utf8) {
    if (!proc || !line_utf8) return 0;
    /* Короткая справка */
    if (strcmp(line_utf8, "help repl")==0){
        out_line(proc, "replication:");
        out_line(proc, "  hub backends                      — список бэкендов (idx/priority/caps/health)");
        out_line(proc, "  hub route [type inst]             — показать текущий бэкенд для топика");
        out_line(proc, "  hub force <idx|-1>                — закрепить/снять пин для console/<CONSOLE_ID>");
        out_line(proc, "  hub require <caps-bitmask>        — требуемые возможности для console/<CONSOLE_ID>");
        out_line(proc, "  hub switch [<to_idx>]             — мягкое переключение (snapshot+буфер)");
        out_line(proc, "  net client connect <host> <port>  — подключиться к лидеру");
        out_line(proc, "  net client disconnect             — разорвать подключение");
        out_line(proc, "  net client stat                   — состояние клиента");
        out_line(proc, "  mesh seed add <host>              — добавить seed (CRDT mesh)");
        out_line(proc, "  mesh stat                         — статистика CRDT mesh");
        return 1;
    }

    /* Грубая токенизация для наших коротких команд */
    char tmp[256]; strncpy(tmp, line_utf8, sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0;
    char* argv[8]; int argc = split_tokens(tmp, argv, 8);
    if (argc == 0) return 0;

    Replicator* hub = con_processor_get_repl_hub(proc);
    uint64_t console_id = con_processor_get_console_id(proc);
    TopicId def_topic = (TopicId){ .type_id = 1u, .inst_id = console_id };

    /* ------- hub ... ------- */
    if (argc>=2 && strcmp(argv[0],"hub")==0){
        if (!hub){ out_line(proc, "hub: нет репликатора"); return 1; }
        if (strcmp(argv[1],"backends")==0){
            int n = replhub_debug_count_backends(hub);
            outf(proc, "backends: %d", n);
            for (int i=0;i<n;i++){
                int pr=0,caps=0,hl=0; char cs[32];
                if (replhub_debug_backend_info(hub, i, &pr, &caps, &hl)==0){
                    outf(proc, "  [%d] prio=%d caps=%s health=%s", i, pr, caps_str(caps,cs,sizeof(cs)), (hl==0?"OK":"BAD"));
                }
            }
            return 1;
        } else if (strcmp(argv[1],"route")==0){
            TopicId t = def_topic;
            if (argc>=4){ t.type_id=(uint64_t)strtoull(argv[2],NULL,0); t.inst_id=(uint64_t)strtoull(argv[3],NULL,0); }
            int bi = replhub_debug_route_backend(hub, t);
            outf(proc, "route type=%" PRIu64 " inst=%" PRIu64 " -> backend=%d", (unsigned long long)t.type_id, (unsigned long long)t.inst_id, bi);
            return 1;
        } else if (strcmp(argv[1],"force")==0 && argc>=3){
            int idx = (int)strtol(argv[2], NULL, 0);
            replhub_force_backend(hub, def_topic, idx);
            outf(proc, "hub: force backend=%d for console/%" PRIu64, idx, (unsigned long long)console_id);
            return 1;
        } else if (strcmp(argv[1],"require")==0 && argc>=3){
            int req = (int)strtol(argv[2], NULL, 0);
            replhub_set_topic_caps(hub, def_topic, req);
            outf(proc, "hub: require caps=0x%X for console/%" PRIu64, req, (unsigned long long)console_id);
            return 1;
        } else if (strcmp(argv[1],"switch")==0){
            int to = -1;
            if (argc>=3) to = (int)strtol(argv[2], NULL, 0);
            replhub_switch_backend(hub, def_topic, to);
            outf(proc, "hub: switch console/%" PRIu64 " to=%d (soft)", (unsigned long long)console_id, to);
            return 1;
        }
        return 0;
    }

    /* ------- net client ... ------- */
    if (argc>=2 && strcmp(argv[0],"net")==0 && strcmp(argv[1],"client")==0){
#if defined(__EMSCRIPTEN__)
        out_line(proc, "net client: недоступно в web-сборке");
        return 1;
#else
        if (!hub){ out_line(proc, "net client: нет репликатора"); return 1; }
        int cidx = find_client_backend_idx(hub);
        if (cidx < 0){ out_line(proc, "net client: бэкенд не найден"); return 1; }
        Replicator* cli = replhub_debug_get_backend(hub, cidx);
        if (!cli){ out_line(proc, "net client: ручка бэкенда недоступна"); return 1; }
        if (argc>=3 && strcmp(argv[2],"connect")==0 && argc>=5){
            const char* host = argv[3]; uint16_t port = (uint16_t)strtoul(argv[4], NULL, 0);
            int rc = repl_client_tcp_connect(cli, host, port);
            outf(proc, "net client: connect %s:%u -> %s", host, (unsigned)port, (rc==0?"OK":"ERR"));
            return 1;
        } else if (argc>=3 && strcmp(argv[2],"disconnect")==0){
            repl_client_tcp_disconnect(cli);
            out_line(proc, "net client: disconnect");
            return 1;
        } else if (argc>=3 && strcmp(argv[2],"stat")==0){
            int ok = repl_client_tcp_is_connected(cli);
            outf(proc, "net client: %s", ok ? "CONNECTED" : "DISCONNECTED");
            return 1;
        }
        return 0;
#endif
    }

    /* ------- mesh ... ------- */
    if (argc>=2 && strcmp(argv[0],"mesh")==0){
#if defined(__EMSCRIPTEN__)
        out_line(proc, "mesh: недоступно в web-сборке");
        return 1;
#else
        if (!hub){ out_line(proc, "mesh: нет репликатора"); return 1; }
        int midx = find_crdt_backend_idx(hub);
        if (midx < 0){ out_line(proc, "mesh: CRDT backend отсутствует"); return 1; }
        Replicator* mesh = replhub_debug_get_backend(hub, midx);
        if (!mesh){ out_line(proc, "mesh: ручка бэкенда недоступна"); return 1; }
        if (argc>=3 && strcmp(argv[1],"seed")==0 && strcmp(argv[2],"add")==0 && argc>=4){
            const char* host = argv[3];
            int rc = repl_crdt_mesh_seed_add(mesh, host);
            outf(proc, "mesh: seed add %s -> %s", host, (rc==0?"OK":"ERR"));
            return 1;
        } else if (strcmp(argv[1],"stat")==0){
            int peers=0, listen=0, topics=0;
            (void)repl_crdt_mesh_stat(mesh, &peers, &listen, &topics);
            outf(proc, "mesh: peers=%d listen=%s topics=%d", peers, listen?"yes":"no", topics);
            return 1;
        }
        return 0;
#endif
    }

    return 0;
}

void con_processor_apply_external(ConsoleProcessor* self, const ConOp* op)
{
    if (!self || !op) return;
    ConsoleStore* st = con_processor_get_store(self);
    if (!st) return;

    /* Полезные временные буферы под нуль-терминированные строки */
    char* tmp = NULL;
    const char* as_cstr = "";

    /* NB: Hub/mesh могут прислать снапшот всего состояния: tag="snapshot", init_blob!=NULL.
       Это вне списка типов, но полезно поддержать — безболезненно для остальных кейсов. */
    if (op->init_blob && op->init_size) {

        con_processor_init_from_blob(self, op->schema, op->init_blob, op->init_size);
        /* обычно Store сам дёрнет notify внутри своих операций; для снапшота можно явно: на всякий случай дёрнем уведомление */
        con_store_notify_changed(st);
        return;
    }

    switch (op->type)
    {
    case CON_OP_INSERT_TEXT: {
        /* payload — текст без гарантий \0 → нуль-терминируем */
        if (op->data && op->size) {
            tmp = (char*)malloc(op->size + 1);
            if (tmp) {
                memcpy(tmp, op->data, op->size);
                tmp[op->size] = '\0';
                as_cstr = tmp;
            }
        }
        /* CRDT-вставка по позиции с заданным ID и автором (user_id) */
        con_store_insert_text_at(st, op->new_item_id, &op->pos, as_cstr, op->user_id);
        free(tmp);
        break;
    }

    case CON_OP_INSERT_WIDGET: {
        /* Фабрика виджетов: реализована в console_processor_ext.c (переименуй, если нужно) */
        extern ConsoleWidget* con_ext_make_widget(uint32_t kind, const void* init_blob, size_t init_size);
        ConsoleWidget* w = con_ext_make_widget(op->widget_kind, op->init_blob, op->init_size);
        if (w) {
            con_store_insert_widget_at(st, op->new_item_id, &op->pos, w, op->user_id);
        }
        /* если фабрика вернула NULL — просто игнорируем незнакомый виджет */
        break;
    }

    case CON_OP_WIDGET_MSG: {
        /* Пробрасываем адресное сообщение в виджет по ID */
        const char* tag = op->tag ? op->tag : "";
        con_store_widget_message(st, op->widget_id, tag, op->data, op->size);
        break;
    }

    case CON_OP_WIDGET_DELTA: {
        /* Дельты приходят с tag="cw.delta" и ConDeltaHdr в начале payload.
           LWW-merge по (hlc, actor_id) выполняется внутри Store/виджета
           (on_message("cw.delta", ...)). Здесь лишь корректно доставляем. */
        const char* tag = (op->tag && *op->tag) ? op->tag : "cw.delta";
        con_store_widget_message(st, op->widget_id, tag, op->data, op->size);
        break;
    }

    case CON_OP_APPEND_LINE: {
        /* payload — готовая строка вывода; нуль-терминируем и добавляем в историю */
        if (op->data && op->size) {
            tmp = (char*)malloc(op->size + 1);
            if (tmp) {
                memcpy(tmp, op->data, op->size);
                tmp[op->size] = '\0';
                as_cstr = tmp;
            }
        }
        con_store_append_line(st, as_cstr);
        free(tmp);
        break;
    }

    case CON_OP_PROMPT_META: {
        /* Обновление метаданных промпта другого участника: edits++ и nonempty */
        con_store_prompt_apply_meta(st, op->user_id, op->prompt_edits_inc, op->prompt_nonempty);
        break;
    }

    default:
        /* Неизвестный тип — ничего не делаем */
        break;
    }
}


int con_processor_snapshot(ConsoleProcessor* self, uint32_t* schema, void** blob, size_t* len)
{
    if (!self || !schema || !blob || !len) return 0;
    ConsoleStore* st = con_processor_get_store(self);
    if (!st) return 0;

    *schema = 0;    /* минимально валидное значение; при наличии явной схемы — подставь нужную */
    *blob   = NULL;
    *len    = 0;

    /* Сериализация состояния Store */
    if (!con_store_serialize(st, blob, len) || !*blob || *len == 0) {
        if (*blob) { free(*blob); *blob=NULL; }
        *len = 0;
        return 0;
    }
    return 1;
}


// В этой минимальной реализации делаем безопасный фолбэк: трактуем blob как UTF-8 текст,
// разбиваем по переводам строк и добавляем в Store. При необходимости позже можно
// заменить на полноценную десериализацию по schema.
void con_processor_init_from_blob(ConsoleProcessor* self, uint32_t schema, const void* blob, size_t len)
{
    (void)schema; /* пока схема не используется */
    if (!self || !blob || len == 0) return;
    ConsoleStore* st = con_processor_get_store(self);
    if (!st) return;

    /* ----- Фолбэк: не COW1-поток. Считаем, что это просто текстовый дамп. ----- */
    const char* bytes = (const char*)blob;
    size_t i = 0;
    while (i < len){
        /* пропустить начальные \r или \n */
        while (i < len && (bytes[i] == '\r' || bytes[i] == '\n')) i++;
        size_t j = i;
        while (j < len && bytes[j] != '\r' && bytes[j] != '\n') j++;
        if (j > i){
            size_t L = j - i;
            char* line = (char*)malloc(L + 1);
            if (!line) break;
            memcpy(line, bytes + i, L);
            line[L] = '\0';
            con_store_append_line(st, line);
            free(line);
        }
        i = j;
    }
    con_store_notify_changed(st);
}
