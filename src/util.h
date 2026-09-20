#ifndef REPOSYNC_UTIL_H
#define REPOSYNC_UTIL_H

#include <stddef.h>
#include <sys/types.h>
#include <zlib.h>
#include "strbuf.h"

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* Recursively create directory path, like mkdir -p. Returns 0 on success. */
int mkdir_p(const char *path, mode_t mode);

/* Join a and b with the platform path separator between them (avoids
   doubling). Returns malloc'd. */
char *path_join(const char *a, const char *b);

/* Read whole file into b (binary). Returns 0 on success, -1 on error. */
int read_file(const char *path, sb_t *b);

/* Write data to path (creates/truncates, mode applied). Returns 0 on success. */
int write_file(const char *path, const char *data, size_t len, mode_t mode);

/* FNV-1a 64-bit hash of a string, hex-formatted into out[17]. */
void fnv1a64_hex(const char *s, char out[17]);

/* gzip-compress data (gzip framing, not zlib) into out. Returns 0 on success. */
int gzip_mem(const char *data, size_t len, sb_t *out);

/* debian policy version comparison: returns <0, 0, >0 like strcmp. */
int deb_vercmp(const char *a, const char *b);

/* Native machine as a Debian architecture name. Cached. */
const char *native_deb_arch(void);

/* ---------------- platform abstraction ---------------- */

/* Directory of the running executable (Windows: %EXEDIR% semantics), with a
   trailing path separator. Linux: static "/" (paths are fixed /etc|/var). */
const char *exe_dir(void);

/* User home directory (Windows: %USERDIR% = C:\Users\<user>; Linux: $HOME),
   no trailing separator. */
const char *user_dir(void);

/* Run argv[0..] as a subprocess, capturing stdout into b (binary-safe).
   stderr is inherited. Returns the exit status (0 on success, 127 if the
   program could not be run). */
int plat_run_capture(const char *const argv[], sb_t *b);

/* Run argv[0..] as a subprocess, inheriting stdio. Returns exit status. */
int plat_run_simple(const char *const argv[]);

/* Path separator character for the platform ('\\' on Windows, '/' else). */
char path_sep(void);

#endif
