# Структура исходников (см. каталог src/*):
#   core/     — менеджер окон, damage-листы, ввод, тайминг (без SDL-типа окон)
#   gfx/      — 2D-поверхность и текст (внутри SDL_Surface/TTF, наружу свой API)
#   net/      — сетевой поллер/транспорты (неблокирующие, кроссплатформенные)
#   apps/     — конкретные окна-приложения (paint/square/console)
#   platform/ — обёртка над SDL (окно, цикл, композиция, события)
#   main.c    — сборка подсистем, запуск цикла
#
# Веб-сборка (wasm) использует те же .c (через -sUSE_SDL=2 -sUSE_SDL_TTF=2).
# ========================================================================

# ===== Имя бинарника (native) =====
OUT      := wm
EXEEXT   :=
ifeq ($(OS),Windows_NT)
  EXEEXT := .exe
endif
BIN      := $(OUT)$(EXEEXT)

# ===== Каталоги =====
SRC_DIR  := src
CORE_DIR := $(SRC_DIR)/core
GFX_DIR  := $(SRC_DIR)/gfx
APPS_DIR := $(SRC_DIR)/apps
NET_DIR  := $(SRC_DIR)/net
PLAT_DIR := $(SRC_DIR)/platform
INC_DIR  := $(SRC_DIR)/include
BUILD_DIR:= build
WEB_DIR  := web
ASSETS   := assets
NET_LIBS :=

# ===== Источники =====
SRC_CORE :=                \
  $(CORE_DIR)/wm.c         \
  $(CORE_DIR)/window.c     \
  $(CORE_DIR)/damage.c     \
  $(CORE_DIR)/input.c      \
  $(CORE_DIR)/timing.c     \
  $(CORE_DIR)/loop_hooks.c

SRC_GFX := \
  $(GFX_DIR)/surface.c \
  $(GFX_DIR)/text.c

SRC_APPS := \
  $(APPS_DIR)/global_state.c \
  $(APPS_DIR)/widget_color.c \
  $(APPS_DIR)/console_processor_ext.c \
  $(APPS_DIR)/console_processor.c \
  $(APPS_DIR)/console_prompt.c \
  $(APPS_DIR)/console_store.c \
  $(APPS_DIR)/console_sink.c \
  $(APPS_DIR)/win_paint.c \
  $(APPS_DIR)/win_square.c \
  $(APPS_DIR)/win_console.c \
  $(APPS_DIR)/echo_component.c \
  $(SRC_DIR)/replication/type_registry.c \
  $(SRC_DIR)/replication/hub.c \
  $(SRC_DIR)/replication/repl_policy_default.c \
  $(SRC_DIR)/replication/backends/local_loop.c \
  $(SRC_DIR)/replication/backends/leader_tcp.c \
  $(SRC_DIR)/replication/backends/crdt_mesh.c \
  $(SRC_DIR)/replication/backends/client_tcp.c \


SRC_PLAT := \
  $(PLAT_DIR)/platform_sdl.c

SRC_NET := \
  $(NET_DIR)/poller.c

# ====== Сеть (кроссплатформенно) ======
# native: выбираем POSIX/Win32
ifeq ($(OS),Windows_NT)
  SRC_NET := $(NET_DIR)/net_win32.c
  NET_LIBS += -lws2_32
else
  SRC_NET := $(NET_DIR)/net_posix.c
endif
# wasm: подменим на заглушку при сборке emcc
WEB_NET := $(NET_DIR)/net_stub_emscripten.c

# Общая часть сети (независимо от платформенных реализаций сокетов)
SRC_NET_COMMON := \
  $(NET_DIR)/conop_wire.c \
  $(NET_DIR)/tcp.c \
  $(NET_DIR)/wire_tcp.c


# NB: wire_tcp.c собираем только на native, в wasm он не нужен (и не работает).

SRC_MAIN := main.c
TEST_DIR := tests

SRC_C := $(SRC_CORE) $(SRC_GFX) $(SRC_APPS) $(SRC_PLAT) $(SRC_NET_COMMON) $(SRC_NET) $(SRC_MAIN)

# ===== Unity build (опционально: make UNITY=1) =====
# Собираем один файл unity_all.c, который #include-ит все *.c
ifeq ($(UNITY),1)
  UNITY_FILE := $(BUILD_DIR)/unity_all.c
  SRC_C      := $(UNITY_FILE)
