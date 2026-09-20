#include "index.h"
#include "deb822.h"
#include "fetch.h"
#include "util.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void src_free(src_t *s)
{
    free(s->uri);
    free(s->suite);
    sl_free(&s->comps);
    sl_free(&s->arches);
}

void src_free_all(src_t *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        src_free(&s[i]);
    free(s);
}

static int src_from_rec(const d822_rec *r, src_t **out, size_t *n)
{
    if (!d822_has_token(r, "types", "deb"))
        return 0;

    const char *uris_v = d822_get(r, "uris");
    if (!uris_v)
        return 0;

    str_list uris, suites, comps, arches;
    sl_init(&uris); sl_init(&suites); sl_init(&comps); sl_init(&arches);
    d822_get_list(r, "uris", &uris);

    /* HTTP(S) sources are preferred over file: when a source lists several
       URIs, the http(s) ones are tried first (same behaviour as apt with
       multiple URIs). fetch() is libcurl either way. */
    {
        size_t w = 0, ui;
        for (ui = 0; ui < uris.n; ui++) {
            if (strncmp(uris.items[ui], "http://", 7) == 0 ||
                strncmp(uris.items[ui], "https://", 8) == 0) {
                char *t = uris.items[w];
                uris.items[w] = uris.items[ui];
                uris.items[ui] = t;
                w++;
            }
        }
    }
    d822_get_list(r, "suites", &suites);
    d822_get_list(r, "components", &comps);
    if (d822_get(r, "architectures")) {
        d822_get_list(r, "architectures", &arches);
    } else {
        /* default to native architecture */
        sl_push(&arches, native_deb_arch());
    }

    size_t i;
    for (i = 0; i < uris.n; i++) {
        src_t *ns = xrealloc(*out, (*n + 1) * sizeof(src_t));
        *out = ns;
        src_t *s = &(*out)[*n];
        memset(s, 0, sizeof *s);
        s->uri = xstrdup(uris.items[i]);
        sl_init(&s->comps);
        sl_init(&s->arches);

        /* expand suites (default "." flat) */
        if (suites.n == 0) {
            s->suite = xstrdup(".");
        } else {
            s->suite = xstrdup(suites.items[0]);
        }

        /* components: if empty, flat repo (suite ".") has no component dir */
        for (size_t j = 0; j < comps.n; j++)
            sl_push(&s->comps, comps.items[j]);
        for (size_t j = 0; j < arches.n; j++)
            sl_push(&s->arches, arches.items[j]);
        (*n)++;
    }

    sl_free(&uris); sl_free(&suites); sl_free(&comps); sl_free(&arches);
    return 0;
}

int src_load_dir(const char *dir, src_t **out, size_t *n)
{
    *out = NULL;
    *n = 0;
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "reposync: cannot open config dir %s: %s\n", dir, strerror(errno));
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        if (nl < 9 || strcmp(de->d_name + nl - 8, ".sources") != 0)
            continue;
        char *path = path_join(dir, de->d_name);
        sb_t content;
        sb_init(&content);
        if (read_file(path, &content) < 0) {
            fprintf(stderr, "reposync: cannot read %s\n", path);
            free(path);
            sb_free(&content);
            continue;
        }
        d822_doc doc;
        if (d822_parse(content.s, content.len, &doc) < 0) {
            fprintf(stderr, "reposync: parse error in %s\n", path);
            free(path);
            sb_free(&content);
            continue;
        }
        size_t i;
        for (i = 0; i < doc.n; i++)
            src_from_rec(&doc.recs[i], out, n);
        d822_doc_free(&doc);
        free(path);
        sb_free(&content);
    }
    closedir(d);
    return 0;
}

/* ---------- update ---------- */

static int is_flat(const src_t *s)
{
    return strcmp(s->suite, ".") == 0 || s->suite[0] == '\0';
}

/* Count DEB822 records by counting blank-line separators. */
static size_t count_records(const char *data, size_t len)
{
    size_t recs = 0;
    size_t i = 0;
    int in_field = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && data[i] != '\n')
            i++;
        size_t elen = i - start;
        if (i < len)
            i++;
        if (elen && data[start + elen - 1] == '\r')
            elen--;
        if (elen == 0) {
            if (in_field) {
                recs++;
                in_field = 0;
            }
        } else if (data[start] != ' ' && data[start] != '\t') {
            in_field = 1;
        }
    }
    if (in_field)
        recs++;
    return recs;
}

