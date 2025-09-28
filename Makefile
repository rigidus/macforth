# Структура исходников (см. каталог src/*):
#   core/     — менеджер окон, damage-листы, ввод, тайминг (без SDL-типа окон)
#   gfx/      — 2D-поверхность и текст (внутри SDL_Surface/TTF, наружу свой API)
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
PLAT_DIR := $(SRC_DIR)/platform
INC_DIR  := include
BUILD_DIR:= build
WEB_DIR  := web
ASSETS   := assets

# ===== Источники =====
SRC_CORE := \
  $(CORE_DIR)/wm.c \
  $(CORE_DIR)/window.c \
  $(CORE_DIR)/damage.c \
  $(CORE_DIR)/input.c \
  $(CORE_DIR)/timing.c

SRC_GFX := \
  $(GFX_DIR)/surface.c \
  $(GFX_DIR)/text.c

SRC_APPS := \
  $(APPS_DIR)/win_paint.c \
  $(APPS_DIR)/win_square.c \
  $(APPS_DIR)/win_console.c

SRC_PLAT := \
  $(PLAT_DIR)/platform_sdl.c

SRC_MAIN := main.c

SRC_C := $(SRC_CORE) $(SRC_GFX) $(SRC_APPS) $(SRC_PLAT) $(SRC_MAIN)

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

# ========================================================================

.PHONY: all run clean mac linux windows-mingw windows-msys wasm serve serve-py tree

all: $(BIN)

# ======= Цель: native бинарник =======
$(BIN): $(DIRS_TO_CREATE) $(OBJ)
	@echo "CFLAGS: $(CFLAGS) $(SDL2_CFLAGS) $(TTF_CFLAGS)"
	@echo "LIBS  : $(SDL2_LIBS) $(TTF_LIBS)"
	$(Q)$(CC) $(OBJ) -o $@ $(LDFLAGS) $(SDL2_LIBS) $(TTF_LIBS)

# ======= Компиляция .c → build/...o с автозависимостями =======
# unity: генерим объединённый файл, чтобы сохранялись пути включений
ifeq ($(UNITY),1)
$(UNITY_FILE): | $(BUILD_DIR)/
	@echo "/* автогенерируемый unity build */" > $@
	@echo "#include <stdio.h>" >> $@
	@$(foreach F,$(filter-out $(UNITY_FILE),$(SRC_CORE) $(SRC_GFX) $(SRC_APPS) $(SRC_PLAT) $(SRC_MAIN)), \
	  echo "#include \"$(F)\"";) >> $@
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
windows-mingw: all

windows-msys: CC?=gcc
windows-msys: EXEEXT=.exe
windows-msys: all

# ======= WebAssembly (Emscripten) =======
# Предзагрузка ассетов (шрифт и т.п.) → /assets в виртуальной FS.
$(WEB_DIR)/.assets: $(ASSETS)
	$(Q)mkdir -p $(WEB_DIR)
	$(Q)cp -R $(ASSETS) $(WEB_DIR)/
	$(Q)touch $@

wasm: $(WEB_OUT)

$(WEB_OUT): $(WEB_SRCS) $(WEB_DIR)/.assets | $(WEB_DIR)
	@echo "emcc: $(WEB_SRCS)"
	$(Q)$(EMCC) $(CFLAGS) $(CPPFLAGS) $(WEB_SRCS) $(WEB_FLAGS) \
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