endif

# ===== Объекты/зависимости (в отдельном build/) =====
OBJ  := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRC_C))
DEPS := $(OBJ:.o=.d)

# Авто-создание подпапок build/…
DIRS_TO_CREATE := $(sort $(dir $(OBJ)) $(BUILD_DIR)/ $(WEB_DIR)/)

# ===== Инструменты и флаги =====
CC       ?= cc
AR       ?= ar
RANLIB   ?= ranlib

# Компилятор/линкер
CSTD      ?= -std=c11
OPT       ?= -O2
WARN      ?= -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align -Wstrict-prototypes
DEBUG     ?= -g
DEFINES   ?=
INC       ?= -I$(SRC_DIR) -I$(INC_DIR)

CFLAGS    ?= $(CSTD) $(OPT) $(WARN) $(DEBUG) -MMD -MP $(DEFINES) $(INC)
CPPFLAGS  ?=
LDFLAGS   ?=

# Санитайзеры по желанию: make SAN=asan (или ubsan, tsan)
ifneq ($(SAN),)
  ifeq ($(SAN),asan)
    CFLAGS  += -fsanitize=address -fno-omit-frame-pointer
    LDFLAGS += -fsanitize=address
  else ifeq ($(SAN),ubsan)
    CFLAGS  += -fsanitize=undefined -fno-omit-frame-pointer
    LDFLAGS += -fsanitize=undefined
  else ifeq ($(SAN),tsan)
    CFLAGS  += -fsanitize=thread -fno-omit-frame-pointer
    LDFLAGS += -fsanitize=thread
  endif
endif

# SDL2/SDL2_ttf (native). Порядок: сначала sdl2-config, потом pkg-config, потом common paths.
SDL2_CFLAGS ?= $(shell sdl2-config --cflags 2>/dev/null)
SDL2_LIBS   ?= $(shell sdl2-config --libs   2>/dev/null)
ifeq ($(strip $(SDL2_CFLAGS)$(SDL2_LIBS)),)
  SDL2_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
  SDL2_LIBS   := $(shell pkg-config --libs   sdl2 2>/dev/null)
endif
ifeq ($(strip $(SDL2_CFLAGS)$(SDL2_LIBS)),)
  ifneq (,$(wildcard /opt/homebrew/include/SDL2))
    SDL2_CFLAGS := -I/opt/homebrew/include/SDL2
    SDL2_LIBS   := -L/opt/homebrew/lib -lSDL2
  else ifneq (,$(wildcard /usr/local/include/SDL2))
    SDL2_CFLAGS := -I/usr/local/include/SDL2
    SDL2_LIBS   := -L/usr/local/lib -lSDL2
  endif
endif

TTF_CFLAGS ?= $(shell pkg-config --cflags SDL2_ttf 2>/dev/null)
TTF_LIBS   ?= $(shell pkg-config --libs   SDL2_ttf 2>/dev/null)
ifeq ($(strip $(TTF_CFLAGS)$(TTF_LIBS)),)
  ifneq (,$(wildcard /opt/homebrew/include/SDL2))
    TTF_CFLAGS := -I/opt/homebrew/include/SDL2
    TTF_LIBS   := -L/opt/homebrew/lib -lSDL2_ttf
  else ifneq (,$(wildcard /usr/local/include/SDL2))
    TTF_CFLAGS := -I/usr/local/include/SDL2
    TTF_LIBS   := -L/usr/local/lib -lSDL2_ttf
  endif
endif

# Тихий вывод (make V=1 — показать команды)
V ?= 0
ifeq ($(V),1)
  Q :=
else
  Q := @
endif

# ===== Web (Emscripten) =====
EMCC      ?= emcc
WEB_OUT   := $(WEB_DIR)/index.html
WEB_SRCS  := $(SRC_C)
WEB_FLAGS := -O2 -sUSE_SDL=2 -sUSE_SDL_TTF=2 -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1 -sEXIT_RUNTIME=0
EMCFLAGS  := $(WEB_FLAGS)
# Emscripten user config + writable cache (для портов FreeType/SDL_ttf)
EM_CONFIG ?= ./.emscripten
EM_CACHE  ?= ./.emscripten_cache
export EM_CONFIG
export EM_CACHE


# ========================================================================

.PHONY: all run clean mac linux windows-mingw windows-msys wasm wasm-setup wasm-info serve serve-py tree

