# reposync-gui (Linux): SDL3 self-drawn UI + local/libssh2 backends
#   Zero GPL/LGPL: SDL3 (zlib), SDL3_ttf (zlib), libssh2 (BSD).
#   deps: libsdl3-dev, libsdl3-ttf-dev, libssh2-1-dev
#   (Debian/Ubuntu: sudo apt install libsdl3-dev libsdl3-ttf-dev libssh2-1-dev)
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu99
CFLAGS += $(shell pkg-config --cflags sdl3 sdl3-ttf libssh2 sqlite3)
LDLIBS = $(shell pkg-config --libs sdl3 sdl3-ttf libssh2 sqlite3)

BIN = reposync-gui
SRCS = main.c app.c backend_local.c backend_ssh2.c ui_sdl.c
OBJS = $(SRCS:.c=.o)

all: $(BIN) selftest

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Headless backend smoke test (no GUI / no display needed):
#   ./selftest [--ssh host port user pass] [command...]
selftest: selftest.o app.o backend_local.o backend_ssh2.o
	$(CC) $(CFLAGS) -o $@ $^ $(shell pkg-config --libs libssh2 sqlite3)

clean:
	rm -f $(OBJS) selftest.o $(BIN) selftest

.PHONY: all clean
