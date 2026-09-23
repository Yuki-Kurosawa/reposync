# reposync CLI (deb822/cross-platform config, libcurl fetch, sqlite local queries)
# Built from the top level via SUBDIRS; binary lands here as src/reposync.
AM_CFLAGS = -Wall -Wextra -std=c99 -D_DEFAULT_SOURCE -DTOOLNAME='"reposync"'

bin_PROGRAMS = reposync
reposync_SOURCES = \
	main.c config.c strbuf.c list.c util.c deb822.c \
	fetch.c index.c db.c sim.c sha256.c target.c ops.c
reposync_LDADD = $(CLI_LIBS)

# automake leaves an empty .deps dir behind; remove it so distclean is total
distclean-local:
	rm -rf .deps
