#include "target.h"
#include "config.h"
#include "deb822.h"
#include "fetch.h"
#include "sim.h"
#include "util.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void target_free(target_t *t)
{
    free(t->path);
    free(t->uri);
    free(t->suite);
    free(t->comp);
    free(t->arch);
    free(t->import_cmd);
    free(t->remove_cmd);
    t->path = t->uri = t->suite = t->comp = t->arch = NULL;
    t->import_cmd = t->remove_cmd = NULL;
}

static int is_file_uri(const char *u)
{
    return strncmp(u, "file:", 5) == 0 || strncmp(u, "file://", 7) == 0;
}

int target_load(const char *sources_path, const char *conf_path, target_t *t)
{
    memset(t, 0, sizeof *t);

    /* --- reposync.conf: repo control command sets (Path/Import-Cmd/
       Remove-Cmd per Repo-Type), active type selected by Active-Repo --- */
    if (conf_path && *conf_path) {
        rconf_t rc;
        rconf_init(&rc);
        if (rconf_load(conf_path, &rc) == 0) {
            const rcmd_t *c = rconf_active(&rc);
            if (c) {
                if (c->path && *c->path)
                    t->path = xstrdup(c->path);
                if (c->import_cmd)
                    t->import_cmd = xstrdup(c->import_cmd);
                if (c->remove_cmd)
                    t->remove_cmd = xstrdup(c->remove_cmd);
            }
        }
        rconf_free(&rc);
    }

    /* --- target.sources: apt source view --- */
    sb_t content;
    sb_init(&content);
    if (read_file(sources_path, &content) < 0) {
        fprintf(stderr, "reposync: cannot read target config %s: %s\n",
                sources_path, strerror(errno));
        sb_free(&content);
        return -1;
    }
    d822_doc doc;
    if (d822_parse(content.s, content.len, &doc) < 0 || doc.n == 0) {
        fprintf(stderr, "reposync: parse error in %s\n", sources_path);
        d822_doc_free(&doc);
        sb_free(&content);
        return -1;
    }
    const d822_rec *r = &doc.recs[0];
    const char *path = d822_get(r, "path");
    const char *uris = d822_get(r, "uris");

    if (uris) {
        str_list l;
        sl_init(&l);
        d822_get_list(r, "uris", &l);
        if (l.n > 0)
            t->uri = xstrdup(l.items[0]);
        sl_free(&l);
    }
    if (!t->uri || !*t->uri) {
        fprintf(stderr, "reposync: target.sources needs URIs:\n");
        d822_doc_free(&doc);
        sb_free(&content);
        return -1;
    }

    if (!t->path && path && *path)
        t->path = xstrdup(path);
    if (!t->path && is_file_uri(t->uri)) {
        const char *p = t->uri;
        if (strncmp(p, "file://", 7) == 0)
            p += 7;
        else if (strncmp(p, "file:", 5) == 0)
            p += 5;
        t->path = xstrdup(p);
    }
    if (t->path) {
        size_t pl = strlen(t->path);
        while (pl > 1 && t->path[pl - 1] == '/')
            t->path[--pl] = '\0';
    }

    str_list sl;
    sl_init(&sl);
    if (d822_get(r, "suites"))
        d822_get_list(r, "suites", &sl);
    t->suite = sl.n ? xstrdup(sl.items[0]) : xstrdup("trixie");
    sl_free(&sl);
    sl_init(&sl);
    if (d822_get(r, "components"))
        d822_get_list(r, "components", &sl);
    t->comp = sl.n ? xstrdup(sl.items[0]) : xstrdup("main");
    sl_free(&sl);
    sl_init(&sl);
    if (d822_get(r, "architectures"))
        d822_get_list(r, "architectures", &sl);
    t->arch = sl.n ? xstrdup(sl.items[0]) : xstrdup(native_deb_arch());
    sl_free(&sl);

    d822_doc_free(&doc);
    sb_free(&content);
    return 0;
}

static char *dup_or_null(const char *s)
{
    return s ? xstrdup(s) : NULL;
}

static void rec_to_pkg(const d822_rec *r, pkg_t *p)
{
    pkg_init(p);
    p->name = dup_or_null(d822_get(r, "package"));
    p->version = dup_or_null(d822_get(r, "version"));
    p->arch = dup_or_null(d822_get(r, "architecture"));
    p->status = xstrdup("install ok installed");
    p->depends = dup_or_null(d822_get(r, "depends"));
    p->pre_depends = dup_or_null(d822_get(r, "pre-depends"));
    p->recommends = dup_or_null(d822_get(r, "recommends"));
    p->suggests = dup_or_null(d822_get(r, "suggests"));
    p->provides = dup_or_null(d822_get(r, "provides"));
    p->conflicts = dup_or_null(d822_get(r, "conflicts"));
    p->breaks = dup_or_null(d822_get(r, "breaks"));
}

int target_state(const target_t *t, pkgset_t *ps, d822_doc *doc_out)
{
    pkgset_init(ps);
    if (doc_out) {
        doc_out->recs = NULL;
        doc_out->n = 0;
        doc_out->cap = 0;
    }

    int got = 0;
    int i;
    sb_t url;
    sb_init(&url);
    for (i = 0; i < 2 && !got; i++) {
        sb_reset(&url);
        sb_printf(&url, "%s/dists/%s/%s/binary-%s/%s",
                  t->uri, t->suite, t->comp, t->arch,
                  i == 0 ? "Packages.gz" : "Packages");
        sb_t raw;
        sb_init(&raw);
        if (fetch_url_to_mem(url.s, &raw) < 0) {
            sb_free(&raw);
            continue;
        }
        sb_t plain;
        sb_init(&plain);
        if (decompress_mem(raw.s, raw.len, i == 0 ? "gz" : "", &plain) < 0) {
            sb_free(&raw);
            sb_free(&plain);
            continue;
        }
        d822_doc doc;
        d822_parse(plain.s, plain.len, &doc);
        size_t j;
        for (j = 0; j < doc.n; j++) {
            pkg_t p;
            rec_to_pkg(&doc.recs[j], &p);
            pkgset_put(ps, &p);
            pkg_free(&p);
            if (doc_out) {
                d822_rec *nr = xrealloc(doc_out->recs,
                                        (doc_out->n + 1) * sizeof(d822_rec));
                doc_out->recs = nr;
                d822_rec_copy(&doc_out->recs[doc_out->n], &doc.recs[j]);
                doc_out->n++;
            }
        }
        d822_doc_free(&doc);
        sb_free(&raw);
        sb_free(&plain);
        got = 1;
    }
    sb_free(&url);
    return 0;
}