static void meta_write(const char *list_dir, const char *name,
                       const char *kind, const src_t *s,
                       const char *comp, const char *arch)
{
    sb_t mname;
    sb_init(&mname);
    sb_printf(&mname, "%s.meta", name);
    char *path = path_join(list_dir, mname.s);
    sb_free(&mname);
    sb_t m;
    sb_init(&m);
    sb_printf(&m, "Kind: %s\nURI: %s\nSuite: %s\n", kind, s->uri, s->suite);
    if (comp)
        sb_printf(&m, "Component: %s\n", comp);
    if (arch)
        sb_printf(&m, "Architecture: %s\n", arch);
    write_file(path, m.s, m.len, 0644);
    free(path);
    sb_free(&m);
}

static int fetch_one_index(const char *list_dir, const src_t *s,
                           const char *comp, const char *arch)
{
    char keybuf[4096];
    const char *base = is_flat(s) ? "" : "dists/";
    const char *mid = is_flat(s) ? "" : "/binary-";
    sb_t url, baseurl;
    sb_init(&url); sb_init(&baseurl);

    if (is_flat(s)) {
        sb_printf(&baseurl, "%s/", s->uri);
        snprintf(keybuf, sizeof keybuf, "packages|%s|flat", s->uri);
    } else {
        sb_printf(&baseurl, "%s/%s%s/%s%s%s/", s->uri, base, s->suite, comp, mid, arch);
        snprintf(keybuf, sizeof keybuf, "packages|%s|%s|%s|%s", s->uri, s->suite, comp, arch);
    }

    char hash[17];
    fnv1a64_hex(keybuf, hash);
    char fname[64];
    snprintf(fname, sizeof fname, "%s_Packages", hash);
    char *cache_path = path_join(list_dir, fname);

    static const char *cands[] = { "Packages.gz", "Packages", "Packages.zst", NULL };
    int i;
    for (i = 0; cands[i]; i++) {
        sb_reset(&url);
        sb_append_str(&url, baseurl.s);
        sb_append_str(&url, cands[i]);
        sb_t raw;
        sb_init(&raw);
        if (fetch_url_to_mem(url.s, &raw) < 0) {
            sb_free(&raw);
            continue;
        }
        const char *fmt = "";
        if (strcmp(cands[i], "Packages.gz") == 0)
            fmt = "gz";
        else if (strcmp(cands[i], "Packages.zst") == 0)
            fmt = "zst";
        sb_t plain;
        sb_init(&plain);
        if (decompress_mem(raw.s, raw.len, fmt, &plain) < 0) {
            fprintf(stderr, "reposync: decompress failed for %s\n", url.s);
            sb_free(&raw);
            sb_free(&plain);
            continue;
        }
        if (write_file(cache_path, plain.s, plain.len, 0644) < 0) {
            fprintf(stderr, "reposync: write %s failed\n", cache_path);
        }
        meta_write(list_dir, fname, "Packages", s, comp, arch);
        printf("  got %s (raw %zu -> %zu bytes, %zu packages)\n", url.s, raw.len, plain.len, count_records(plain.s, plain.len));
        sb_free(&raw);
        sb_free(&plain);
        free(cache_path);
        sb_free(&url); sb_free(&baseurl);
        return 0;
    }

    fprintf(stderr, "reposync: no Packages index found at %s\n", baseurl.s);
    free(cache_path);
    sb_free(&url); sb_free(&baseurl);
    return -1;
}

