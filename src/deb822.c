#include "deb822.h"
#include "strbuf.h"
#include "util.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void rec_init(d822_rec *r)
{
    r->keys = NULL; r->vals = NULL; r->n = 0;
}

static void rec_free(d822_rec *r)
{
    size_t i;
    for (i = 0; i < r->n; i++) {
        free(r->keys[i]);
        free(r->vals[i]);
    }
    free(r->keys);
    free(r->vals);
    rec_init(r);
}

void d822_rec_free(d822_rec *r)
{
    rec_free(r);
}

static int rec_set(d822_rec *r, const char *key, size_t klen, const char *val, size_t vlen)
{
    if (r->n % 16 == 0) {
        size_t ncap = r->n + 16;
        char **nk = xrealloc(r->keys, ncap * sizeof(char *));
        char **nv = xrealloc(r->vals, ncap * sizeof(char *));
        r->keys = nk;
        r->vals = nv;
    }
    char *k = xmalloc(klen + 1);
    size_t i;
    for (i = 0; i < klen; i++)
        k[i] = (char)tolower((unsigned char)key[i]);
    k[klen] = '\0';
    char *v = xmalloc(vlen + 1);
    memcpy(v, val, vlen);
    v[vlen] = '\0';
    r->keys[r->n] = k;
    r->vals[r->n] = v;
    r->n++;
    return 0;
}

/* Replace the last field's value with old + "\n" + cont (continuation line). */
static int rec_append_cont(d822_rec *r, const char *cont, size_t clen)
{
    if (r->n == 0)
        return -1;
    char *old = r->vals[r->n - 1];
    size_t olen = strlen(old);
    char *nv = xmalloc(olen + 1 + clen + 1);
    memcpy(nv, old, olen);
    nv[olen] = '\n';
    memcpy(nv + olen + 1, cont, clen);
    nv[olen + 1 + clen] = '\0';
    free(old);
    r->vals[r->n - 1] = nv;
    return 0;
}

static int doc_push(d822_doc *d, d822_rec *r)
{
    if (d->n == d->cap) {
        size_t ncap = d->cap ? d->cap * 2 : 8;
        d->recs = xrealloc(d->recs, ncap * sizeof(d822_rec));
        d->cap = ncap;
    }
    d->recs[d->n] = *r;
    d->n++;
    return 0;
}

int d822_parse(const char *data, size_t len, d822_doc *doc)
{
    doc->recs = NULL; doc->n = 0; doc->cap = 0;
    d822_rec cur;
    rec_init(&cur);
    int has_rec = 0;

    size_t i = 0;
    while (i < len) {
        /* find end of line */
        size_t start = i;
        while (i < len && data[i] != '\n')
            i++;
        size_t elen = i - start;
        if (i < len)
            i++; /* consume '\n' */
        /* strip trailing '\r' */
        if (elen && data[start + elen - 1] == '\r')
            elen--;

        const char *line = data + start;

        if (elen == 0 || (elen == 1 && (line[0] == '\r'))) {
            /* blank line: end record */
            if (has_rec) {
                doc_push(doc, &cur);
                rec_init(&cur);
                has_rec = 0;
            }
            continue;
        }

        if (line[0] == ' ' || line[0] == '\t') {
            /* continuation line: skip leading whitespace, append */
            const char *p = line;
            const char *end = line + elen;
            while (p < end && (*p == ' ' || *p == '\t'))
                p++;
            if (has_rec && p < end) {
                if (rec_append_cont(&cur, p, (size_t)(end - p)) < 0)
                    return -1;
            }
            continue;
        }

        /* field line: Key: value */
        const char *colon = memchr(line, ':', elen);
        if (!colon || colon == line) {
            /* malformed line: skip silently */
            continue;
        }
        size_t klen = (size_t)(colon - line);
        /* trim key trailing spaces */
        while (klen && (line[klen - 1] == ' ' || line[klen - 1] == '\t'))
            klen--;
        const char *v = colon + 1;
        const char *vend = line + elen;
        while (v < vend && (*v == ' ' || *v == '\t'))
            v++;
        size_t vlen = (size_t)(vend - v);
        while (vlen && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t'))
            vlen--;
        if (klen == 0)
            continue;
        rec_set(&cur, line, klen, v, vlen);
        has_rec = 1;
    }

    if (has_rec)
        doc_push(doc, &cur);
    else
        rec_free(&cur);
    return 0;
}

void d822_doc_free(d822_doc *doc)
{
    size_t i;
    for (i = 0; i < doc->n; i++)
        rec_free(&doc->recs[i]);
    free(doc->recs);
    doc->recs = NULL; doc->n = 0; doc->cap = 0;
}

const char *d822_get(const d822_rec *r, const char *key)
{
    size_t i;
    for (i = 0; i < r->n; i++) {
        if (strcasecmp(r->keys[i], key) == 0)
            return r->vals[i];
    }
    return NULL;
}

int d822_get_list(const d822_rec *r, const char *key, str_list *l)
{
    const char *v = d822_get(r, key);
    if (!v)
        return 0;
    return sl_push_split(l, v);
}

int d822_has_token(const d822_rec *r, const char *key, const char *tok)
{
    const char *v = d822_get(r, key);
    if (!v)
        return 0;
    str_list l;
    sl_init(&l);
    sl_push_split(&l, v);
    int found = 0;
    size_t i;
    for (i = 0; i < l.n; i++) {
        if (strcmp(l.items[i], tok) == 0) {
            found = 1;
            break;
        }
    }
    sl_free(&l);
    return found;
}

void d822_rec_copy(d822_rec *dst, const d822_rec *src)
{
    rec_init(dst);
    size_t i;
    for (i = 0; i < src->n; i++)
        rec_set(dst, src->keys[i], strlen(src->keys[i]),
                src->vals[i], strlen(src->vals[i]));
}

int d822_rec_set(d822_rec *r, const char *key, const char *val)
{
    size_t i;
    for (i = 0; i < r->n; i++) {
        if (strcasecmp(r->keys[i], key) == 0) {
            char *nv = xstrdup(val);
            free(r->vals[i]);
            r->vals[i] = nv;
            return 0;
        }
    }
    return rec_set(r, key, strlen(key), val, strlen(val));
}

int d822_rec_append_text(sb_t *b, const d822_rec *r)
{
    size_t i;
    for (i = 0; i < r->n; i++) {
        /* standard DEB822 output: first letter capitalized (keys stored lower) */
        if (sb_append_char(b, (char)toupper((unsigned char)r->keys[i][0])) < 0)
            return -1;
        if (sb_append_str(b, r->keys[i] + 1) < 0)
            return -1;
        if (sb_append(b, ": ", 2) < 0)
            return -1;
        const char *v = r->vals[i];
        const char *p = v;
        while (*p) {
            if (*p == '\n') {
                if (sb_append(b, "\n ", 2) < 0)
                    return -1;
            } else if (sb_append_char(b, *p) < 0) {
                return -1;
            }
            p++;
        }
        if (sb_append_char(b, '\n') < 0)
            return -1;
    }
    return sb_append_char(b, '\n');
}
