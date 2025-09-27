OUT := main

# ===== Sources =====
SRC_C   := main.c
SRC_S   := add.S  asm_call_get_size.S
# make NO_ASM=1
ifeq ($(NO_ASM),1)
  SRC_S :=
endif
SRC     := $(SRC_C) $(SRC_S)

EXEEXT :=
ifeq ($(OS),Windows_NT)
  EXEEXT := .exe
endif
BIN  := $(OUT)$(EXEEXT)

OBJ  := $(SRC_C:.c=.o) $(SRC_S:.S=.o)
DEPS := $(OBJ:.o=.d)

# ===== Toolchain & flags =====
CC ?= cc
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -MMD -MP
CPPFLAGS ?=
LDFLAGS  ?=

# SDL2
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

# SDL2_ttf (native)
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

# Quiet output (make V=1 to see commands)
V ?= 0
ifeq ($(V),1)
  Q :=
else
  Q := @
endif


.PHONY: all run clean mac linux windows-mingw windows-msys web serve serve-py


all: $(BIN)


$(BIN): $(OBJ)
	@echo "CFLAGS: $(CFLAGS) $(SDL2_CFLAGS) $(TTF_CFLAGS)"
	@echo "LIBS  : $(SDL2_LIBS) $(TTF_LIBS)"
	$(Q)$(CC) $(CFLAGS) $(SDL2_CFLAGS) $(TTF_CFLAGS) $(OBJ) -o $@ $(LDFLAGS) $(SDL2_LIBS) $(TTF_LIBS)


# Compile rules (with deps)
%.o: %.c
	$(Q)$(CC) $(CFLAGS) $(CPPFLAGS) $(SDL2_CFLAGS) $(TTF_CFLAGS) -c $< -o $@
%.o: %.S
	$(Q)$(CC) $(CFLAGS) $(CPPFLAGS) $(SDL2_CFLAGS) -c $< -o $@


run: $(BIN)
	./$(BIN)


clean:
	$(RM) -f $(BIN) $(OBJ) $(DEPS) web/index.html


# MacOs
mac: CC=clang
mac: CFLAGS+= -std=c11
mac: all


# Linux
linux: CC?=gcc
linux: CFLAGS+= -std=c11
linux: all


# Web (Emscripten)
# C-файлы для веба (исключаем любой ASM, если вдруг он добавится в SRC)
WEB_SRCS   := $(filter %.c,$(SRC))
WEB_DIR    := web
WEB_OUT    := $(WEB_DIR)/index.html
EMCC       ?= emcc


# SDL2/TTF in browser
WEB_FLAGS  := -O2 -sUSE_SDL=2 -sUSE_SDL_TTF=2 -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1 -sEXIT_RUNTIME=0
# optional: WEB_FLAGS += -sUSE_FREETYPE=1


web: $(WEB_OUT)


$(WEB_OUT): $(WEB_SRCS) | $(WEB_DIR)
	$(EMCC) $(CFLAGS) $(CPPFLAGS) $(WEB_SRCS) $(WEB_FLAGS) \
	  --preload-file assets@/assets -o $(WEB_OUT)
	@echo
	@echo "Built: $(WEB_OUT)"
	@echo "Run:   make serve  (or serve-py)"

# Предзагрузка ассетов (шрифт)
WEB_ASSETS := assets
$(WEB_DIR)/.assets: $(WEB_ASSETS)
	@mkdir -p $(WEB_DIR)
	@cp -R $(WEB_ASSETS) $(WEB_DIR)/
	@touch $(WEB_DIR)/.assets


$(WEB_DIR):
	@mkdir -p $(WEB_DIR)


serve: web
	emrun --no_browser --port 8080 $(WEB_OUT)


serve-py: web
	cd $(WEB_DIR) && python3 -m http.server 8080


# Windows
windows-mingw: CC?=x86_64-w64-mingw32-gcc
windows-mingw: EXEEXT=.exe
windows-mingw: all

windows-msys: CC?=gcc
windows-msys: EXEEXT=.exe
windows-msys: all

# Auto deps
-include $(DEPS)