static int fetch_one_release(const char *list_dir, const src_t *s)
{
    if (is_flat(s))
        return 0;
    char keybuf[4096];
    snprintf(keybuf, sizeof keybuf, "release|%s|%s", s->uri, s->suite);
    char hash[17];
    fnv1a64_hex(keybuf, hash);
    char fname[64];
    snprintf(fname, sizeof fname, "%s_Release", hash);
    char *cache_path = path_join(list_dir, fname);

    sb_t url;
    sb_init(&url);
    sb_printf(&url, "%s/dists/%s/Release", s->uri, s->suite);

    sb_t raw;
    sb_init(&raw);
    if (fetch_url_to_mem(url.s, &raw) < 0) {
        fprintf(stderr, "reposync: cannot fetch %s\n", url.s);
        free(cache_path);
        sb_free(&url);
        sb_free(&raw);
        return -1;
    }
    write_file(cache_path, raw.s, raw.len, 0644);
    meta_write(list_dir, fname, "Release", s, NULL, NULL);
    printf("  got %s (%zu bytes)\n", url.s, raw.len);
    free(cache_path);
    sb_free(&url);
    sb_free(&raw);
    return 0;
}

int idx_update(const char *list_dir, src_t *srcs, size_t n)
{
    if (mkdir_p(list_dir, 0755) < 0) {
        fprintf(stderr, "reposync: cannot create %s: %s\n", list_dir, strerror(errno));
        return -1;
    }
    size_t i, j, k;
    int errors = 0;
    for (i = 0; i < n; i++) {
        src_t *s = &srcs[i];
        printf("Source: %s  suite=%s\n", s->uri, s->suite);
        for (j = 0; j < s->comps.n; j++) {
            for (k = 0; k < s->arches.n; k++) {
                if (fetch_one_index(list_dir, s, s->comps.items[j], s->arches.items[k]) < 0)
                    errors++;
            }
        }
        if (fetch_one_release(list_dir, s) < 0) {
            /* Release is advisory for unauthenticated/local sources: the
               Packages index already succeeded, so only warn. */
            fprintf(stderr, "reposync: warning: no Release index for %s\n", s->uri);
        }
    }
    return errors ? -1 : 0;
}

/* ---------- download ---------- */

typedef struct {
    char *version;
    char *filename;
    char *arch;
    long long size;
    int has_size;
    char *uri;
    char *suite;
} cand_t;

static void cand_free(cand_t *c)
{
    free(c->version); free(c->filename); free(c->arch); free(c->uri); free(c->suite);
}

static int read_meta(const char *meta_path, char **uri, char **suite)
{
    sb_t m;
    sb_init(&m);
    if (read_file(meta_path, &m) < 0) {
        sb_free(&m);
        return -1;
    }
    d822_doc doc;
    d822_parse(m.s, m.len, &doc);
    const char *u = NULL, *su = NULL;
    if (doc.n > 0) {
        u = d822_get(&doc.recs[0], "uri");
        su = d822_get(&doc.recs[0], "suite");
    }
    if (u)
        *uri = xstrdup(u);
    if (su)
        *suite = xstrdup(su);
    d822_doc_free(&doc);
    sb_free(&m);
    return (u && *uri) ? 0 : -1;
}

static int arch_ok(const char *arch, const char *want)
{
    if (!arch || !*arch)
        return 1;
    return strcmp(arch, want) == 0 || strcmp(arch, "all") == 0;
}

