SRC := main.c
OUT := main

SRC_C   := main.c
SRC_S   := add.S  asm_call_get_size.S
SRC     := $(SRC_C) $(SRC_S)

EXEEXT :=
ifeq ($(OS),Windows_NT)
  EXEEXT := .exe
endif
BIN := $(OUT)$(EXEEXT)

OBJ     := $(SRC_C:.c=.o)
OBJ     += $(SRC_S:.S=.o)

CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra
CPPFLAGS?=
LDFLAGS ?=

SDL2_CFLAGS ?= $(shell sdl2-config --cflags 2>/dev/null)
SDL2_LIBS ?= $(shell sdl2-config --libs 2>/dev/null)
ifeq ($(strip $(SDL2_CFLAGS)$(SDL2_LIBS)),)
  SDL2_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
  SDL2_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
endif
ifeq ($(strip $(SDL2_CFLAGS)$(SDL2_LIBS)),)
  ifneq (,$(wildcard /opt/homebrew/include/SDL2))
    SDL2_CFLAGS := -I/opt/homebrew/include/SDL2
    SDL2_LIBS := -L/opt/homebrew/lib -lSDL2
  else ifneq (,$(wildcard /usr/local/include/SDL2))
    SDL2_CFLAGS := -I/usr/local/include/SDL2
    SDL2_LIBS := -L/usr/local/lib -lSDL2
  endif
endif

.PHONY: all run clean mac linux windows-mingw windows-msys

all: $(BIN)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SDL2_CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SDL2_CFLAGS) -c $< -o $@

$(BIN): $(OBJ)
	@echo CFLAGS: $(CFLAGS) $(SDL2_CFLAGS)
	@echo LIBS  : $(SDL2_LIBS)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(SDL2_LIBS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN) *.o
	rm -f web/*

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
# Базовые флаги для SDL2 в браузере
WEB_FLAGS  := -O2 -sUSE_SDL=2 -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1 -sEXIT_RUNTIME=0
# Можно добавить: -sSTACK_SIZE=1mb, -sINITIAL_MEMORY=64mb при необходимости

.PHONY: web serve

web: $(WEB_OUT)

$(WEB_OUT): $(WEB_SRCS)
	@mkdir -p $(WEB_DIR)
	$(EMCC) $(CFLAGS) $(CPPFLAGS) $(WEB_SRCS) $(WEB_FLAGS) -o $(WEB_OUT)
	@echo
	@echo "Built: $(WEB_OUT)"
	@echo "Open it via http server (see 'make serve')."

# Требует 'emrun' из EMSDK. Альтернатива ниже — python http.server
serve: web
	emrun --no_browser --port 8080 $(WEB_OUT)

# Альтернатива, если emrun не установлен:
.PHONY: serve-py
serve-py: web
	cd $(WEB_DIR) && python3 -m http.server 8080


# Cross-compile for MinGW (Linux/macOS, with toolchain)
windows-mingw: CC?=x86_64-w64-mingw32-gcc
windows-mingw: EXEEXT=.exe
windows-mingw: all

# Native build Windows in MSYS2/MinGW env
windows-msys: CC?=gcc
windows-msys: EXEEXT=.exe
windows-msys: all
