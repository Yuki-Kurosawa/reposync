#include "sim.h"
#include "db.h"
#include "deb822.h"
#include "index.h"
#include "list.h"
#include "util.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- pkg / pkgset ---------------- */

void pkg_init(pkg_t *p)
{
    memset(p, 0, sizeof *p);
}

static char *dup_or_null(const char *s)
{
    return s ? xstrdup(s) : NULL;
}

void pkg_copy(pkg_t *dst, const pkg_t *src)
{
    dst->name = dup_or_null(src->name);
    dst->version = dup_or_null(src->version);
    dst->arch = dup_or_null(src->arch);
    dst->status = dup_or_null(src->status);
    dst->essential = src->essential;
    dst->multi_arch = dup_or_null(src->multi_arch);
    dst->section = dup_or_null(src->section);
    dst->priority = dup_or_null(src->priority);
    dst->source = dup_or_null(src->source);
    dst->depends = dup_or_null(src->depends);
    dst->pre_depends = dup_or_null(src->pre_depends);
    dst->recommends = dup_or_null(src->recommends);
    dst->suggests = dup_or_null(src->suggests);
    dst->conflicts = dup_or_null(src->conflicts);
    dst->breaks = dup_or_null(src->breaks);
    dst->replaces = dup_or_null(src->replaces);
    dst->provides = dup_or_null(src->provides);
    dst->description = dup_or_null(src->description);
}

void pkg_free(pkg_t *p)
{
    free(p->name); free(p->version); free(p->arch); free(p->status);
    free(p->multi_arch); free(p->section); free(p->priority); free(p->source);
    free(p->depends); free(p->pre_depends); free(p->recommends); free(p->suggests);
    free(p->conflicts); free(p->breaks); free(p->replaces); free(p->provides);
    free(p->description);
    memset(p, 0, sizeof *p);
}

static unsigned long long str_hash(const char *s)
{
    unsigned long long h = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        h ^= (unsigned long long)*p++;
        h *= 1099511628211ULL;
    }
    return h;
}

void pkgset_init(pkgset_t *ps)
{
    memset(ps, 0, sizeof *ps);
}

void pkgset_free(pkgset_t *ps)
{
    size_t i;
    for (i = 0; i < ps->n; i++)
        pkg_free(&ps->items[i]);
    free(ps->items);
    free(ps->hash);
    memset(ps, 0, sizeof *ps);
}

static int ps_rehash(pkgset_t *ps, size_t hcap)
{
    int *h = calloc(hcap, sizeof(int));
    if (!h)
        return -1;
    free(ps->hash);
    ps->hash = h;
    ps->hcap = hcap;
    size_t mask = hcap - 1;
    size_t i;
    for (i = 0; i < ps->n; i++) {
        size_t slot = (size_t)(str_hash(ps->items[i].name) & mask);
        while (ps->hash[slot])
            slot = (slot + 1) & mask;
        ps->hash[slot] = (int)(i + 1);
    }
    return 0;
}

static int ps_probe(const pkgset_t *ps, const char *name)
{
    if (!ps->hash)
        return 0;
    size_t mask = ps->hcap - 1;
    size_t slot = (size_t)(str_hash(name) & mask);
    while (ps->hash[slot]) {
        size_t idx = (size_t)(ps->hash[slot] - 1);
        if (strcmp(ps->items[idx].name, name) == 0)
            return ps->hash[slot];
        slot = (slot + 1) & mask;
    }
    return 0;
}

pkg_t *pkgset_find(pkgset_t *ps, const char *name)
{
    int r = ps_probe(ps, name);
    if (!r)
        return NULL;
    return &ps->items[(size_t)(r - 1)];
}

int pkgset_put(pkgset_t *ps, const pkg_t *p)
{
    if (!ps->hash) {
        ps->hash = calloc(64, sizeof(int));
        if (!ps->hash)
            return -1;
        ps->hcap = 64;
    }
    int r = ps_probe(ps, p->name);
    if (r) {
        pkg_free(&ps->items[(size_t)(r - 1)]);
        pkg_copy(&ps->items[(size_t)(r - 1)], p);
        return 0;
    }
    if (ps->n + 1 > ps->hcap * 7 / 10) {
        if (ps_rehash(ps, ps->hcap * 2) < 0)
            return -1;
    }
    size_t mask = ps->hcap - 1;
    size_t slot = (size_t)(str_hash(p->name) & mask);
    while (ps->hash[slot])
        slot = (slot + 1) & mask;
    if (ps->n == ps->cap) {
        size_t ncap = ps->cap ? ps->cap * 2 : 256;
        pkg_t *ni = realloc(ps->items, ncap * sizeof(pkg_t));
        if (!ni)
            return -1;
        ps->items = ni;
        ps->cap = ncap;
    }
    pkg_init(&ps->items[ps->n]);
    pkg_copy(&ps->items[ps->n], p);
    ps->hash[slot] = (int)(ps->n + 1);
    ps->n++;
    return 0;
}

int pkgset_del(pkgset_t *ps, const char *name)
{
    int r = ps_probe(ps, name);
    if (!r)
        return 0;
    size_t idx = (size_t)(r - 1);
    pkg_free(&ps->items[idx]);
    ps->n--;
    if (idx < ps->n)
        ps->items[idx] = ps->items[ps->n];
    return ps_rehash(ps, ps->hcap) == 0 ? 1 : -1;
}

/* ---------------- candset ---------------- */

void candset_init(candset_t *cs)
{
    memset(cs, 0, sizeof *cs);
}

void candset_free(candset_t *cs)
{
    size_t i;
    for (i = 0; i < cs->n; i++) {
        pkg_free(&cs->items[i].p);
        free(cs->items[i].suite);
        free(cs->items[i].comp);
    }
    free(cs->items);
    free(cs->hash);
    memset(cs, 0, sizeof *cs);
}

static int cs_rehash(candset_t *cs, size_t hcap)
{
    int *h = calloc(hcap, sizeof(int));
    if (!h)
        return -1;
    free(cs->hash);
    cs->hash = h;
    cs->hcap = hcap;
    size_t mask = hcap - 1;
    size_t i;
    for (i = 0; i < cs->n; i++) {
        size_t slot = (size_t)(str_hash(cs->items[i].p.name) & mask);
        while (cs->hash[slot])
            slot = (slot + 1) & mask;
        cs->hash[slot] = (int)(i + 1);
    }
    return 0;
}

static int cs_probe(const candset_t *cs, const char *name)
{
    if (!cs->hash)
        return 0;
    size_t mask = cs->hcap - 1;
    size_t slot = (size_t)(str_hash(name) & mask);
    while (cs->hash[slot]) {
        size_t idx = (size_t)(cs->hash[slot] - 1);
        if (strcmp(cs->items[idx].p.name, name) == 0)
            return cs->hash[slot];
        slot = (slot + 1) & mask;
    }
    return 0;
}

