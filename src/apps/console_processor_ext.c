#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <inttypes.h>
#include "console/processor.h"
#include "console/store.h"   /* чтобы были прототипы con_store_* и тип ConsoleStore */
#include "console/sink.h"
#include "console/store.h"   /* чтобы были прототипы con_store_* и тип ConsoleStore */
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

static void sinkf(ConsoleSink* s, int user_id, const char* fmt, ...){
    if (!s || !fmt) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    con_sink_append_line(s, user_id, buf);
}

/* Опционально: если процессор держит прямую ссылку на client-бэкенд, возьмём её.
 * Функция может отсутствовать — объявляем как weak. Тогда net client ... вежливо сообщит,
 * что «клиент не провязан», без крэшей и лишних зависимостей. */
#if defined(__GNUC__) || defined(__clang__)
extern Replicator* con_processor_get_client_tcp(ConsoleProcessor*)
    __attribute__((weak));
#else
/* На MSVC weak не поддерживается — просто объявление; если символа нет, линковка подскажет. */
extern Replicator* con_processor_get_client_tcp(ConsoleProcessor*);
#endif

static Replicator* get_client_backend(ConsoleProcessor* p){
    Replicator* cli = NULL;
#if defined(__GNUC__) || defined(__clang__)
    if (&con_processor_get_client_tcp) /* weak может быть NULL */
        cli = con_processor_get_client_tcp(p);
#else
    cli = con_processor_get_client_tcp(p);
#endif
    return cli;
}
static void reply(ConsoleProcessor* p, const char* msg){
    ConsoleSink* s = con_processor_get_sink(p);
    if (s) con_sink_append_line(s, /*user_id=*/-1, msg);
}
static int has_prefix(const char* s, const char* pfx){
    size_t n=strlen(pfx); return strncmp(s,pfx,n)==0;
}
static uint16_t parse_port(const char* s, uint16_t def){
    long v = strtol(s, NULL, 10);
    if (v<=0 || v>65535) return def;
    return (uint16_t)v;
}

/* parse "host[:port]" */
static int split_host_port(const char* arg, char* host_out, size_t cap, uint16_t* port_out, uint16_t dflt){
    const char* c = strchr(arg, ':');
    if (c){
        size_t L = (size_t)(c-arg);
        if (L==0 || L>=cap) return 0;
        memcpy(host_out, arg, L); host_out[L]=0;
        *port_out = parse_port(c+1, dflt);
        return 1;
    } else {
        size_t L = strlen(arg);
        if (L==0 || L>=cap) return 0;
        memcpy(host_out, arg, L+1);
        *port_out = dflt;
        return 1;
    }
}

/* caps parser: space-separated keywords */
static int parse_caps(const char* args){
    int caps = 0;
    char buf[256]; strncpy(buf, args, sizeof(buf)-1); buf[sizeof(buf)-1]=0;
    for (char* tok = strtok(buf, " \t"); tok; tok=strtok(NULL, " \t")){
        if      (strcmp(tok,"none")==0)      { caps = 0; }
        else if (strcmp(tok,"ordered")==0)   { caps |= REPL_ORDERED; }
        else if (strcmp(tok,"reliable")==0)  { caps |= REPL_RELIABLE; }
        else if (strcmp(tok,"broadcast")==0) { caps |= REPL_BROADCAST; }
        else if (strcmp(tok,"crdt")==0)      { caps |= REPL_CRDT; }
        else if (strcmp(tok,"discover")==0)  { caps |= REPL_DISCOVER; }
    }
    return caps;
}

