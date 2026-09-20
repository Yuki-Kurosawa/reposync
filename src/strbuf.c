#include "strbuf.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SB_INIT_CAP 128

void sb_init(sb_t *b)
{
    b->s = malloc(SB_INIT_CAP);
    if (!b->s) {
        b->s = NULL; b->len = 0; b->cap = 0;
        return;
    }
    b->s[0] = '\0';
    b->len = 0;
    b->cap = SB_INIT_CAP;
}

void sb_free(sb_t *b)
{
    free(b->s);
    b->s = NULL; b->len = 0; b->cap = 0;
}

int sb_reserve(sb_t *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return 0;
    size_t ncap = b->cap ? b->cap : SB_INIT_CAP;
    while (b->len + extra + 1 > ncap)
        ncap *= 2;
    char *ns = realloc(b->s, ncap);
    if (!ns)
        return -1;
    b->s = ns;
    b->cap = ncap;
    return 0;
}

int sb_append(sb_t *b, const char *data, size_t n)
{
    if (sb_reserve(b, n) < 0)
        return -1;
    memcpy(b->s + b->len, data, n);
    b->len += n;
    b->s[b->len] = '\0';
    return 0;
}

int sb_append_str(sb_t *b, const char *str)
{
    return sb_append(b, str, strlen(str));
}

int sb_append_char(sb_t *b, char c)
{
    if (sb_reserve(b, 1) < 0)
        return -1;
    b->s[b->len++] = c;
    b->s[b->len] = '\0';
    return 0;
}

int sb_printf(sb_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return -1;
    }
    if (sb_reserve(b, (size_t)need) < 0) {
        va_end(ap2);
        return -1;
    }
    vsnprintf(b->s + b->len, b->cap - b->len, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)need;
    return 0;
}

void sb_reset(sb_t *b)
{
    if (b->s) {
        b->s[0] = '\0';
        b->len = 0;
    } else {
        sb_init(b);
    }
}

char *sb_detach(sb_t *b)
{
    char *r = b->s;
    sb_init(b);
    return r;
}
