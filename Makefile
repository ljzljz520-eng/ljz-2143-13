CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -pthread
SRC_DIR := src
TARGET := visual-window-app
SOURCES := $(SRC_DIR)/main.c $(SRC_DIR)/window.c $(SRC_DIR)/renderer.c $(SRC_DIR)/layout.c \
           $(SRC_DIR)/font_manager.c $(SRC_DIR)/net.c $(SRC_DIR)/storage.c $(SRC_DIR)/app.c

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_image SDL2_ttf libcurl jansson 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 SDL2_image SDL2_ttf libcurl jansson 2>/dev/null)
LDLIBS := $(SDL_LIBS) -lm

.PHONY: all clean run server test

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(SOURCES) -o $@ $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

server:
	python3 server/app.py

test:
	python3 -m unittest discover -s tests -v

clean:
	rm -f $(TARGET)
