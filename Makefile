# reposync - Debian repo mirror/sync tool (Linux CLI)
#   deps (all permissive, no GPL/LGPL): zlib, zstd, sqlite3 (public domain),
#   libcurl (curl license; HTTPS pulls in OpenSSL libssl/libcrypto,
#   Apache-2.0/OpenSSL license). No curl/curl.exe binary is ever needed.
#   debian/ubuntu: libz-dev libzstd-dev libsqlite3-dev libcurl4-openssl-dev
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c99 -D_DEFAULT_SOURCE -DTOOLNAME='"reposync"'
LDLIBS = -lz -lzstd -lsqlite3 -lcurl

SRCS = src/main.c src/config.c src/strbuf.c src/list.c src/util.c src/deb822.c src/fetch.c \
       src/index.c src/db.c src/sim.c src/sha256.c src/target.c src/ops.c
OBJS = $(SRCS:.c=.o)
BIN = reposync

all: $(BIN) gui

cli: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

gui:
	$(MAKE) -C gui

clean:
	rm -f $(OBJS) $(BIN)
	$(MAKE) -C gui clean

.PHONY: all cli gui clean
