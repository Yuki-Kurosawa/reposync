#ifndef REPOSYNC_LIST_H
#define REPOSYNC_LIST_H

#include <stddef.h>

/* Simple dynamic array of NUL-terminated strings (copied on push). */
typedef struct {
    char **items;
    size_t n;
    size_t cap;
} str_list;

void sl_init(str_list *l);
void sl_free(str_list *l);
int sl_push(str_list *l, const char *s);
int sl_push_n(str_list *l, const char *s, size_t n);
/* Append copy of every token of src split on any run of whitespace. */
int sl_push_split(str_list *l, const char *src);

#endif
