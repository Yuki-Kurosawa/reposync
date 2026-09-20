#ifndef REPOSYNC_STRBUF_H
#define REPOSYNC_STRBUF_H

#include <stddef.h>

typedef struct {
    char *s;      /* NUL-terminated */
    size_t len;
    size_t cap;
} sb_t;

void sb_init(sb_t *b);
void sb_free(sb_t *b);
int sb_reserve(sb_t *b, size_t extra);
int sb_append(sb_t *b, const char *data, size_t n);
int sb_append_str(sb_t *b, const char *str);
int sb_append_char(sb_t *b, char c);
int sb_printf(sb_t *b, const char *fmt, ...);
void sb_reset(sb_t *b);
char *sb_detach(sb_t *b); /* returns NUL-terminated string, resets b */

#endif
