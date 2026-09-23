# reposync — top-level: CLI binary, then optional gui/
# Build: autoreconf -i && ./configure && make   (make distclean to wipe all)
AM_CFLAGS = -Wall -Wextra -std=c99 -D_DEFAULT_SOURCE -DTOOLNAME='"reposync"'

bin_PROGRAMS = reposync
reposync_SOURCES = \
	src/main.c src/config.c src/strbuf.c src/list.c src/util.c src/deb822.c \
	src/fetch.c src/index.c src/db.c src/sim.c src/sha256.c src/target.c src/ops.c
reposync_LDADD = $(CLI_LIBS)

if BUILD_GUI
SUBDIRS = gui
endif

# convenience targets kept for the test scripts / workflows
cli: reposync

# automake leaves empty .deps dirs behind; remove them too so distclean is total
distclean-local:
	rm -rf src/.deps

.PHONY: cli
