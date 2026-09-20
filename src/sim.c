#include "sim.h"
#include "db.h"
#include "deb822.h"
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
    for (i = 0; i < cs->n; i++)
        pkg_free(&cs->items[i].p);
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
            candset_merge(cs, &c);
            pkg_free(&c.p);
        }
        d822_doc_free(&doc);
        free(path);
        sb_free(&content);
    }
    closedir(d);
    return 0;
}

/* ---------------- dependency parsing ---------------- */

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