cand_t *candset_find(candset_t *cs, const char *name)
{
    int r = cs_probe(cs, name);
    if (!r)
        return NULL;
    return &cs->items[(size_t)(r - 1)];
}

static void cand_init(cand_t *c)
{
    memset(c, 0, sizeof *c);
}

static void cand_copy(cand_t *dst, const cand_t *src)
{
    pkg_copy(&dst->p, &src->p);
    dst->size = src->size;
    dst->installed_size = src->installed_size;
    dst->suite = dup_or_null(src->suite);
    dst->comp = dup_or_null(src->comp);
}

int candset_merge(candset_t *cs, const cand_t *c)
{
    if (!cs->hash) {
        cs->hash = calloc(256, sizeof(int));
        if (!cs->hash)
            return -1;
        cs->hcap = 256;
    }
    int r = cs_probe(cs, c->p.name);
    if (r) {
        cand_t *old = &cs->items[(size_t)(r - 1)];
        if (deb_vercmp(c->p.version, old->p.version) > 0) {
            pkg_free(&old->p);
            free(old->suite);
            free(old->comp);
            cand_copy(old, c);
        }
        return 0;
    }
    if (cs->n + 1 > cs->hcap * 7 / 10) {
        if (cs_rehash(cs, cs->hcap * 2) < 0)
            return -1;
    }
    size_t mask = cs->hcap - 1;
    size_t slot = (size_t)(str_hash(c->p.name) & mask);
    while (cs->hash[slot])
        slot = (slot + 1) & mask;
    if (cs->n == cs->cap) {
        size_t ncap = cs->cap ? cs->cap * 2 : 1024;
        cand_t *ni = realloc(cs->items, ncap * sizeof(cand_t));
        if (!ni)
            return -1;
        cs->items = ni;
        cs->cap = ncap;
    }
    cand_init(&cs->items[cs->n]);
    cand_copy(&cs->items[cs->n], c);
    cs->hash[slot] = (int)(cs->n + 1);
    cs->n++;
    return 0;
}

int candset_load(const char *list_dir, candset_t *cs, const char *native_arch)
{
    DIR *d = opendir(list_dir);
    if (!d) {
        fprintf(stderr, "reposync: cannot open %s: %s\n", list_dir, strerror(errno));
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        if (nl < 10 || strcmp(de->d_name + nl - 9, "_Packages") != 0)
            continue;
        char *path = path_join(list_dir, de->d_name);
        sb_t content;
        sb_init(&content);
        if (read_file(path, &content) < 0) {
            free(path);
            sb_free(&content);
            continue;
        }
        /* .meta next to it carries URI / Suite / Component: needed so every
           candidate keeps its source-suite attribution for the sync suite
           mapping (a package in several suites maps by its highest-version
           candidate's suite). */
        char *mp = xmalloc(strlen(path) + 1 + 5 + 1);
        sprintf(mp, "%s.meta", path);
        char *msuite = NULL, *mcomp = NULL;
        idx_read_meta(mp, NULL, &msuite, &mcomp);
        free(mp);
        d822_doc doc;
        d822_parse(content.s, content.len, &doc);
        size_t i;
        for (i = 0; i < doc.n; i++) {
            const d822_rec *r = &doc.recs[i];
            const char *name = d822_get(r, "package");
            const char *ver = d822_get(r, "version");
            const char *arch = d822_get(r, "architecture");
            if (!name || !ver)
                continue;
            if (!arch || !*arch)
                arch = native_arch;
            if (strcmp(arch, native_arch) != 0 && strcmp(arch, "all") != 0)
                continue;
            cand_t c;
            cand_init(&c);
            c.p.name = xstrdup(name);
            c.p.version = xstrdup(ver);
            c.p.arch = xstrdup(arch);
            c.p.essential = d822_get(r, "essential") &&
                            strcasecmp(d822_get(r, "essential"), "yes") == 0;
            c.p.multi_arch = dup_or_null(d822_get(r, "multi-arch"));
            c.p.section = dup_or_null(d822_get(r, "section"));
            c.p.priority = dup_or_null(d822_get(r, "priority"));
            c.p.source = dup_or_null(d822_get(r, "source"));
            c.p.depends = dup_or_null(d822_get(r, "depends"));
            c.p.pre_depends = dup_or_null(d822_get(r, "pre-depends"));
            c.p.recommends = dup_or_null(d822_get(r, "recommends"));
            c.p.suggests = dup_or_null(d822_get(r, "suggests"));
            c.p.conflicts = dup_or_null(d822_get(r, "conflicts"));
            c.p.breaks = dup_or_null(d822_get(r, "breaks"));
            c.p.replaces = dup_or_null(d822_get(r, "replaces"));
            c.p.provides = dup_or_null(d822_get(r, "provides"));
            c.p.description = dup_or_null(d822_get(r, "description"));
            const char *sz = d822_get(r, "size");
            if (sz)
                c.size = strtoll(sz, NULL, 10);
            const char *is = d822_get(r, "installed-size");
            if (is)
                c.installed_size = strtoll(is, NULL, 10);
            c.suite = msuite ? xstrdup(msuite) : NULL;
            c.comp = mcomp ? xstrdup(mcomp) : NULL;
            candset_merge(cs, &c);
            pkg_free(&c.p);
            free(c.suite);
            free(c.comp);
        }
        d822_doc_free(&doc);
        free(msuite);
        free(mcomp);
        free(path);
        sb_free(&content);
    }
    closedir(d);
    return 0;
}

/* ---------------- sources (srcset) ---------------- */

void srcset_init(srcset_t *ss)
{
    memset(ss, 0, sizeof *ss);
}

static void srcpkg_free(srcpkg_t *s)
{
    free(s->name); free(s->version); free(s->binary); free(s->build_depends);
    free(s->directory); free(s->files); free(s->uri);
    free(s->suite); free(s->comp);
    memset(s, 0, sizeof *s);
}

void srcset_free(srcset_t *ss)
{
    size_t i;
    for (i = 0; i < ss->n; i++)
        srcpkg_free(&ss->items[i]);
    free(ss->items);
    free(ss->hash);
    for (i = 0; i < ss->bn; i++)
        free(ss->balias[i]);
    free(ss->balias);
    free(ss->bsrc);
    free(ss->bhash);
    memset(ss, 0, sizeof *ss);
}

static unsigned long long str_hash(const char *s);

static int ss_rehash(srcset_t *ss, size_t hcap)
{
    int *h = calloc(hcap, sizeof(int));
    if (!h)
        return -1;
    free(ss->hash);
    ss->hash = h;
    ss->hcap = hcap;
    size_t mask = hcap - 1;
    size_t i;
    for (i = 0; i < ss->n; i++) {
        size_t slot = (size_t)(str_hash(ss->items[i].name) & mask);
        while (ss->hash[slot])
            slot = (slot + 1) & mask;
        ss->hash[slot] = (int)(i + 1);
    }
    return 0;
}