int idx_download(const char *list_dir, const char *pkg, const char *outdir, char **saved_path)
{
    *saved_path = NULL;
    DIR *d = opendir(list_dir);
    if (!d) {
        fprintf(stderr, "reposync: cannot open %s: %s\n", list_dir, strerror(errno));
        return -1;
    }
    const char *want_arch = native_deb_arch();
    cand_t best;
    memset(&best, 0, sizeof best);

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        if (nl < 10 || strcmp(de->d_name + nl - 9, "_Packages") != 0)
            continue;
        char *p = path_join(list_dir, de->d_name);
        sb_t content;
        sb_init(&content);
        if (read_file(p, &content) < 0) {
            free(p);
            sb_free(&content);
            continue;
        }
        /* meta next to it: <file>.meta */
        char *mp = xmalloc(strlen(p) + 1 + 5 + 1);
        sprintf(mp, "%s.meta", p);
        char *muri = NULL, *msuite = NULL;
        read_meta(mp, &muri, &msuite);

        d822_doc doc;
        d822_parse(content.s, content.len, &doc);
        size_t i;
        for (i = 0; i < doc.n; i++) {
            const d822_rec *r = &doc.recs[i];
            const char *name = d822_get(r, "package");
            if (!name || strcmp(name, pkg) != 0)
                continue;
            const char *ver = d822_get(r, "version");
            const char *fn = d822_get(r, "filename");
            const char *arch = d822_get(r, "architecture");
            if (!ver || !fn)
                continue;
            if (!arch_ok(arch, want_arch))
                continue;
            cand_t c;
            memset(&c, 0, sizeof c);
            c.version = xstrdup(ver);
            c.filename = xstrdup(fn);
            c.arch = arch ? xstrdup(arch) : NULL;
            const char *sz = d822_get(r, "size");
            if (sz) {
                c.size = strtoll(sz, NULL, 10);
                c.has_size = 1;
            }
            c.uri = muri ? xstrdup(muri) : NULL;
            c.suite = msuite ? xstrdup(msuite) : NULL;
            if (!best.filename || deb_vercmp(c.version, best.version) > 0) {
                cand_free(&best);
                best = c;
            } else {
                cand_free(&c);
            }
        }
        d822_doc_free(&doc);
        free(muri);
        free(msuite);
        free(mp);
        free(p);
        sb_free(&content);
    }
    closedir(d);

    if (!best.filename) {
        fprintf(stderr, "reposync: package '%s' not found in cached indexes (run update first)\n", pkg);
        return -1;
    }
    if (!best.uri) {
        fprintf(stderr, "reposync: no source URI recorded for %s\n", pkg);
        cand_free(&best);
        return -1;
    }

    sb_t url;
    sb_init(&url);
    sb_printf(&url, "%s/%s", best.uri, best.filename);
    const char *bn = strrchr(best.filename, '/');
    bn = bn ? bn + 1 : best.filename;
    char *out = path_join(outdir, bn);
    printf("  downloading %s\n", url.s);
    if (fetch_url_to_file(url.s, out) < 0) {
        fprintf(stderr, "reposync: download failed: %s\n", url.s);
        free(out);
        sb_free(&url);
        cand_free(&best);
        return -1;
    }
    if (best.has_size) {
        struct stat st;
        if (stat(out, &st) == 0 && (long long)st.st_size != best.size) {
            fprintf(stderr, "reposync: size mismatch for %s (%lld != %lld), removing\n",
                    out, (long long)st.st_size, best.size);
            unlink(out);
            free(out);
            sb_free(&url);
            cand_free(&best);
            return -1;
        }
    }
    printf("  saved %s\n", out);
    *saved_path = out;
    sb_free(&url);
    cand_free(&best);
    return 0;
}

int idx_best(const char *list_dir, const char *pkg, d822_rec *out)
{
    DIR *d = opendir(list_dir);
    if (!d) {
        fprintf(stderr, "reposync: cannot open %s: %s\n", list_dir, strerror(errno));
        return -1;
    }
    const char *want_arch = native_deb_arch();
    d822_rec best;
    memset(&best, 0, sizeof best);
    const char *best_ver = NULL;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        if (nl < 10 || strcmp(de->d_name + nl - 9, "_Packages") != 0)
            continue;
        char *p = path_join(list_dir, de->d_name);
        sb_t content;
        sb_init(&content);
        if (read_file(p, &content) < 0) {
            free(p);
            sb_free(&content);
            continue;
        }
        d822_doc doc;
        d822_parse(content.s, content.len, &doc);
        size_t i;
        for (i = 0; i < doc.n; i++) {
            const d822_rec *r = &doc.recs[i];
            const char *name = d822_get(r, "package");
            if (!name || strcmp(name, pkg) != 0)
                continue;
            const char *ver = d822_get(r, "version");
            const char *arch = d822_get(r, "architecture");
            if (!ver)
                continue;
            if (!arch_ok(arch, want_arch))
                continue;
            if (!best_ver || deb_vercmp(ver, best_ver) > 0) {
                if (best.n)
                    d822_rec_free(&best);
                d822_rec_copy(&best, r);
                best_ver = d822_get(&best, "version");
            }
        }
        d822_doc_free(&doc);
        free(p);
        sb_free(&content);
    }
    closedir(d);

    if (!best_ver) {
        fprintf(stderr, "reposync: package '%s' not found in cached indexes (run update first)\n", pkg);
        return -1;
    }
    *out = best;
    return 0;
}
