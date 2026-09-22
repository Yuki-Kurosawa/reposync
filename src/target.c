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
    sl_free(&t->suites);
    sl_free(&t->comps);
    free(t->import_cmd);
    free(t->source_import_cmd);
    free(t->remove_cmd);
    t->path = t->uri = t->suite = t->comp = t->arch = NULL;
    t->import_cmd = t->source_import_cmd = t->remove_cmd = NULL;
    t->sync_sources = -1;
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
                if (c->source_import_cmd)
                    t->source_import_cmd = xstrdup(c->source_import_cmd);
                if (c->remove_cmd)
                    t->remove_cmd = xstrdup(c->remove_cmd);
            }
            t->sync_sources = rc.sync_sources;
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
    sl_init(&t->suites);
    sl_init(&t->comps);
    if (d822_get(r, "suites"))
        d822_get_list(r, "suites", &sl);
    if (sl.n == 0)
        sl_push(&sl, "trixie");
    t->suite = xstrdup(sl.items[0]); /* base = first declared suite */
    size_t si;
    for (si = 0; si < sl.n; si++)
        sl_push(&t->suites, sl.items[si]); /* full mapping set */
    sl_free(&sl);
    sl_init(&sl);
    if (d822_get(r, "components"))
        d822_get_list(r, "components", &sl);
    if (sl.n == 0)
        sl_push(&sl, "main");
    t->comp = xstrdup(sl.items[0]);
    for (si = 0; si < sl.n; si++)
        sl_push(&t->comps, sl.items[si]);
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

    /* installed set = union of every declared (suite, component) of the
       target; per package name the highest version wins across suites */
    size_t si, ci;
    sb_t url;
    sb_init(&url);
    for (si = 0; si < t->suites.n; si++) {
        for (ci = 0; ci < t->comps.n; ci++) {
            int got = 0;
            int i;
            for (i = 0; i < 2 && !got; i++) {
                sb_reset(&url);
                sb_printf(&url, "%s/dists/%s/%s/binary-%s/%s",
                          t->uri, t->suites.items[si], t->comps.items[ci],
                          t->arch, i == 0 ? "Packages.gz" : "Packages");
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
                    pkg_t *cur = pkgset_find(ps, p.name);
                    if (cur && deb_vercmp(cur->version, p.version) >= 0) {
                        pkg_free(&p);
                    } else {
                        pkgset_put(ps, &p);
                        pkg_free(&p);
                    }
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
        }
    }
    sb_free(&url);
    return 0;
}

int target_sources_state(const target_t *t, srcset_t *ss)
{
    srcset_init(ss);
    static const char *cands[] = { "Sources.gz", "Sources", "Sources.zst" };
    size_t si, ci;
    sb_t url;
    sb_init(&url);
    for (si = 0; si < t->suites.n; si++) {
        for (ci = 0; ci < t->comps.n; ci++) {
            int got = 0;
            int i;
            for (i = 0; i < 3 && !got; i++) {
                sb_reset(&url);
                sb_printf(&url, "%s/dists/%s/%s/source/%s",
                          t->uri, t->suites.items[si], t->comps.items[ci],
                          cands[i]);
                sb_t raw;
                sb_init(&raw);
                if (fetch_url_to_mem(url.s, &raw) < 0) {
                    sb_free(&raw);
                    continue;
                }
                sb_t plain;
                sb_init(&plain);
                const char *fmt = i == 0 ? "gz" : i == 2 ? "zst" : "";
                if (decompress_mem(raw.s, raw.len, fmt, &plain) < 0) {
                    sb_free(&raw);
                    sb_free(&plain);
                    continue;
                }
                d822_doc doc;
                if (d822_parse(plain.s, plain.len, &doc) < 0) {
                    sb_free(&raw);
                    sb_free(&plain);
                    continue;
                }
                size_t j;
                for (j = 0; j < doc.n; j++) {
                    const char *name = d822_get(&doc.recs[j], "package");
                    const char *ver = d822_get(&doc.recs[j], "version");
                    if (name && ver) {
                        srcpkg_t *have = srcset_find(ss, name);
                        if (!have || deb_vercmp(ver, have->version) > 0)
                            srcset_put(ss, name, ver);
                    }
                }
                d822_doc_free(&doc);
                sb_free(&raw);
                sb_free(&plain);
                got = 1;
            }
        }
    }
    sb_free(&url);
    return 0;
}

/* ---- suite/component mapping for sync (需求.md 套件映射) ---- */

static int str_in_list(const str_list *l, const char *s)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        if (strcmp(l->items[i], s) == 0)
            return 1;
    return 0;
}

int target_map_suite(const target_t *t, const char *src_suite,
                     const char *src_comp, char **out_suite, char **out_comp)
{
    *out_suite = NULL;
    *out_comp = NULL;
    if (!src_suite || !*src_suite || strcmp(src_suite, ".") == 0)
        return 0; /* flat/unknown sources cannot be mapped */

    /* 1. same-name: source (S,C) -> target (S,C) */
    if (str_in_list(&t->suites, src_suite) &&
        str_in_list(&t->comps, src_comp)) {
        *out_suite = xstrdup(src_suite);
        *out_comp = xstrdup(src_comp);
        return 1;
    }

    /* 2. variant fallback: only abc-* -> abc, and only when the target has
       NO suite S at all (otherwise the package would be split across the
       variant and the base suite). base = first declared suite. */
    size_t blen = strlen(t->suite);
    if (blen > 0 && strncmp(src_suite, t->suite, blen) == 0 &&
        src_suite[blen] == '-' && !str_in_list(&t->suites, src_suite) &&
        str_in_list(&t->suites, t->suite) &&
        str_in_list(&t->comps, src_comp)) {
        *out_suite = xstrdup(t->suite);
        *out_comp = xstrdup(src_comp);
        return 1;
    }

    /* 3. everything else (cross-component, base -> variant, unrelated
       suites) is not allowed */
    return 0;
}