all: $(BIN)

# ======= Цель: native бинарник =======
$(BIN): $(DIRS_TO_CREATE) $(OBJ)
	@echo "CFLAGS: $(CFLAGS) $(SDL2_CFLAGS) $(TTF_CFLAGS)"
	@echo "LIBS  : $(SDL2_LIBS) $(TTF_LIBS)"
	$(Q)$(CC) $(OBJ) -o $@ $(LDFLAGS) $(SDL2_LIBS) $(TTF_LIBS) $(NET_LIBS) -lm

# ======= Компиляция .c → build/...o с автозависимостями =======
# unity: генерим объединённый файл, чтобы сохранялись пути включений
ifeq ($(UNITY),1)
  UNITY_FILE := $(BUILD_DIR)/unity_all.c
  SRC_C      := $(UNITY_FILE)
  OBJ        := $(BUILD_DIR)/unity_all.o
$(UNITY_FILE): $(SRC_CORE) $(SRC_GFX) $(SRC_APPS) $(SRC_PLAT) $(SRC_NET_COMMON) $(SRC_NET) $(SRC_MAIN) | $(BUILD_DIR)/
	@echo "/* автогенерируемый unity build */" > $@
	@echo "#include <stdio.h>" >> $@
	@$(foreach F,$(filter-out $(UNITY_FILE),$(SRC_CORE) $(SRC_GFX) $(SRC_APPS) $(SRC_PLAT) $(SRC_NET_COMMON) $(SRC_NET) $(SRC_MAIN)), \
		echo "#include \"../$(F)\"" >> $@;)

# Спец-правило компиляции именно этого файла (обходит паттерн %.o: %.c)
$(BUILD_DIR)/unity_all.o: $(UNITY_FILE)
	$(Q)$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
endif

# Обычный .c
$(BUILD_DIR)/%.o: %.c
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) $(CPPFLAGS) $(SDL2_CFLAGS) $(TTF_CFLAGS) -c $< -o $@

# ======= Вспомогательное =======
$(DIRS_TO_CREATE):
	$(Q)mkdir -p $@

run: $(BIN)
	./$(BIN)

clean:
	$(RM) -r $(BUILD_DIR) $(BIN) $(WEB_OUT) $(WEB_DIR)/assets $(WEB_DIR)/.assets

tree:
	@echo "Sources:"; \
	find src -type f \( -name '*.c' -o -name '*.h' \) | sed 's/^/  /'

# ======= Профили под ОС =======
mac:    CC=clang
mac:    CFLAGS+= -std=c11
mac:    all

linux:  CC?=gcc
linux:  CFLAGS+= -std=c11
linux:  all

windows-mingw: CC?=x86_64-w64-mingw32-gcc
windows-mingw: EXEEXT=.exe
windows-mingw: NET_LIBS+=-lws2_32
windows-mingw: all

windows-msys: CC?=gcc
windows-msys: EXEEXT=.exe
windows-msys: NET_LIBS+=-lws2_32
windows-msys: all

# ======= WebAssembly (Emscripten) =======
# Предзагрузка ассетов (шрифт и т.п.) → /assets в виртуальной FS.
$(WEB_DIR)/.assets: $(ASSETS)
	$(Q)mkdir -p $(WEB_DIR)
	$(Q)cp -R $(ASSETS) $(WEB_DIR)/
	$(Q)touch $@

## Один раз на машине: создать конфиг и разморозить кэш
wasm-setup:
	@echo ">> Generating/patching Emscripten config: $(EM_CONFIG)"
	@[ -f "$(EM_CONFIG)" ] || env -u EM_CONFIG $(EMCC) --generate-config "$(EM_CONFIG)"
	@mkdir -p "$(EM_CACHE)"
	# GNU/BSD sed portability: try GNU '-i -E', fallback to BSD '-i '' -E'
	@sed -i -E 's/^FROZEN_CACHE *= *.*/FROZEN_CACHE = False/' "$(EM_CONFIG)" || sed -i '' -E 's/^FROZEN_CACHE *= *.*/FROZEN_CACHE = False/' "$(EM_CONFIG)"
	@sed -i -E 's|^CACHE *= .*|CACHE = os.path.expanduser("$(EM_CACHE)")|' "$(EM_CONFIG)" || sed -i '' -E 's|^CACHE *= .*|CACHE = os.path.expanduser("$(EM_CACHE)")|' "$(EM_CONFIG)"
	@echo ">> Done. CACHE=$(EM_CACHE); FROZEN_CACHE=False"