static int ss_probe(const srcset_t *ss, const char *name)
{
    if (!ss->hash)
        return 0;
    size_t mask = ss->hcap - 1;
    size_t slot = (size_t)(str_hash(name) & mask);
    while (ss->hash[slot]) {
        size_t idx = (size_t)(ss->hash[slot] - 1);
        if (strcmp(ss->items[idx].name, name) == 0)
            return ss->hash[slot];
        slot = (slot + 1) & mask;
    }
    return 0;
}

srcpkg_t *srcset_find(srcset_t *ss, const char *name)
{
    int r = ss_probe(ss, name);
    if (!r)
        return NULL;
    return &ss->items[(size_t)(r - 1)];
}

srcpkg_t *srcset_put(srcset_t *ss, const char *name, const char *version)
{
    srcpkg_t *have = srcset_find(ss, name);
    if (have) {
        if (deb_vercmp(have->version, version) >= 0)
            return have;
        srcpkg_free(have);
    } else {
        if (ss->n == ss->cap) {
            size_t ncap = ss->cap ? ss->cap * 2 : 1024;
            srcpkg_t *ni = realloc(ss->items, ncap * sizeof(srcpkg_t));
            if (!ni)
                return NULL;
            ss->items = ni;
            ss->cap = ncap;
        }
        if (!ss->hash) {
            ss->hash = calloc(256, sizeof(int));
            if (!ss->hash)
                return NULL;
            ss->hcap = 256;
        }
        if (ss->n + 1 > ss->hcap * 7 / 10) {
            if (ss_rehash(ss, ss->hcap * 2) < 0)
                return NULL;
        }
        have = &ss->items[ss->n];
        memset(have, 0, sizeof *have);
        ss->n++;
        size_t mask = ss->hcap - 1;
        size_t slot = (size_t)(str_hash(name) & mask);
        while (ss->hash[slot])
            slot = (slot + 1) & mask;
        ss->hash[slot] = (int)ss->n;
    }
    have->name = xstrdup(name);
    have->version = xstrdup(version);
    return have;
}

static int ss_bprobe(const srcset_t *ss, const char *bin)
{
    if (!ss->bhash)
        return 0;
    size_t mask = ss->bhcap - 1;
    size_t slot = (size_t)(str_hash(bin) & mask);
    while (ss->bhash[slot]) {
        size_t aidx = (size_t)(ss->bhash[slot] - 1);
        if (strcmp(ss->balias[aidx], bin) == 0)
            return ss->bhash[slot];
        slot = (slot + 1) & mask;
    }
    return 0;
}

srcpkg_t *srcset_find_bin(srcset_t *ss, const char *bin)
{
    int r = ss_bprobe(ss, bin);
    if (!r)
        return NULL;
    return &ss->items[ss->bsrc[(size_t)(r - 1)]];
}

/* grow the alias hash and reinsert every alias string */
static int ss_bgrow(srcset_t *ss, size_t bhcap)
{
    int *h = calloc(bhcap, sizeof(int));
    if (!h)
        return -1;
    free(ss->bhash);
    ss->bhash = h;
    ss->bhcap = bhcap;
    size_t mask = bhcap - 1;
    size_t i;
    for (i = 0; i < ss->bn; i++) {
        size_t slot = (size_t)(str_hash(ss->balias[i]) & mask);
        while (ss->bhash[slot])
            slot = (slot + 1) & mask;
        ss->bhash[slot] = (int)(i + 1);
    }
    return 0;
}

static int ss_bput_one(srcset_t *ss, const char *bin, size_t src_idx)
{
    if (ss_bprobe(ss, bin))
        return 0;
    if (ss->bn == ss->bcap) {
        size_t ncap = ss->bcap ? ss->bcap * 2 : 1024;
        char **na = realloc(ss->balias, ncap * sizeof(char *));
        if (!na)
            return -1;
        ss->balias = na;
        size_t *ni = realloc(ss->bsrc, ncap * sizeof(size_t));
        if (!ni)
            return -1;
        ss->bsrc = ni;
        ss->bcap = ncap;
    }
    if (!ss->bhash) {
        ss->bhash = calloc(512, sizeof(int));
        if (!ss->bhash)
            return -1;
        ss->bhcap = 512;
    }
    if (ss->bn + 1 > ss->bhcap * 7 / 10) {
        if (ss_bgrow(ss, ss->bhcap * 2) < 0)
            return -1;
    }
    ss->balias[ss->bn] = xstrdup(bin);
    ss->bsrc[ss->bn] = src_idx;
    ss->bn++;
    size_t mask = ss->bhcap - 1;
    size_t slot = (size_t)(str_hash(bin) & mask);
    while (ss->bhash[slot])
        slot = (slot + 1) & mask;
    ss->bhash[slot] = (int)ss->bn;
    return 0;
}