int con_processor_ext_try_handle(ConsoleProcessor* proc, const char* line_utf8) {
    if (!proc || !line_utf8) return 0;

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


    /* /\* ===== NET commands ===== *\/ */
    /* if (strncmp(line_utf8, "net", 3)==0 && (line_utf8[3]==0 || line_utf8[3]==' ')){ */
    /*     ConsoleSink* sink = con_processor_get_sink(proc); */
    /*     int user_id = 0; /\* кто ввёл команду — если известен, можно пробросить *\/ */
    /*     Replicator* hub = con_processor_get_repl_hub(proc); */
    /*     uint64_t cid = con_processor_get_console_id(proc); */
    /*     if (!hub){ */
    /*         sinkf(sink, user_id, "net: hub is not set"); */
    /*         return 1; */
    /*     } */

    /*     /\* разобьём на токены *\/ */
    /*     char tmp[256]; strncpy(tmp, line_utf8, sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0; */
    /*     char* save=NULL; */
    /*     char* tok = strtok_r(tmp, " \t", &save);                  /\* "net" *\/ */
    /*     char* a1  = strtok_r(NULL, " \t", &save);                 /\* subcmd *\/ */
    /*     char* a2  = strtok_r(NULL, " \t", &save); */
    /*     char* a3  = strtok_r(NULL, " \t", &save); */
    /*     char* a4  = strtok_r(NULL, " \t", &save); */

    /*     TopicId topic = { .type_id = 1u, .inst_id = cid };        /\* console/<cid> *\/ */

    /*     if (!a1 || strcmp(a1,"status")==0){ */
    /*         int n = replhub_debug_count_backends(hub); */
    /*         int route = replhub_debug_route_backend(hub, topic); */
    /*         sinkf(sink, user_id, "net: backends=%d, route(console/%" PRIu64 ")=%d", n, cid, route); */
    /*         for (int i=0;i<n;i++){ */
    /*             int pr=0, caps=0, hl=-1; */
    /*             (void)replhub_debug_backend_info(hub, i, &pr, &caps, &hl); */
    /*             char cbuf[16]; caps_str(caps, cbuf); */
    /*             sinkf(sink, user_id, "  [%d]%s prio=%d caps=%s health=%s", */
    /*                   i, (i==route ? "*" : " "), pr, cbuf, (hl==0 ? "OK" : "BAD")); */
    /*         } */
    /*         return 1; */
    /*     } */

    /*     if (strcmp(a1,"switch")==0){ */
    /*         int to = -1; */
    /*         if (a2 && *a2){ */
    /*             char* end=NULL; long v = strtol(a2, &end, 10); */
    /*             if (end && *end==0) to = (int)v; */
    /*             else { sinkf(sink,user_id,"net switch: bad index '%s'", a2); return 1; } */
    /*         } */
    /*         replhub_switch_backend(hub, topic, to); */
    /*         if (to>=0) sinkf(sink,user_id,"net: switching route(console/%" PRIu64 ") -> backend[%d]", cid, to); */
    /*         else       sinkf(sink,user_id,"net: switching route(console/%" PRIu64 ") by policy", cid); */
    /*         return 1; */
    /*     } */

    /*     if (strcmp(a1,"force")==0){ */
    /*         if (!a2){ sinkf(sink,user_id,"usage: net force <idx|-1>"); return 1; } */
    /*         char* end=NULL; long v=strtol(a2,&end,10); */
    /*         if (end && *end==0){ */
    /*             replhub_force_backend(hub, topic, (int)v); */
    /*             if (v>=0) sinkf(sink,user_id,"net: force backend[%ld] for console/%" PRIu64, v, cid); */
    /*             else      sinkf(sink,user_id,"net: force cleared for console/%" PRIu64, cid); */
    /*         } else { */
    /*             sinkf(sink,user_id,"net force: bad index '%s'", a2); */
    /*         } */
    /*         return 1; */
    /*     } */

    /*     if (strcmp(a1,"caps")==0){ */
    /*         if (!a2){ sinkf(sink,user_id,"usage: net caps <mask>    (e.g., 3 or 0x3)"); return 1; } */
    /*         char* end=NULL; long v=strtol(a2,&end,0); */
    /*         if (end && *end==0){ */
    /*             replhub_set_topic_caps(hub, topic, (int)v); */
    /*             char cbuf[16]; caps_str((int)v, cbuf); */
    /*             sinkf(sink,user_id,"net: required caps(console/%" PRIu64 ") = %s (0x%lx)", cid, cbuf, v); */
    /*         } else { */
    /*             sinkf(sink,user_id,"net caps: bad mask '%s'", a2); */
    /*         } */
    /*         return 1; */
    /*     } */

    /*     if (strcmp(a1,"client")==0){ */
    /*         Replicator* cli = get_client_backend(proc); */
    /*         if (!a2 || strcmp(a2,"status")==0){ */
    /*             int ok = cli ? repl_client_tcp_is_connected(cli) : 0; */
    /*             sinkf(sink,user_id,"net client: %s", ok ? "connected" : (cli ? "disconnected" : "not wired")); */
    /*             return 1; */
    /*         } */
    /*         if (strcmp(a2,"connect")==0){ */
    /*             if (!a3 || !a4){ sinkf(sink,user_id,"usage: net client connect <host> <port>"); return 1; } */
    /*             if (!cli){ sinkf(sink,user_id,"net client: backend is not wired"); return 1; } */
    /*             char* end=NULL; long port = strtol(a4,&end,10); */
    /*             if (!(end && *end==0) || port<=0 || port>65535){ */
    /*                 sinkf(sink,user_id,"net client: bad port '%s'", a4); return 1; */
    /*             } */
    /*             int rc = repl_client_tcp_connect(cli, a3, (uint16_t)port); */
    /*             sinkf(sink,user_id, rc==0 ? "net client: connecting to %s:%ld" : "net client: connect(%s:%ld) failed", */
    /*                   a3, port); */
    /*             return 1; */
    /*         } */
    /*         if (strcmp(a2,"disconnect")==0){ */
    /*             if (!cli){ sinkf(sink,user_id,"net client: backend is not wired"); return 1; } */
    /*             repl_client_tcp_disconnect(cli); */
    /*             sinkf(sink,user_id,"net client: disconnect"); */
    /*             return 1; */
    /*         } */
    /*         sinkf(sink,user_id,"usage: net client <status|connect|disconnect> ..."); */
    /*         return 1; */
    /*     } */

    /*     /\* неизвестная подкоманда net *\/ */
    /*     sinkf(sink, user_id, "usage:"); */
    /*     sinkf(sink, user_id, "  net status"); */
    /*     sinkf(sink, user_id, "  net switch [idx]"); */
    /*     sinkf(sink, user_id, "  net force <idx|-1>"); */
    /*     sinkf(sink, user_id, "  net caps <mask>"); */
    /*     sinkf(sink, user_id, "  net client status"); */
    /*     sinkf(sink, user_id, "  net client connect <host> <port>"); */
    /*     sinkf(sink, user_id, "  net client disconnect"); */
    /*     return 1; */
    /* } */
    /* /\* ===== net client ===== *\/ */
    /* if (has_prefix(line, "net client")){ */
    /*     const char* args = line + strlen("net client"); */
    /*     while (*args==' '||*args=='\t') ++args; */
    /*     if (strncmp(args,"off",3)==0){ */
    /*         Replicator* hub = con_processor_get_repl_hub(p); */
    /*         (void)hub; /\* сам клиент живёт как один из бэкендов Хаба — отключим его через API бэкенда *\/ */
    /*         /\* найдём клиент среди бэкендов и вызовем disconnect, но у нас только Hub; проще: */
    /*            опубликовать «disconnect» на известном клиенте — добавим прямой вызов, */
    /*            так как клиент мы создаём в main и кладём в backends (см. приоритеты). *\/ */
    /*         /\* В минимальной версии просто попробуем на «всем известном» клиенте — через Hub это неэкспонировано. */
    /*            Поэтому просим пользователя подключать/отключать через ту же команду (OK), а detach делаем без адреса. *\/ */
    /*         /\* Более прямолинейно: просто сообщим, что соединение будет закрыто при следующем connect. *\/ */
    /*         reply(p, "[net] client: disconnecting"); */
    /*         /\* Для простоты: вызовем disconnect на «всем» клиенте — у нас нет указателя, поэтому */
    /*            документируем, что этот вызов придёт из main через глобальный клиент. См. ниже команды hub switch. *\/ */
    /*         /\* Ничего не делаем здесь, чтобы не плодить глобалы. *\/ */
    /*         return 1; */
    /*     } */
    /*     if (strncmp(args,"status",6)==0){ */
    /*         reply(p, "[net] client: status is shown via 'hub backends' (health==0 => connected)"); */
    /*         return 1; */
    /*     } */
    /*     /\* host[:port] *\/ */
    /*     char host[256]; uint16_t port=33334; */
    /*     if (!*args){ reply(p, "usage: net client <host[:port]>"); return 1; } */
    /*     if (!split_host_port(args, host, sizeof(host), &port, 33334)){ */
    /*         reply(p, "[net] client: bad host[:port]"); return 1; */
    /*     } */
    /*     /\* достанем hub и попросим клиент подключиться: */
    /*        В этой минимальной реализации предполагается, что клиент-бэкенд уже присутствует в Hub (создан в main). *\/ */
    /*     Replicator* hub = con_processor_get_repl_hub(p); */
    /*     (void)hub; */
    /*     /\* Чтобы не вытаскивать сам объект клиента из Hub, вызовем глобальную фабрику через «hint»: */
    /*        в нашем дизайне клиент создан и включён, так что просьба на подключение можно «транзитом» */
    /*        доставить, если у нас есть прямой указатель. *\/ */
    /*     /\* Проще: добавим тонкую прослойку — укажем пользователю, что переключить маршрут на клиента можно 'hub switch <idx>' *\/ */
    /*     reply(p, "[net] client: dialing (see 'hub backends' for index; then 'hub switch <idx>'):"); */
    /*     char msg[384]; */
    /*     snprintf(msg, sizeof(msg), "→ target %s:%u", host, (unsigned)port); */
    /*     reply(p, msg); */
    /*     /\* Реальный connect выполняется из main через вызов repl_client_tcp_connect() — см. комментарий в README / инструкции. *\/ */
    /*     return 1; */
    /* } */

    /* /\* ===== hub backends ===== *\/ */
    /* if (has_prefix(line, "hub backends")){ */
    /*     Replicator* hub = con_processor_get_repl_hub(p); */
    /*     if (!hub){ reply(p, "[hub] not set"); return 1; } */
    /*     int n = replhub_debug_count_backends(hub); */
    /*     char buf[128]; */
    /*     snprintf(buf,sizeof(buf),"[hub] backends: %d", n); */
    /*     reply(p, buf); */
    /*     TopicId t = { .type_id=1, .inst_id=con_processor_get_console_id(p) }; */
    /*     int sel = replhub_debug_route_backend(hub, t); */
    /*     for (int i=0;i<n;i++){ */
    /*         int pr=0, caps=0, hl=-1; */
    /*         replhub_debug_backend_info(hub, i, &pr, &caps, &hl); */
    /*         snprintf(buf,sizeof(buf),"  #%d prio=%d caps=0x%02X health=%d%s", */
    /*                  i, pr, caps, hl, (i==sel?"  <selected>":"")); */
    /*         reply(p, buf); */
    /*     } */
    /*     return 1; */
    /* } */

    /* if (has_prefix(line, "hub switch")){ */
    /*     const char* args = line + strlen("hub switch"); */
    /*     while (*args==' '||*args=='\t') ++args; */
    /*     Replicator* hub = con_processor_get_repl_hub(p); */
    /*     if (!hub){ reply(p, "[hub] not set"); return 1; } */
    /*     TopicId t = { .type_id=1, .inst_id=con_processor_get_console_id(p) }; */
    /*     if (strncmp(args,"best",4)==0 || *args==0){ */
    /*         replhub_switch_backend(hub, t, -1); */
    /*         reply(p, "[hub] switched to best"); */
    /*         return 1; */
    /*     } */
    /*     int idx = (int)strtol(args, NULL, 10); */
    /*     replhub_switch_backend(hub, t, idx); */
    /*     reply(p, "[hub] switch requested"); */
    /*     return 1; */
    /* } */

    /* if (has_prefix(line, "hub force")){ */
    /*     const char* args = line + strlen("hub force"); */
    /*     while (*args==' '||*args=='\t') ++args; */
    /*     Replicator* hub = con_processor_get_repl_hub(p); */
    /*     if (!hub){ reply(p, "[hub] not set"); return 1; } */
    /*     TopicId t = { .type_id=1, .inst_id=con_processor_get_console_id(p) }; */
    /*     if (strncmp(args,"off",3)==0){ */
    /*         replhub_force_backend(hub, t, -1); */
    /*         reply(p, "[hub] force off"); */
    /*         return 1; */
    /*     } */
    /*     int idx = (int)strtol(args, NULL, 10); */
    /*     replhub_force_backend(hub, t, idx); */
    /*     reply(p, "[hub] force set"); */
    /*     return 1; */
    /* } */

    /* if (has_prefix(line, "hub caps")){ */
    /*     const char* args = line + strlen("hub caps"); */
    /*     while (*args==' '||*args=='\t') ++args; */
    /*     Replicator* hub = con_processor_get_repl_hub(p); */
    /*     if (!hub){ reply(p, "[hub] not set"); return 1; } */
    /*     int caps = parse_caps(args); */
    /*     TopicId t = { .type_id=1, .inst_id=con_processor_get_console_id(p) }; */
    /*     replhub_set_topic_caps(hub, t, caps); */
    /*     char buf[128]; snprintf(buf,sizeof(buf),"[hub] caps=0x%02X", caps); */
    /*     reply(p, buf); */
    /*     return 1; */
    /* } */

    /* return 0; /\* не моя команда *\/ */
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
