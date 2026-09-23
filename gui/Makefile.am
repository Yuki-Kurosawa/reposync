# reposync-gui (SDL3 self-drawn UI + local/libssh2 backends)
# Built only when ./configure runs with the GUI enabled (default).
AM_CFLAGS = -Wall -Wextra -std=gnu99
AM_CPPFLAGS = $(SDL3_CFLAGS) $(SDL3_TTF_CFLAGS) $(LIBSSH2_CFLAGS) $(SQLITE3_CFLAGS)

bin_PROGRAMS = reposync-gui
reposync_gui_SOURCES = main.c app.c backend_local.c backend_ssh2.c ui_sdl.c
reposync_gui_LDADD = $(SDL3_LIBS) $(SDL3_TTF_LIBS) $(LIBSSH2_LIBS) $(SQLITE3_LIBS)

# Headless backend smoke test (no GUI / no display needed):
#   ./selftest [--ssh host port user pass] [command...]
noinst_PROGRAMS = selftest
selftest_SOURCES = selftest.c app.c backend_local.c backend_ssh2.c
selftest_LDADD = $(LIBSSH2_LIBS) $(SQLITE3_LIBS)

# automake leaves an empty .deps dir behind; remove it so distclean is total
distclean-local:
	rm -rf .deps