## Диагностика конфигурации
wasm-info:
	@echo "EM_CONFIG = $(EM_CONFIG)"
	@echo "EM_CACHE  = $(EM_CACHE)"
	@{ [ -f "$(EM_CONFIG)" ] && grep -E '^(CACHE|FROZEN_CACHE) *=' "$(EM_CONFIG)" || echo "(no $(EM_CONFIG))"; } || true


wasm: $(WEB_OUT)

$(WEB_OUT): $(WEB_SRCS) $(WEB_DIR)/.assets | $(WEB_DIR)
	@echo "emcc: $(WEB_SRCS)"
	# Заменяем native-бэкенд сети на wasm-заглушку
	$(eval WEB_SRCS := $(filter-out $(SRC_NET),$(WEB_SRCS)))
	$(eval WEB_SRCS := $(WEB_SRCS) $(WEB_NET))
	# Не включаем TCP-leader бэкенд/клиент и wire-транспорт в wasm
	$(eval WEB_SRCS := $(filter-out $(SRC_DIR)/replication/backends/leader_tcp.c,$(WEB_SRCS)))
	$(eval WEB_SRCS := $(filter-out $(SRC_DIR)/replication/backends/client_tcp.c,$(WEB_SRCS)))
	$(eval WEB_SRCS := $(filter-out $(NET_DIR)/wire_tcp.c,$(WEB_SRCS)))
	# wasm-setup
	@[ -f "$(EM_CONFIG)" ] || $(MAKE) wasm-setup
	@mkdir -p "$(EM_CACHE)"
	$(Q)$(EMCC) $(filter-out -MMD -MP,$(CFLAGS)) $(CPPFLAGS) $(WEB_SRCS) $(EMCFLAGS) \
	  --preload-file $(WEB_DIR)/assets@/assets \
	  -o $(WEB_OUT)
	@echo
	@echo "Built: $(WEB_OUT)"
	@echo "Run:   make serve  (или serve-py)"

$(WEB_DIR):
	$(Q)mkdir -p $(WEB_DIR)

serve: wasm
	# Требуется emrun из Emscripten
	emrun --no_browser --port 8080 $(WEB_OUT)

serve-py: wasm
	cd $(WEB_DIR) && python3 -m http.server 8080

# ======= Автозависимости =======
-include $(DEPS)

# ======= Тесты =======
.PHONY: test
TEST_BIN := $(BUILD_DIR)/tests/test_conop_wire$(EXEEXT)
TEST_OBJS := $(BUILD_DIR)/$(NET_DIR)/conop_wire.o $(BUILD_DIR)/$(TEST_DIR)/test_conop_wire.o

# второй тест — ReplHub policy/switch
TEST_BIN2 := $(BUILD_DIR)/tests/test_repl_hub$(EXEEXT)
TEST_OBJS2 := \
  $(BUILD_DIR)/$(SRC_DIR)/replication/hub.o \
  $(BUILD_DIR)/$(SRC_DIR)/replication/repl_policy_default.o \
  $(BUILD_DIR)/$(SRC_DIR)/replication/backends/local_loop.o \
  $(BUILD_DIR)/$(SRC_DIR)/replication/type_registry.o \
  $(BUILD_DIR)/$(TEST_DIR)/test_repl_hub.o

$(BUILD_DIR)/$(TEST_DIR)/test_conop_wire.o: $(TEST_DIR)/test_conop_wire.c
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -I$(SRC_DIR) -I$(INC_DIR) -c $< -o $@

$(TEST_BIN): $(DIRS_TO_CREATE) $(TEST_OBJS)
	$(Q)$(CC) $(TEST_OBJS) -o $@

$(BUILD_DIR)/$(TEST_DIR)/test_repl_hub.o: $(TEST_DIR)/test_repl_hub.c
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CFLAGS) -I$(SRC_DIR) -I$(INC_DIR) -c $< -o $@

$(TEST_BIN2): $(DIRS_TO_CREATE) $(TEST_OBJS2)
	$(Q)$(CC) $(TEST_OBJS2) -o $@

test: $(TEST_BIN) $(TEST_BIN2)
	@echo ">> Running tests"
	@$(TEST_BIN)
	@$(TEST_BIN2)
