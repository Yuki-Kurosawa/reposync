dnl reposync — autoconf/automake build
dnl CLI (zlib/zstd/sqlite3/libcurl) + optional SDL3 GUI
AC_INIT([reposync], [0.1.0], [yuki@ksyuki.com])
AC_CONFIG_SRCDIR([src/main.c])
AM_INIT_AUTOMAKE([foreign subdir-objects])

AC_PROG_CC
AC_PROG_INSTALL

dnl ---- CLI dependencies (all mandatory, matching the legacy Makefile) ----
AC_CHECK_HEADER([zlib.h],        [], [AC_MSG_ERROR([zlib.h not found (install zlib1g-dev)])])
AC_CHECK_HEADER([zstd.h],        [], [AC_MSG_ERROR([zstd.h not found (install libzstd-dev)])])
AC_CHECK_HEADER([sqlite3.h],     [], [AC_MSG_ERROR([sqlite3.h not found (install libsqlite3-dev)])])
AC_CHECK_HEADER([curl/curl.h],   [], [AC_MSG_ERROR([curl/curl.h not found (install libcurl4-openssl-dev)])])
AC_CHECK_LIB([z],      [inflate],        [CLI_LIBS="$CLI_LIBS -lz"],      [AC_MSG_ERROR([libz not found])])
AC_CHECK_LIB([zstd],   [ZSTD_compress],  [CLI_LIBS="$CLI_LIBS -lzstd"],   [AC_MSG_ERROR([libzstd not found])])
AC_CHECK_LIB([sqlite3],[sqlite3_open],   [CLI_LIBS="$CLI_LIBS -lsqlite3"], [AC_MSG_ERROR([libsqlite3 not found])])
AC_CHECK_LIB([curl],   [curl_easy_init], [CLI_LIBS="$CLI_LIBS -lcurl"],   [AC_MSG_ERROR([libcurl not found])])
AC_SUBST([CLI_LIBS])

dnl ---- GUI (SDL3/libssh2/sqlite3), optional: --disable-gui ----
AC_ARG_ENABLE([gui],
  [AS_HELP_STRING([--disable-gui], [do not build the SDL3 GUI, CLI only])],
  [], [enable_gui=yes])
AS_IF([test "x$enable_gui" = xyes], [
  PKG_CHECK_MODULES([SDL3],     [sdl3],     [], [AC_MSG_ERROR([sdl3 not found (install libsdl3-dev, or use --disable-gui)])])
  PKG_CHECK_MODULES([SDL3_TTF], [sdl3-ttf], [], [AC_MSG_ERROR([sdl3-ttf not found (install libsdl3-ttf-dev)])])
  PKG_CHECK_MODULES([LIBSSH2],  [libssh2],  [], [AC_MSG_ERROR([libssh2 not found (install libssh2-1-dev)])])
  PKG_CHECK_MODULES([SQLITE3],  [sqlite3],  [], [AC_MSG_ERROR([sqlite3 not found (install libsqlite3-dev)])])
])
AM_CONDITIONAL([BUILD_GUI], [test "x$enable_gui" = xyes])

AC_CONFIG_FILES([Makefile gui/Makefile])
AC_OUTPUT
