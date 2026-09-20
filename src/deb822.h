#ifndef REPOSYNC_DEB822_H
#define REPOSYNC_DEB822_H

#include <stddef.h>
#include "list.h"
#include "strbuf.h"

/* One RFC822-style record: parallel arrays of keys and values. */
typedef struct {
    char **keys;
    char **vals;
    size_t n;
} d822_rec;

typedef struct {
    d822_rec *recs;
    size_t n;
    size_t cap;
} d822_doc;

/* Parse a DEB822 document. Continuation lines (leading space/tab) are merged
   into the previous value joined with '\n'. Records are separated by blank
   lines. Keys are stored lowercased. Returns 0 on success. */
int d822_parse(const char *data, size_t len, d822_doc *doc);
void d822_doc_free(d822_doc *doc);

/* First value for key (case-insensitive), or NULL. */
const char *d822_get(const d822_rec *r, const char *key);

/* Split value on whitespace into l. */
int d822_get_list(const d822_rec *r, const char *key, str_list *l);

/* 1 if value (whitespace-split) contains the token tok. */
int d822_has_token(const d822_rec *r, const char *key, const char *tok);

/* Deep copy src into dst (dst must be uninitialized or freed). */
void d822_rec_copy(d822_rec *dst, const d822_rec *src);

/* Set key to val: replace existing field or append a new one. */
int d822_rec_set(d822_rec *r, const char *key, const char *val);

/* Free one record's internal arrays. */
void d822_rec_free(d822_rec *r);

/* Serialize record (DEB822, multi-line values with proper continuation
   indentation) into b, terminated by a blank line. Returns 0 on success. */
int d822_rec_append_text(sb_t *b, const d822_rec *r);

#endif