int srcset_load(const char *list_dir, srcset_t *ss)
{
    DIR *d = opendir(list_dir);
    if (!d) {
        fprintf(stderr, "reposync: cannot open %s: %s\n", list_dir, strerror(errno));
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        if (nl < 9 || strcmp(de->d_name + nl - 8, "_Sources") != 0)
            continue;
        char *path = path_join(list_dir, de->d_name);
        sb_t content;
        sb_init(&content);
        if (read_file(path, &content) < 0) {
            free(path);
            sb_free(&content);
            continue;
        }
        /* .meta next to it carries the base URI + Suite / Component (needed
           for the source-side sync suite mapping) */
        char *mp = xmalloc(strlen(path) + 1 + 5 + 1);
        sprintf(mp, "%s.meta", path);
        char *muri = NULL, *msuite = NULL, *mcomp = NULL;
        idx_read_meta(mp, &muri, &msuite, &mcomp);
        free(mp);

        d822_doc doc;
        if (d822_parse(content.s, content.len, &doc) < 0) {
            free(path);
            free(muri);
            sb_free(&content);
            continue;
        }
        size_t i;
        for (i = 0; i < doc.n; i++) {
            const d822_rec *r = &doc.recs[i];
            const char *name = d822_get(r, "package");
            const char *ver = d822_get(r, "version");
            if (!name || !ver)
                continue;
            srcpkg_t *have = srcset_find(ss, name);
            if (have && deb_vercmp(have->version, ver) >= 0)
                continue; /* keep highest version only */
            if (!have) {
                if (ss->n == ss->cap) {
                    size_t ncap = ss->cap ? ss->cap * 2 : 1024;
                    srcpkg_t *ni = realloc(ss->items, ncap * sizeof(srcpkg_t));
                    if (!ni)
                        continue;
                    ss->items = ni;
                    ss->cap = ncap;
                }
                if (!ss->hash) {
                    ss->hash = calloc(256, sizeof(int));
                    if (!ss->hash)
                        continue;
                    ss->hcap = 256;
                }
                if (ss->n + 1 > ss->hcap * 7 / 10) {
                    if (ss_rehash(ss, ss->hcap * 2) < 0)
                        continue;
                }
                have = &ss->items[ss->n];
                memset(have, 0, sizeof *have);
                ss->n++;
                size_t mask = ss->hcap - 1;
                size_t slot = (size_t)(str_hash(name) & mask);
                while (ss->hash[slot])
                    slot = (slot + 1) & mask;
                ss->hash[slot] = (int)ss->n;
            }
            srcpkg_free(have); /* drop older version, replace with newer */
            have->name = xstrdup(name);
            have->version = xstrdup(ver);
            have->binary = dup_or_null(d822_get(r, "binary"));
            have->build_depends = dup_or_null(d822_get(r, "build-depends"));
            have->directory = dup_or_null(d822_get(r, "directory"));
            have->files = dup_or_null(d822_get(r, "files"));
            have->uri = muri ? xstrdup(muri) : NULL;
            have->suite = msuite ? xstrdup(msuite) : NULL;
            have->comp = mcomp ? xstrdup(mcomp) : NULL;
        }
        d822_doc_free(&doc);
        free(path);
        free(muri);
        free(msuite);
        free(mcomp);
        sb_free(&content);
    }
    closedir(d);

    /* reverse index: binary package name -> source package */
    size_t i;
    for (i = 0; i < ss->n; i++) {
        if (!ss->items[i].binary)
            continue;
        char *b = xstrdup(ss->items[i].binary);
        char *save = NULL;
        char *tok;
        for (tok = strtok_r(b, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            while (*tok == ' ' || *tok == '\t')
                tok++;
            size_t tl = strlen(tok);
            while (tl && (tok[tl - 1] == ' ' || tok[tl - 1] == '\t' || tok[tl - 1] == '\n'))
                tok[--tl] = '\0';
            if (*tok)
                ss_bput_one(ss, tok, i);
        }
        free(b);
    }
    return 0;
}

/* ---------------- source closure ---------------- */

/* Plain names from a Depends-style value (binary side): split on ',', '|',
   strip version constraints and :arch. Every alternative is kept. */
static void dep_names_c(const char *dep, str_list *out)
{
    if (!dep)
        return;
    char *copy = xstrdup(dep);
    char *save = NULL;
    char *tok = strtok_r(copy, ",", &save);
    while (tok) {
        char *save2 = NULL;
        char *part = strtok_r(tok, "|", &save2);
        while (part) {
            char *s = part;
            while (*s == ' ' || *s == '\t')
                s++;
            char *e = s + strlen(s);
            while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
                *--e = '\0';
            char *p;
            for (p = s; *p; p++) {
                if (*p == '(' || *p == ':') {
                    *p = '\0';
                    break;
                }
            }
            e = s + strlen(s);
            while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
                *--e = '\0';
            if (*s)
                sl_push(out, s);
            part = strtok_r(NULL, "|", &save2);
        }
        tok = strtok_r(NULL, ",", &save);
    }
    free(copy);
}

/* Architecture qualifier of a Build-Depends entry: "[amd64 linux-any !i386]".
   Returns 1 when the qualifier admits native. spec is the bracket contents. */
static int archq_ok(const char *spec, const char *native)
{
    int all_ok = 0;
    char *copy = xstrdup(spec);
    char *save = NULL;
    char *tok;
    for (tok = strtok_r(copy, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        int neg = 0;
        char *t = tok;
        if (*t == '!') {
            neg = 1;
            t++;
        }
        int match = 0;
        if (strcmp(t, native) == 0) {
            match = 1;
        } else {
            size_t tl = strlen(t);
            if (tl > 4 && strcmp(t + tl - 4, "-any") == 0) {
                size_t oslen = tl - 4;
                if (strlen(native) > oslen && strncmp(native, t, oslen) == 0 &&
                    native[oslen] == '-')
                    match = 1;
            }
        }
        if (neg ? !match : match)
            all_ok = 1;
    }
    free(copy);
    return all_ok;
}

/* Extract plain package names from a Build-Depends-style value: split on
   ',', then '|', strip [arch] qualifiers, version constraints and
   :arch suffixes. Every alternative is kept (over-inclusion is fine for a
   sync closure; the package list is administrator-maintained). */
static void builddep_names(const char *dep, str_list *out, const char *native)
{
    if (!dep)
        return;
    char *copy = xstrdup(dep);
    char *save = NULL;
    char *tok = strtok_r(copy, ",", &save);
    while (tok) {
        char *save2 = NULL;
        char *part = strtok_r(tok, "|", &save2);
        while (part) {
            /* strip [arch] qualifier */
            char *br = strchr(part, '[');
            char archq[512] = "";
            if (br) {
                char *bend = strchr(br, ']');
                if (bend) {
                    size_t qlen = (size_t)(bend - br - 1);
                    if (qlen > sizeof archq - 1)
                        qlen = sizeof archq - 1;
                    memcpy(archq, br + 1, qlen);
                    archq[qlen] = '\0';
                    *br = '\0';
                    if (!archq_ok(archq, native)) {
                        part = strtok_r(NULL, "|", &save2);
                        continue;
                    }
                }
            }
            char *s = part;
            while (*s == ' ' || *s == '\t')
                s++;
            char *e = s + strlen(s);
            while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
                *--e = '\0';
            char *p;
            for (p = s; *p; p++) {
                if (*p == '(' || *p == ':') {
                    *p = '\0';
                    break;
                }
            }
            e = s + strlen(s);
            while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
                *--e = '\0';
            if (*s)
                sl_push(out, s);
            part = strtok_r(NULL, "|", &save2);
        }
        tok = strtok_r(NULL, ",", &save);
    }
    free(copy);
}

/* ---------------- lightweight hash set of names ---------------- */

typedef struct {
    char **items;
    size_t n, cap;
    int *hash;
    size_t hcap;
} strset_t;

static void strset_init(strset_t *set)
{
    memset(set, 0, sizeof *set);
}

static void strset_free(strset_t *set)
{
    size_t i;
    for (i = 0; i < set->n; i++)
        free(set->items[i]);
    free(set->items);
    free(set->hash);
    memset(set, 0, sizeof *set);
}

static int strset_contains(const strset_t *set, const char *s)
{
    if (!set->hash)
        return 0;
    size_t mask = set->hcap - 1;
    size_t slot = (size_t)(str_hash(s) & mask);
    while (set->hash[slot]) {
        if (strcmp(set->items[set->hash[slot] - 1], s) == 0)
            return 1;
        slot = (slot + 1) & mask;
    }
    return 0;
}

static int strset_add(strset_t *set, const char *s)
{
    if (strset_contains(set, s))
        return 0;
    if (!set->hash) {
        set->hash = calloc(512, sizeof(int));
        if (!set->hash)
            return -1;
        set->hcap = 512;
    }
    if (set->n + 1 > set->hcap * 7 / 10) {
        size_t ncap = set->hcap * 2;
        int *h = calloc(ncap, sizeof(int));
        if (!h)
            return -1;
        size_t mask = ncap - 1;
        size_t i;
        for (i = 0; i < set->n; i++) {
            size_t slot = (size_t)(str_hash(set->items[i]) & mask);
            while (h[slot])
                slot = (slot + 1) & mask;
            h[slot] = (int)(i + 1);
        }
        free(set->hash);
        set->hash = h;
        set->hcap = ncap;
    }
    if (set->n == set->cap) {
        size_t ncap = set->cap ? set->cap * 2 : 512;
        char **ni = realloc(set->items, ncap * sizeof(char *));
        if (!ni)
            return -1;
        set->items = ni;
        set->cap = ncap;
    }
    size_t mask = set->hcap - 1;
    size_t slot = (size_t)(str_hash(s) & mask);
    while (set->hash[slot])
        slot = (slot + 1) & mask;
    set->items[set->n] = xstrdup(s);
    set->hash[slot] = (int)(set->n + 1);
    set->n++;
    return 0;
}

int sim_source_closure(candset_t *cs, srcset_t *ss, const char *native,
                       const str_list *targets, str_list *out_bin, str_list *out_src)
{
    sl_init(out_bin);
    sl_init(out_src);
    strset_t seen, src_set;
    strset_init(&seen);
    strset_init(&src_set);
    str_list queue;
    sl_init(&queue);
    size_t i;
    for (i = 0; i < targets->n; i++)
        sl_push(&queue, targets->items[i]);

    size_t qi = 0;
    while (qi < queue.n) {
        if ((qi & 0x3FFFF) == 0 && getenv("REPOSYNC_DEBUG"))
            fprintf(stderr, "  [sim-sources] queue=%zu seen=%zu srcset=%zu qi=%zu\n",
                    queue.n, seen.n, src_set.n, qi);
        const char *p = queue.items[qi++];
        if (strset_contains(&seen, p))
            continue;
        strset_add(&seen, p);

        cand_t *c = candset_find(cs, p);
        if (c) {
            /* binary side: Depends + Pre-Depends */
            str_list deps;
            sl_init(&deps);
            dep_names_c(c->p.depends, &deps);
            dep_names_c(c->p.pre_depends, &deps);
            size_t k;
            for (k = 0; k < deps.n; k++)
                if (candset_find(cs, deps.items[k]))
                    sl_push(&queue, deps.items[k]);
            sl_free(&deps);
            /* its source package */
            srcpkg_t *s = srcset_find_bin(ss, p);
            if (s && !strset_contains(&src_set, s->name)) {
                strset_add(&src_set, s->name);
                sl_push(&queue, s->name);
            }
        } else {
            srcpkg_t *s = srcset_find(ss, p);
            if (!s) {
                fprintf(stderr, "reposync: warning: cannot locate '%s' as binary or source package\n", p);
                continue;
            }
            if (!strset_contains(&src_set, s->name))
                strset_add(&src_set, s->name);
            /* its binary packages */
            if (s->binary) {
                str_list bins;
                sl_init(&bins);
                sl_push_split(&bins, s->binary);
                size_t k;
                for (k = 0; k < bins.n; k++) {
                    char tmp[512];
                    snprintf(tmp, sizeof tmp, "%s", bins.items[k]);
                    char *e = tmp + strlen(tmp);
                    while (e > tmp && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n'))
                        *--e = '\0';
                    if (*tmp && candset_find(cs, tmp))
                        sl_push(&queue, tmp);
                }
                sl_free(&bins);
            }
            /* its build-depends */
            if (s->build_depends) {
                str_list bd;
                sl_init(&bd);
                builddep_names(s->build_depends, &bd, native);
                size_t k;
                for (k = 0; k < bd.n; k++)
                    sl_push(&queue, bd.items[k]);
                sl_free(&bd);
            }
        }
    }
    /* out_bin: every reached binary package; out_src: every reached source */
    for (i = 0; i < seen.n; i++)
        if (candset_find(cs, seen.items[i]))
            sl_push(out_bin, seen.items[i]);
    for (i = 0; i < src_set.n; i++)
        sl_push(out_src, src_set.items[i]);
    sl_free(&queue);
    strset_free(&seen);
    strset_free(&src_set);
    return 0;
}

typedef struct {
    char *name;
    char op[3];      /* "", ">=", "<=", ">>", "<<", "=", ">", "<" */
    char *ver;
    char *archf;     /* NULL or "any"/"native"/arch */
} alt_t;

typedef struct {
    alt_t *alts;
    size_t n;
} dgroup_t;

typedef struct {
    dgroup_t *g;
    size_t n;
} deplist_t;

static char *trim_str(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
    return s;
}

static void alt_parse(char *s, alt_t *a)
{
    memset(a, 0, sizeof *a);
    s = trim_str(s);
    char *p = s;

    /* name */
    char *pend = p;
    while (*pend && *pend != ':' && *pend != '(' && *pend != ' ')
        pend++;
    a->name = xmalloc((size_t)(pend - p) + 1);
    memcpy(a->name, p, (size_t)(pend - p));
    a->name[pend - p] = '\0';
    p = pend;

    /* :arch */
    if (*p == ':') {
        p++;
        char *astart = p;
        while (*p && *p != '(' && *p != ' ')
            p++;
        a->archf = xmalloc((size_t)(p - astart) + 1);
        memcpy(a->archf, astart, (size_t)(p - astart));
        a->archf[p - astart] = '\0';
    }

    /* skip spaces */
    while (*p == ' ')
        p++;

    /* (op ver) */
    if (*p == '(') {
        char *close = strchr(p, ')');
        if (!close)
            return;
        *close = '\0';
        char *inner = trim_str(p + 1);
        char *q = inner;
        while (*q && *q != ' ')
            q++;
        if (q - inner <= 2) {
            size_t oplen = (size_t)(q - inner);
            memcpy(a->op, inner, oplen);
            a->op[oplen] = '\0';
            char *v = trim_str(q);
            if (*v)
                a->ver = xstrdup(v);
        }
    }
}

static void alt_free(alt_t *a)
{
    free(a->name);
    free(a->ver);
    free(a->archf);
}

/* Split a depends/conflicts/etc. value into groups of alternatives. */
static int parse_deplist(const char *dep, deplist_t *dl)
{
    memset(dl, 0, sizeof *dl);
    if (!dep || !*dep)
        return 0;
    char *dup = xstrdup(dep);
    char *save1 = NULL;
    char *group;
    for (group = strtok_r(dup, ",", &save1); group;
         group = strtok_r(NULL, ",", &save1)) {
        dgroup_t *ng = xrealloc(dl->g, (dl->n + 1) * sizeof(dgroup_t));
        dl->g = ng;
        dgroup_t *g = &dl->g[dl->n];
        g->alts = NULL;
        g->n = 0;
        dl->n++;
        char *save2 = NULL;
        char *altstr;
        for (altstr = strtok_r(group, "|", &save2); altstr;
             altstr = strtok_r(NULL, "|", &save2)) {
            alt_t *na = xrealloc(g->alts, (g->n + 1) * sizeof(alt_t));
            g->alts = na;
            alt_parse(altstr, &g->alts[g->n]);
            g->n++;
        }
    }
    free(dup);
    return 0;
}

static void deplist_free(deplist_t *dl)
{
    size_t i, j;
    for (i = 0; i < dl->n; i++) {
        for (j = 0; j < dl->g[i].n; j++)
            alt_free(&dl->g[i].alts[j]);
        free(dl->g[i].alts);
    }
    free(dl->g);
    memset(dl, 0, sizeof *dl);
}

static int ver_satisfies(const char *have, const char *op, const char *need)
{
    int c = deb_vercmp(have, need);
    if (strcmp(op, ">=") == 0) return c >= 0;
    if (strcmp(op, "<=") == 0) return c <= 0;
    if (strcmp(op, ">>") == 0) return c > 0;
    if (strcmp(op, "<<") == 0) return c < 0;
    if (strcmp(op, "=") == 0) return c == 0;
    if (strcmp(op, ">") == 0) return c > 0;
    if (strcmp(op, "<") == 0) return c < 0;
    return 1;
}

static int arch_ok(const char *archf, const char *native)
{
    if (!archf || !*archf || strcmp(archf, "any") == 0)
        return 1;
    if (strcmp(archf, "native") == 0)
        return 1;
    return strcmp(archf, native) == 0;
}

/* Check whether pkg p's Provides covers (name, op, ver). ver may be NULL
   meaning the dependency has no version constraint. */
static int provides_satisfies(const pkg_t *p, const char *want, const char *op, const char *ver)
{
    if (!p->provides || !*p->provides)
        return 0;
    char *dup = xstrdup(p->provides);
    int found = 0;
    char *save = NULL;
    char *tok;
    for (tok = strtok_r(dup, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *t = trim_str(tok);
        char *paren = strchr(t, '(');
        size_t nlen = paren ? (size_t)(paren - t) : strlen(t);
        while (nlen && (t[nlen - 1] == ' ' || t[nlen - 1] == '\t'))
            nlen--;
        if (nlen != strlen(want) || strncmp(t, want, nlen) != 0)
            continue;
        if (!ver || !*ver) {
            /* dependency without version constraint: any provide satisfies */
            found = 1;
            break;
        }
        if (!paren)
            continue; /* versioned dependency, unversioned provide: no */
        char *inner = paren + 1;
        char *cp = strchr(inner, ')');
        if (!cp)
            continue;
        *cp = '\0';
        inner = trim_str(inner);
        char *q = inner;
        while (*q && *q != ' ')
            q++;
        char *pv = trim_str(q); /* version part after the provide operator */
        if (*pv && ver_satisfies(pv, op, ver)) {
            found = 1;
            break;
        }
    }
    free(dup);
    return found;
}

static int is_installed(const pkg_t *p)
{
    return p && p->status && strncmp(p->status, "install", 7) == 0;
}

static int list_contains(const str_list *l, const char *s)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        if (strcmp(l->items[i], s) == 0)
            return 1;
    return 0;
}

/* Try to satisfy one dependency alternative against the target state.
   Returns 1 if satisfiable; when an install/upgrade is required, queues it.
   Virtual deps resolve through providers of their candidate versions. */
static int alt_resolve(pkgset_t *ps, candset_t *cs, const alt_t *a,
                       const char *native, str_list *queue)
{
    if (!arch_ok(a->archf, native))
        return 0;
    pkg_t *h = pkgset_find(ps, a->name);
    if (is_installed(h) && (!a->op[0] || !a->ver ||
                            ver_satisfies(h->version, a->op, a->ver)))
        return 1;
    size_t k;
    for (k = 0; k < ps->n; k++) {
        if (is_installed(&ps->items[k]) &&
            provides_satisfies(&ps->items[k], a->name, a->op, a->ver))
            return 1;
    }
    if (candset_find(cs, a->name)) {
        if (!list_contains(queue, a->name))
            sl_push(queue, a->name);
        return 1;
    }
    /* virtual package: prefer an already-installed provider (upgrade it),
       else fall back to any provider of a candidate version */
    for (k = 0; k < ps->n; k++) {
        pkg_t *prov = &ps->items[k];
        if (!is_installed(prov))
            continue;
        cand_t *pc = candset_find(cs, prov->name);
        if (pc && provides_satisfies(&pc->p, a->name, a->op, a->ver)) {
            if (!list_contains(queue, prov->name))
                sl_push(queue, prov->name);
            return 1;
        }
    }
    for (k = 0; k < cs->n; k++) {
        if (provides_satisfies(&cs->items[k].p, a->name, a->op, a->ver)) {
            if (!list_contains(queue, cs->items[k].p.name))
                sl_push(queue, cs->items[k].p.name);
            return 1;
        }
    }
    return 0;
}

/* ---------------- closure solving ---------------- */

/* For an already-present package, queue any hard dependency that is not yet
   satisfied in the target state (repairs inconsistent repos). */
static void ensure_installed_deps(pkgset_t *ps, candset_t *cs, const char *name,
                                  const char *native, str_list *queue)
{
    pkg_t *h = pkgset_find(ps, name);
    if (!h)
        return;
    deplist_t dl1, dl2;
    parse_deplist(h->depends, &dl1);
    parse_deplist(h->pre_depends, &dl2);
    size_t gi, i;
    for (gi = 0; gi < dl1.n; gi++) {
        int ok = 0;
        for (i = 0; i < dl1.g[gi].n; i++) {
            if (alt_resolve(ps, cs, &dl1.g[gi].alts[i], native, queue)) {
                ok = 1;
                break;
            }
        }
        if (!ok)
            fprintf(stderr, "reposync: warning: cannot satisfy dependency of %s: %s\n",
                    name, dl1.g[gi].alts[0].name);
    }
    for (gi = 0; gi < dl2.n; gi++) {
        int ok = 0;
        for (i = 0; i < dl2.g[gi].n; i++) {
            if (alt_resolve(ps, cs, &dl2.g[gi].alts[i], native, queue)) {
                ok = 1;
                break;
            }
        }
        if (!ok)
            fprintf(stderr, "reposync: warning: cannot satisfy Pre-Depends of %s: %s\n",
                    name, dl2.g[gi].alts[0].name);
    }
    deplist_free(&dl1);
    deplist_free(&dl2);
}

/* Returns 0 on success, -1 on unsatisfiable. Modifies ps (the target state).
   cl->changed = packages needing install/upgrade (names); cl->upgraded subset
   already present; cl->queue holds every name that entered the closure.
   When ensure_deps is set, already-present packages also get their hard
   dependencies checked and missing ones queued (repo repair semantics). */
int sim_closure_solve(pkgset_t *ps, candset_t *cs, const char *native,
                      const str_list *targets, closure_t *cl, int ensure_deps)
{
    sl_init(&cl->queue);
    sl_init(&cl->changed);
    sl_init(&cl->upgraded);

    size_t i;
    for (i = 0; i < targets->n; i++) {
        if (!list_contains(&cl->queue, targets->items[i]))
            sl_push(&cl->queue, targets->items[i]);
    }

    size_t qi = 0;
    while (qi < cl->queue.n) {
        char *name = xstrdup(cl->queue.items[qi++]);
        cand_t *c = candset_find(cs, name);
        if (!c) {
            free(name);
            continue;
        }
        int was_upgrade = 0;
        pkg_t *have = pkgset_find(ps, name);
        if (have && is_installed(have) && deb_vercmp(have->version, c->p.version) >= 0) {
            if (ensure_deps)
                ensure_installed_deps(ps, cs, name, native, &cl->queue);
            free(name);
            continue; /* already satisfied at >= candidate */
        }
        if (have && is_installed(have))
            was_upgrade = 1;

        /* apply new/upgraded package to target state */
        pkg_t np;
        pkg_init(&np);
        pkg_copy(&np, &c->p);
        if (!np.arch)
            np.arch = xstrdup(native);
        np.status = xstrdup("install ok installed");
        pkgset_put(ps, &np);
        pkg_free(&np);
        if (!list_contains(&cl->changed, name)) {
            sl_push(&cl->changed, name);
            if (was_upgrade)
                sl_push(&cl->upgraded, name);
        }

        /* resolve Depends + Pre-Depends */
        deplist_t dl1, dl2, dl3;
        parse_deplist(c->p.depends, &dl1);
        parse_deplist(c->p.pre_depends, &dl2);
        parse_deplist(c->p.recommends, &dl3);
        int fail = 0;
        size_t gi;
        for (gi = 0; gi < dl1.n && !fail; gi++) {
            int ok = 0;
            for (i = 0; i < dl1.g[gi].n; i++) {
                if (alt_resolve(ps, cs, &dl1.g[gi].alts[i], native, &cl->queue)) {
                    ok = 1;
                    break;
                }
            }
            if (!ok) {
                fprintf(stderr, "reposync: sim: cannot satisfy dependency of %s: %s\n",
                        name, dl1.g[gi].alts[0].name);
                fail = 1;
            }
        }
        for (gi = 0; gi < dl2.n && !fail; gi++) {
            int ok = 0;
            for (i = 0; i < dl2.g[gi].n; i++) {
                if (alt_resolve(ps, cs, &dl2.g[gi].alts[i], native, &cl->queue)) {
                    ok = 1;
                    break;
                }
            }
            if (!ok) {
                fprintf(stderr, "reposync: sim: cannot satisfy Pre-Depends of %s: %s\n",
                        name, dl2.g[gi].alts[0].name);
                fail = 1;
            }
        }
        /* Recommends: soft deps. Install available candidates, ignore the
           rest (apt default behavior with Install-Recommends=true). */
        for (gi = 0; gi < dl3.n; gi++) {
            int ok = 0;
            for (i = 0; i < dl3.g[gi].n; i++) {
                if (alt_resolve(ps, cs, &dl3.g[gi].alts[i], native, &cl->queue)) {
                    ok = 1;
                    break;
                }
            }
            (void)ok;
        }
        deplist_free(&dl1);
        deplist_free(&dl2);
        deplist_free(&dl3);
        free(name);
        if (fail)
            return -1;
    }
    return 0;
}

/* Check Conflicts/Breaks of changed packages against the target state. */
static int check_conflicts(pkgset_t *ps, const str_list *changed, const char *native)
{
    size_t i, k;
    for (i = 0; i < changed->n; i++) {
        pkg_t *p = pkgset_find(ps, changed->items[i]);
        if (!p)
            continue;
        const char *fields[2] = { p->conflicts, p->breaks };
        int fi;
        for (fi = 0; fi < 2; fi++) {
            if (!fields[fi])
                continue;
            deplist_t dl;
            parse_deplist(fields[fi], &dl);
            size_t gi;
            for (gi = 0; gi < dl.n; gi++) {
                for (k = 0; k < dl.g[gi].n; k++) {
                    alt_t *a = &dl.g[gi].alts[k];
                    if (!arch_ok(a->archf, native))
                        continue;
                    pkg_t *q = pkgset_find(ps, a->name);
                    if (is_installed(q) &&
                        (!a->op[0] || !a->ver ||
                         ver_satisfies(q->version, a->op, a->ver))) {
                        fprintf(stderr,
                                "reposync: sim: conflict: %s conflicts with installed %s %s\n",
                                changed->items[i], a->name,
                                a->op[0] ? a->ver : "");
                        deplist_free(&dl);
                        return -1;
                    }
                }
            }
            deplist_free(&dl);
        }
    }
    return 0;
}

/* ---------------- apt-style output ---------------- */

static void print_plan(const char *verb, const str_list *newpkgs,
                       const str_list *upgraded, long long disk_extra)
{
    size_t i;
    if (newpkgs->n) {
        printf("The following NEW packages will be installed:\n  ");
        for (i = 0; i < newpkgs->n; i++) {
            if (i && i % 6 == 0)
                printf("\n  ");
            printf("%s ", newpkgs->items[i]);
        }
        printf("\n");
    }
    if (upgraded->n) {
        printf("The following packages will be upgraded:\n  ");
        for (i = 0; i < upgraded->n; i++) {
            if (i && i % 6 == 0)
                printf("\n  ");
            printf("%s ", upgraded->items[i]);
        }
        printf("\n");
    }
    printf("%zu newly installed, %zu upgraded.\n", newpkgs->n, upgraded->n);
    if (disk_extra > 0)
        printf("After this operation, %lld kB of additional disk space will be used.\n",
               disk_extra / 1024);
    (void)verb;
}

/* changed minus upgraded -> truly new packages */
static int newonly_init(str_list *out, const closure_t *cl)
{
    sl_init(out);
    size_t i;
    for (i = 0; i < cl->changed.n; i++) {
        if (!list_contains(&cl->upgraded, cl->changed.items[i])) {
            if (sl_push(out, cl->changed.items[i]) < 0)
                return -1;
        }
    }
    return 0;
}

static int confirm(int yes)
{
    if (yes)
        return 1;
    printf("Do you want to continue? [Y/n] ");
    fflush(stdout);
    int c = getchar();
    return (c == 'n' || c == 'N') ? 0 : 1;
}

/* ---------------- sim operations ---------------- */

int sim_install(const char *db_path, const char *list_dir, const char *pkg, int yes)
{
    candset_t cs;
    candset_init(&cs);
    if (candset_load(list_dir, &cs, native_deb_arch()) < 0)
        return -1;
    if (!candset_find(&cs, pkg)) {
        fprintf(stderr, "reposync: sim: unable to locate package %s\n", pkg);
        candset_free(&cs);
        return -1;
    }
    pkgset_t ps;
    pkgset_init(&ps);
    if (db_load_all(db_path, &ps) < 0) {
        candset_free(&cs);
        return -1;
    }
    str_list targets;
    sl_init(&targets);
    sl_push(&targets, pkg);

    closure_t cl;
    if (sim_closure_solve(&ps, &cs, native_deb_arch(), &targets, &cl, 0) < 0) {
        sl_free(&targets);
        pkgset_free(&ps);
        candset_free(&cs);
        return -1;
    }
    if (check_conflicts(&ps, &cl.changed, native_deb_arch()) < 0) {
        sl_free(&targets);
        pkgset_free(&ps);
        candset_free(&cs);
        return -1;
    }

    printf("Reading package lists... Done\n");
    printf("Building dependency tree... Done\n");
    str_list newonly;
    newonly_init(&newonly, &cl);
    print_plan("install", &newonly, &cl.upgraded, 0);
    sl_free(&newonly);
    if (!confirm(yes)) {
        printf("Abort.\n");
        sl_free(&targets);
        pkgset_free(&ps);
        candset_free(&cs);
        return 0;
    }
    if (db_write_all(db_path, &ps) < 0) {
        sl_free(&targets);
        pkgset_free(&ps);
        candset_free(&cs);
        return -1;
    }
    sl_free(&targets);
    pkgset_free(&ps);
    candset_free(&cs);
    return 0;
}

int sim_remove(const char *db_path, const char *pkg, int yes)
{
    pkgset_t ps;
    pkgset_init(&ps);
    if (db_load_all(db_path, &ps) < 0)
        return -1;
    pkg_t *p = pkgset_find(&ps, pkg);
    if (!p || !is_installed(p)) {
        fprintf(stderr, "reposync: sim: package %s is not installed\n", pkg);
        pkgset_free(&ps);
        return -1;
    }

    /* reverse dependencies: installed packages that depend on pkg (or its
       virtual names) and cannot be satisfied without it */
    const char *native = native_deb_arch();
    str_list blockers;
    sl_init(&blockers);
    size_t i, gi;
    for (i = 0; i < ps.n; i++) {
        pkg_t *q = &ps.items[i];
        if (!is_installed(q) || strcmp(q->name, pkg) == 0)
            continue;
        if (!q->depends && !q->pre_depends)
            continue;
        deplist_t dl;
        parse_deplist(q->depends, &dl);
        deplist_t dlp;
        parse_deplist(q->pre_depends, &dlp);
        if (dlp.n) {
            dl.g = xrealloc(dl.g, (dl.n + dlp.n) * sizeof(dgroup_t));
            memcpy(dl.g + dl.n, dlp.g, dlp.n * sizeof(dgroup_t));
            dl.n += dlp.n;
            free(dlp.g); /* ownership of groups moved into dl */
        }
        for (gi = 0; gi < dl.n; gi++) {
            /* does this group reference pkg (directly or via provides)? */
            int refs = 0;
            for (size_t k = 0; k < dl.g[gi].n; k++) {
                alt_t *a = &dl.g[gi].alts[k];
                if (strcmp(a->name, pkg) == 0 ||
                    provides_satisfies(p, a->name, a->op, a->ver))
                    refs = 1;
            }
            if (!refs)
                continue;
            /* can the group be satisfied without pkg? */
            int ok = 0;
            for (size_t k = 0; k < dl.g[gi].n; k++) {
                alt_t *a = &dl.g[gi].alts[k];
                if (strcmp(a->name, pkg) == 0 ||
                    provides_satisfies(p, a->name, a->op, a->ver))
                    continue; /* this alternative is removed with pkg */
                if (!arch_ok(a->archf, native))
                    continue;
                pkg_t *h = pkgset_find(&ps, a->name);
                if (is_installed(h) && (!a->op[0] || !a->ver ||
                                        ver_satisfies(h->version, a->op, a->ver))) {
                    ok = 1;
                    break;
                }
                int provided = 0;
                for (size_t m = 0; m < ps.n; m++) {
                    pkg_t *pp = &ps.items[m];
                    if (is_installed(pp) && strcmp(pp->name, pkg) != 0 &&
                        provides_satisfies(pp, a->name, a->op, a->ver)) {
                        provided = 1;
                        break;
                    }
                }
                if (provided) {
                    ok = 1;
                    break;
                }
            }
            if (!ok) {
                if (!list_contains(&blockers, q->name))
                    sl_push(&blockers, q->name);
            }
        }
        deplist_free(&dl);
    }

    if (blockers.n) {
        fprintf(stderr, "reposync: sim: cannot remove %s: still needed by:\n  ", pkg);
        for (i = 0; i < blockers.n; i++)
            fprintf(stderr, "%s ", blockers.items[i]);
        fprintf(stderr, "\n");
        sl_free(&blockers);
        pkgset_free(&ps);
        return -1;
    }
    sl_free(&blockers);

    printf("The following packages will be REMOVED:\n  %s\n", pkg);
    if (!confirm(yes)) {
        printf("Abort.\n");
        pkgset_free(&ps);
        return 0;
    }
    p->status = xstrdup("deinstall ok config-files");
    if (db_write_all(db_path, &ps) < 0) {
        pkgset_free(&ps);
        return -1;
    }
    pkgset_free(&ps);
    return 0;
}

int sim_upgrade(const char *db_path, const char *list_dir, int yes)
{
    candset_t cs;
    candset_init(&cs);
    if (candset_load(list_dir, &cs, native_deb_arch()) < 0)
        return -1;
    pkgset_t ps;
    pkgset_init(&ps);
    if (db_load_all(db_path, &ps) < 0) {
        candset_free(&cs);
        return -1;
    }

    str_list targets;
    sl_init(&targets);
    size_t i;
    for (i = 0; i < ps.n; i++) {
        pkg_t *p = &ps.items[i];
        if (!is_installed(p))
            continue;
        cand_t *c = candset_find(&cs, p->name);
        if (c && deb_vercmp(c->p.version, p->version) > 0)
            sl_push(&targets, p->name);
    }
    if (targets.n == 0) {
        printf("0 upgraded, 0 newly installed.\n");
        sl_free(&targets);
        pkgset_free(&ps);
        candset_free(&cs);
        return 0;
    }

    closure_t cl;
    if (sim_closure_solve(&ps, &cs, native_deb_arch(), &targets, &cl, 0) < 0) {
        sl_free(&targets);
        pkgset_free(&ps);
        candset_free(&cs);
        return -1;
    }
    if (check_conflicts(&ps, &cl.changed, native_deb_arch()) < 0) {
        sl_free(&targets);
        pkgset_free(&ps);
        candset_free(&cs);
        return -1;
    }

    printf("Reading package lists... Done\n");
    printf("Building dependency tree... Done\n");
    str_list newonly;
    newonly_init(&newonly, &cl);
    print_plan("upgrade", &newonly, &cl.upgraded, 0);
    sl_free(&newonly);
    if (!confirm(yes)) {
        printf("Abort.\n");
        sl_free(&targets);
        pkgset_free(&ps);
        candset_free(&cs);
        return 0;
    }
    if (db_write_all(db_path, &ps) < 0) {
        sl_free(&targets);
        pkgset_free(&ps);
        candset_free(&cs);
        return -1;
    }
    sl_free(&targets);
    pkgset_free(&ps);
    candset_free(&cs);
    return 0;
}
