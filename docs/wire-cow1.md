# COW1 (Console Operations Wire v1)

Бинарный формат сериализации `ConOp` для передачи по байтовому потоку (TCP и т.п.).
Цели: фиксированная разметка, LE-эндиян, стриминговый разбор, жёсткие лимиты, отсутствие UB.

## Фрейм

Фрейм — это длина + полезная нагрузка:

```
u32  frame_len_le    // длина ВСЕГО после этого поля (payload)
char magic[4] = "COW1"
u16  ver = 1
u16  type                 // ConOpType
u64  console_id
u64  op_id
u32  actor_id
u64  hlc
i32  user_id
u64  widget_id
u32  widget_kind
u64  new_item_id
u64  parent_left
u64  parent_right
// ConPosId (fixed-size; depth<=8)
u8   pos.depth
repeat 8 times:
  u16 digit
  u32 actor
u64  init_hash
i32  prompt_edits_inc
i32  prompt_nonempty
u32  tag_len
u32  data_len
u32  init_len
u8[tag_len]  tag (без NUL)
u8[data_len] data
u8[init_len] init_blob
```

Максимальные размеры секций (можно переопределить макросами при сборке):

- `COW1_MAX_TAG`  — по умолчанию 4 KB
- `COW1_MAX_DATA` — 256 KB
- `COW1_MAX_INIT` — 1 MB

Ограничения проверки:

- `magic == "COW1"`, `ver == 1`
- `pos.depth <= 8`
- `tag_len/data_len/init_len` не превышают лимитов
- для некоторых `type` — согласованность полей:
  - `CON_OP_PROMPT_META`: `tag_len==0 && data_len==0 && init_len==0`
  - `CON_OP_INSERT_WIDGET`: `widget_kind != 0`

## Сериализация

Функция `conop_wire_encode()` пишет фрейм целиком (с префиксом длины).
`conop_wire_decode()` разбирает ПОЛНЫЙ фрейм из буфера.

## Потоковый разбор

`Cow1Decoder` — конечный автомат для TCP-потока:

1. Копит минимум 4 байта, чтобы узнать `frame_len`.
2. Дочитывает `4 + frame_len` байт.
3. Возвращает готовый `ConOp` (с копиями `tag/data/init`), оставаясь в буфере с остатком.

API:
```c
void   cow1_decoder_init(Cow1Decoder*);
size_t cow1_decoder_consume(Cow1Decoder*, const uint8_t* data, size_t len);
int    cow1_decoder_take_next(Cow1Decoder*, ConOp* out,
                              char** out_tag, void** out_data, size_t* out_data_len,
                              void** out_init, size_t* out_init_len);
void   cow1_decoder_reset(Cow1Decoder*);
```

### Замечания

- Декодер хранит внутренний линейный буфер и по мере «съедания» кадров сдвигает хвост.
- Никаких `#pragma pack`/чтения через «сырые» структуры — только явные `le16/le32/le64`.
- При ошибке валидации `cow1_decoder_take_next()` возвращает `<0` и очищает свой буфер
  (рекомендуется разорвать TCP-соединение).

## TCP-фрейминг

Модуль `net/wire_tcp.{h,c}` реализует поверх неблокирующего сокета:

- чтение частичных/слипшихся пакетов,
- сборку фреймов через `Cow1Decoder`,
- очереди на запись (частичные `send()`),
- обратный вызов на каждый успешно разобранный `ConOp`.

Создание:
```c
Cow1Tcp* cow1tcp_create(NetPoller* np, net_fd_t fd,
                        Cow1TcpOnOp on_op, void* user);
void     cow1tcp_destroy(Cow1Tcp*);
int      cow1tcp_send(Cow1Tcp*, const ConOp* op); /* encode+enqueue */
```

На ошибках чтения/валидации рекомендуется закрывать соединение.

## Инварианты ConPosId

`depth <= 8`; если `depth = d`, то значимы компоненты `[0..d-1]` (каждый `(digit:u16, actor:u32)`).
В сериализации всегда пишется фиксированная «решётка» на 8 элементов с `depth` спереди.

## Совместимость вперёд

Зарезервированные поля: нет (v1). Эволюцию планируем через `flags` в будущем и игнорирование незнакомых тегов/видов.
