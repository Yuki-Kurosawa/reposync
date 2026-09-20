#ifndef REPOSYNC_FETCH_H
#define REPOSYNC_FETCH_H

#include <stddef.h>
#include "strbuf.h"

/* Download url into memory b (binary). Returns 0 on success. */
int fetch_url_to_mem(const char *url, sb_t *b);

/* Download url to file path (curl -o, atomic via temp+rename). Returns 0 on success. */
int fetch_url_to_file(const char *url, const char *path);

/* Decompress data (gz / zst / plain) into out. fmt: "gz", "zst" or "". Returns 0. */
int decompress_mem(const char *data, size_t len, const char *fmt, sb_t *out);

#endif
