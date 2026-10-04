CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror
SRC_DIR := src
CORE_DIR := src/core
TARGET := visual-window-app
CORE_SOURCES := $(CORE_DIR)/vw_layout.c $(CORE_DIR)/vw_command.c \
                $(CORE_DIR)/vw_fsm.c $(CORE_DIR)/vw_policy.c \
                $(CORE_DIR)/vw_exit.c $(CORE_DIR)/vw_journal.c
CORE_TEST := build/core-test

SDL_AVAILABLE := $(shell command -v sdl2-config >/dev/null 2>&1 && echo 1)

.PHONY: all clean run check app core-test

# Host default: build the SDL app when SDL is present, otherwise the host-testable
# control core. Both always include the core library.
ifeq ($(SDL_AVAILABLE),1)
all: $(TARGET)
else
all: core-test
endif

core-test: $(CORE_TEST)

$(CORE_TEST): tests/core_test.c $(CORE_SOURCES) $(CORE_DIR)/vw.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc tests/core_test.c $(CORE_SOURCES) -o $@

check: core-test
	./$(CORE_TEST)

ifeq ($(SDL_AVAILABLE),1)
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)
SDLTTF_AVAILABLE := $(shell pkg-config --exists SDL2_ttf && echo 1)
ifeq ($(SDLTTF_AVAILABLE),1)
TTF_CFLAGS := $(shell pkg-config --cflags SDL2_ttf) -DVWIN_HAVE_SDL_TTF=1
TTF_LIBS := $(shell pkg-config --libs SDL2_ttf)
CURL_LIBS := $(shell pkg-config --libs libcurl)
APP_SOURCES := $(SRC_DIR)/main.c $(SRC_DIR)/window.c $(SRC_DIR)/renderer.c \
               $(SRC_DIR)/widgets.c $(SRC_DIR)/vw_net_client.c $(CORE_SOURCES)
else
APP_SOURCES := $(SRC_DIR)/main.c $(SRC_DIR)/window.c $(SRC_DIR)/renderer.c
TTF_CFLAGS :=
TTF_LIBS :=
CURL_LIBS :=
endif

app: $(TARGET)

$(TARGET): $(APP_SOURCES)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(TTF_CFLAGS) -Isrc $(APP_SOURCES) -o $@ \
	    $(SDL_LIBS) $(TTF_LIBS) -lSDL2_image $(CURL_LIBS)

run: $(TARGET)
	./$(TARGET)
else
app:
	@echo "SDL2 development files not found on host; build the app in Docker:"
	@echo "  docker compose build && docker compose up"
	@exit 1
endif

clean:
	rm -f $(TARGET)
	rm -rf build
