#include "list.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

void sl_init(str_list *l)
{
    l->items = NULL; l->n = 0; l->cap = 0;
}

void sl_free(str_list *l)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        free(l->items[i]);
    free(l->items);
    sl_init(l);
}

int sl_push_n(str_list *l, const char *s, size_t n)
{
    if (l->n + 1 > l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 8;
        char **ni = realloc(l->items, ncap * sizeof(char *));
        if (!ni)
            return -1;
        l->items = ni;
        l->cap = ncap;
    }
    char *copy = xmalloc(n + 1);
    memcpy(copy, s, n);
    copy[n] = '\0';
    l->items[l->n++] = copy;
    return 0;
}

int sl_push(str_list *l, const char *s)
{
    return sl_push_n(l, s, strlen(s));
}

int sl_push_split(str_list *l, const char *src)
{
    const char *p = src;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
            p++;
        if (sl_push_n(l, start, (size_t)(p - start)) < 0)
            return -1;
    }
    return 0;
}
