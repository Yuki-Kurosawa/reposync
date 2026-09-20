#include "ops.h"
#include "deb822.h"
#include "fetch.h"
#include "index.h"
#include "sim.h"
#include "util.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ---------- helpers ---------- */

static int confirm(const char *msg, int yes)
{
    if (yes)
        return 1;
    printf("%s [Y/n] ", msg);
    fflush(stdout);
    int c = getchar();
    return (c == 'n' || c == 'N') ? 0 : 1;
}

static int list_contains(const str_list *l, const char *s)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        if (strcmp(l->items[i], s) == 0)
            return 1;
    return 0;
}

/* Collect candidate package names referenced by a Depends-style string:
   split on ',', then '|', take each item's name (strip version/arch). */
static void dep_names(const char *dep, str_list *out)
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
            /* the '(' cut may leave trailing whitespace; re-trim so the
               name matches the package index exactly (topo sort depends on
               it) */
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

static int rec_find(const d822_doc *doc, const char *pkg, size_t *idx)
{
    size_t i;
    for (i = 0; i < doc->n; i++) {
        const char *name = d822_get(&doc->recs[i], "package");
        if (name && strcmp(name, pkg) == 0) {
            *idx = i;
            return 1;
        }
    }
    return 0;
}

static char *mk_tmpdir(void)
{
#ifdef _WIN32
    char base[MAX_PATH];
    DWORD n = GetTempPathA(sizeof base, base);
    if (n == 0 || n >= sizeof base)
        return NULL;
    char *d = xmalloc(n + 24);
    snprintf(d, n + 24, "%sreposync-inst-%lu", base,
             (unsigned long)_getpid());
    if (mkdir_p(d, 0700) != 0) {
        free(d);
        return NULL;
    }
    return d;
#else
    char *d = xstrdup("/tmp/reposync-inst-XXXXXX");
    if (!mkdtemp(d)) {
        free(d);
        return NULL;
    }
    return d;
#endif
}

/* Run "cmdline arg" (cmdline split on whitespace, arg appended). Returns 0. */
static int run_cmd(const char *cmdline, const char *arg)
{
    if (!cmdline || !*cmdline) {
        fprintf(stderr, "reposync: no import/remove command configured in reposync.conf\n");
        return -1;
    }
    char *copy = xstrdup(cmdline);
    char **args = xmalloc(64 * sizeof(char *));
    int n = 0;
    char *save = NULL;
    char *tok = strtok_r(copy, " \t", &save);
    while (tok && n < 62) {
        args[n++] = tok;
        tok = strtok_r(NULL, " \t", &save);
    }
    if (n == 0) {
        free(copy);
        free(args);
        fprintf(stderr, "reposync: empty command\n");
        return -1;
    }
    args[n] = (char *)arg;
    args[n + 1] = NULL;

    int i;
    printf("  exec:");
    for (i = 0; i <= n; i++)
        printf(" %s", args[i]);
    printf("\n");

    int rc = plat_run_simple((const char *const *)args);
    free(copy);
    free(args);
    if (rc == 0)
        return 0;
    fprintf(stderr, "reposync: command failed (exit %d)\n", rc);
    return -1;
}

/* Reorder changed so every package comes after the packages it depends on
   (deps first). Cycles are broken arbitrarily (they exist in Debian). */
static void topo_order(candset_t *cs, str_list *ch)
{
    size_t n = ch->n;
    if (n <= 1)
        return;
    int *pred = xmalloc(n * sizeof(int));
    int **succ = xmalloc(n * sizeof(int *));
    int *succn = xmalloc(n * sizeof(int));
    int *succcap = xmalloc(n * sizeof(int));
    size_t i;
    for (i = 0; i < n; i++) {
        pred[i] = 0;
        succ[i] = NULL;
        succn[i] = 0;
        succcap[i] = 0;
    }
    for (i = 0; i < n; i++) {
        cand_t *c = candset_find(cs, ch->items[i]);
        if (!c)
            continue;
        str_list deps;
        sl_init(&deps);
        dep_names(c->p.depends, &deps);
        dep_names(c->p.pre_depends, &deps);
        dep_names(c->p.recommends, &deps);
        size_t k;
        for (k = 0; k < deps.n; k++) {
            size_t j;
            for (j = 0; j < n; j++) {
                if (j != i && strcmp(ch->items[j], deps.items[k]) == 0) {
                    pred[i]++;
                    if (succn[j] == succcap[j]) {
                        succcap[j] = succcap[j] ? succcap[j] * 2 : 4;
                        succ[j] = xrealloc(succ[j], succcap[j] * sizeof(int));
                    }
                    succ[j][succn[j]++] = (int)i;
                    break;
                }
            }
        }
        sl_free(&deps);
    }
    int *order = xmalloc(n * sizeof(int));
    int on = 0;
    int *q = xmalloc(n * sizeof(int));
    int qh = 0, qt = 0;
    for (i = 0; i < n; i++)
        if (pred[i] == 0)
            q[qt++] = (int)i;
    while (qh < qt) {
        int v = q[qh++];
        order[on++] = v;
        int k;
        for (k = 0; k < succn[v]; k++) {
            int w = succ[v][k];
            if (--pred[w] == 0)
                q[qt++] = w;
        }
    }
    for (i = 0; i < n; i++)
        if (pred[i] > 0)
            order[on++] = (int)i;
    str_list out;
    sl_init(&out);
    for (i = 0; i < (size_t)on; i++)
        sl_push(&out, ch->items[order[i]]);
    sl_free(ch);
    *ch = out;
    free(pred);
    for (i = 0; i < n; i++)
        free(succ[i]);
    free(succ);
    free(succn);
    free(succcap);
    free(order);
    free(q);
}

/* Fetch and import every package in cl.changed (deps-first order) via
   target.import_cmd. Downloads land in a temp dir. Returns 0 on success. */
static int import_changed(target_t *t, const char *list_dir, str_list *changed)
{
    if (!t->import_cmd) {
        fprintf(stderr, "reposync: Import-Cmd not set in reposync.conf\n");
        return -1;
    }
    char *tmpdir = mk_tmpdir();
    if (!tmpdir) {
        fprintf(stderr, "reposync: mkdtemp failed\n");
        return -1;
    }
    int ok = 1;
    size_t i;
    for (i = 0; i < changed->n; i++) {
        const char *pkg = changed->items[i];
        char *saved = NULL;
        if (idx_download(list_dir, pkg, tmpdir, &saved) < 0) {
            ok = 0;
            continue;
        }
        if (run_cmd(t->import_cmd, saved) < 0) {
            ok = 0;
        } else {
            printf("  imported %s\n", pkg);
        }
        free(saved);
    }
    rmdir(tmpdir);
    free(tmpdir);
    return ok ? 0 : -1;
}

/* ---------- manual (explicitly installed) package list ---------- */

#define MANUAL_PATH "/var/lib/" TOOLNAME "/manual-packages"

static const char *manual_path(void)
{
    const char *p = getenv("REPOSYNC_MANUAL");
    return (p && *p) ? p : MANUAL_PATH;
}

static int manual_load(str_list *out)
{
    sl_init(out);
    FILE *f = fopen(manual_path(), "r");
    if (!f)
        return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r'))
            *--e = '\0';
        if (*line)
            sl_push(out, line);
    }
    fclose(f);
    return 0;
}

static void manual_add(const str_list *names)
{
    str_list cur;
    manual_load(&cur);
    size_t i;
    for (i = 0; i < names->n; i++)
        if (!list_contains(&cur, names->items[i]))
            sl_push(&cur, names->items[i]);
    FILE *f = fopen(manual_path(), "w");
    if (!f) {
        fprintf(stderr, "reposync: cannot write %s: %s\n",
                manual_path(), strerror(errno));
        sl_free(&cur);
        return;
    }
    for (i = 0; i < cur.n; i++)
        fprintf(f, "%s\n", cur.items[i]);
    fclose(f);
    sl_free(&cur);
}

/* ---------- install ---------- */

int ops_install(target_t *t, const char *list_dir, int argc, char **argv, int yes)
{
    pkgset_t ps;
    d822_doc tdoc;
    if (target_state(t, &ps, &tdoc) < 0) {
        pkgset_free(&ps);
        d822_doc_free(&tdoc);
        return -1;
    }

    candset_t cs;
    candset_init(&cs);
    if (candset_load(list_dir, &cs, t->arch) < 0) {
        pkgset_free(&ps);
        d822_doc_free(&tdoc);
        return -1;
    }

    str_list targets;
    sl_init(&targets);
    int a;
    for (a = 0; a < argc; a++)
        sl_push(&targets, argv[a]);

    closure_t cl;
    if (sim_closure_solve(&ps, &cs, t->arch, &targets, &cl, 1) < 0) {
        fprintf(stderr, "reposync: unsatisfiable dependency closure for requested packages\n");
        sl_free(&targets);
        sl_free(&cl.queue);
        sl_free(&cl.changed);
        sl_free(&cl.upgraded);
        pkgset_free(&ps);
        candset_free(&cs);
        d822_doc_free(&tdoc);
        return -1;
    }
    topo_order(&cs, &cl.changed);

    if (cl.changed.n == 0) {
        printf("  all requested package(s) already in target repo\n");
        sl_free(&targets);
        sl_free(&cl.queue);
        sl_free(&cl.changed);
        sl_free(&cl.upgraded);
        pkgset_free(&ps);
        candset_free(&cs);
        d822_doc_free(&tdoc);
        return 0;
    }

    str_list newonly;
    sl_init(&newonly);
    size_t i;
    for (i = 0; i < cl.changed.n; i++) {
        if (!list_contains(&cl.upgraded, cl.changed.items[i]))
            sl_push(&newonly, cl.changed.items[i]);
    }
    printf("The following NEW packages will be imported:\n  ");
    for (i = 0; i < newonly.n; i++) {
        if (i && i % 6 == 0)
            printf("\n  ");
        printf("%s ", newonly.items[i]);
    }
    printf("\n");
    if (cl.upgraded.n) {
        printf("The following packages will be upgraded in target:\n  ");
        for (i = 0; i < cl.upgraded.n; i++) {
            if (i && i % 6 == 0)
                printf("\n  ");
            printf("%s ", cl.upgraded.items[i]);
        }
        printf("\n");
    }
    printf("%zu new, %zu upgraded.\n", newonly.n, cl.upgraded.n);
    sl_free(&newonly);

    if (!confirm("Continue?", yes)) {
        printf("Abort.\n");
        sl_free(&targets);
        sl_free(&cl.queue);
        sl_free(&cl.changed);
        sl_free(&cl.upgraded);
        pkgset_free(&ps);
        candset_free(&cs);
        d822_doc_free(&tdoc);
        return 0;
    }

    int rc = import_changed(t, list_dir, &cl.changed);
    if (rc == 0)
        manual_add(&targets); /* explicit requests survive autoremove */
    sl_free(&targets);
    sl_free(&cl.queue);
    sl_free(&cl.changed);
    sl_free(&cl.upgraded);
    pkgset_free(&ps);
    candset_free(&cs);
    d822_doc_free(&tdoc);
    return rc;
}

/* ---------- upgrade ---------- */

int ops_upgrade(target_t *t, const char *list_dir, int argc, char **argv, int yes)
{
    (void)argc; (void)argv;
    pkgset_t ps;
    d822_doc tdoc;
    if (target_state(t, &ps, &tdoc) < 0) {
        pkgset_free(&ps);
        d822_doc_free(&tdoc);
        return -1;
    }

    candset_t cs;
    candset_init(&cs);
    if (candset_load(list_dir, &cs, t->arch) < 0) {
        pkgset_free(&ps);
        candset_free(&cs);
        d822_doc_free(&tdoc);
        return -1;
    }

    str_list targets;
    sl_init(&targets);
    size_t i;
    for (i = 0; i < ps.n; i++)
        sl_push(&targets, ps.items[i].name);

    closure_t cl;
    if (sim_closure_solve(&ps, &cs, t->arch, &targets, &cl, 1) < 0) {
        fprintf(stderr, "reposync: unsatisfiable dependency closure for upgrade\n");
        sl_free(&targets);
        sl_free(&cl.queue);
        sl_free(&cl.changed);
        sl_free(&cl.upgraded);
        pkgset_free(&ps);
        candset_free(&cs);
        d822_doc_free(&tdoc);
        return -1;
    }
    topo_order(&cs, &cl.changed);

    if (cl.changed.n == 0) {
        printf("  all %zu package(s) in target repo up to date\n", ps.n);
        sl_free(&targets);
        sl_free(&cl.queue);
        sl_free(&cl.changed);
        sl_free(&cl.upgraded);
        pkgset_free(&ps);
        candset_free(&cs);
        d822_doc_free(&tdoc);
        return 0;
    }
    printf("The following packages will be upgraded:\n  ");
    for (i = 0; i < cl.changed.n; i++) {
        if (i && i % 6 == 0)
            printf("\n  ");
        printf("%s ", cl.changed.items[i]);
    }
    printf("\n%zu to change.\n", cl.changed.n);

    if (!confirm("Continue?", yes)) {
        printf("Abort.\n");
        sl_free(&targets);
        sl_free(&cl.queue);
        sl_free(&cl.changed);
        sl_free(&cl.upgraded);
        pkgset_free(&ps);
        candset_free(&cs);
        d822_doc_free(&tdoc);
        return 0;
    }

    int rc = import_changed(t, list_dir, &cl.changed);
    sl_free(&targets);
    sl_free(&cl.queue);
    sl_free(&cl.changed);
    sl_free(&cl.upgraded);
    pkgset_free(&ps);
    candset_free(&cs);
    d822_doc_free(&tdoc);
    return rc;
}

/* ---------- remove ---------- */

int ops_remove(target_t *t, const char *list_dir, int argc, char **argv, int yes)
{
    (void)list_dir;
    if (!t->remove_cmd) {
        fprintf(stderr, "reposync: Remove-Cmd not set in reposync.conf\n");
        return -1;
    }
    pkgset_t ps;
    d822_doc tdoc;
    if (target_state(t, &ps, &tdoc) < 0) {
        pkgset_free(&ps);
        d822_doc_free(&tdoc);
        return -1;
    }

    /* reverse-dependency check among remaining packages */
    str_list refs;
    sl_init(&refs);
    size_t i;
    for (i = 0; i < tdoc.n; i++) {
        const char *nm = d822_get(&tdoc.recs[i], "package");
        if (!nm)
            continue;
        int is_target = 0;
        int a;
        for (a = 0; a < argc; a++) {
            if (strcmp(nm, argv[a]) == 0) {
                is_target = 1;
                break;
            }
        }
        if (is_target)
            continue;
        dep_names(d822_get(&tdoc.recs[i], "depends"), &refs);
        dep_names(d822_get(&tdoc.recs[i], "pre-depends"), &refs);
        dep_names(d822_get(&tdoc.recs[i], "recommends"), &refs);
    }
    int blocked = 0;
    int a;
    for (a = 0; a < argc; a++) {
        size_t j;
        for (j = 0; j < refs.n; j++) {
            if (strcmp(refs.items[j], argv[a]) == 0) {
                fprintf(stderr, "reposync: cannot remove %s: still referenced by another package\n", argv[a]);
                blocked = 1;
                break;
            }
        }
    }
    sl_free(&refs);
    if (blocked) {
        pkgset_free(&ps);
        d822_doc_free(&tdoc);
        return -1;
    }

    /* verify presence, print plan */
    int nrm = 0;
    for (a = 0; a < argc; a++) {
        size_t exi = 0;
        if (rec_find(&tdoc, argv[a], &exi))
            nrm++;
        else
            printf("  %s: not in target repo\n", argv[a]);
    }
    if (nrm == 0) {
        printf("  nothing to remove\n");
        pkgset_free(&ps);
        d822_doc_free(&tdoc);
        return 0;
    }
    printf("The following packages will be REMOVED:\n  ");
    int shown = 0;
    for (a = 0; a < argc; a++) {
        size_t exi = 0;
        if (rec_find(&tdoc, argv[a], &exi)) {
            if (shown && shown % 6 == 0)
                printf("\n  ");
            printf("%s ", argv[a]);
            shown++;
        }
    }
    printf("\n%d to remove.\n", nrm);
    if (!confirm("Continue?", yes)) {
        printf("Abort.\n");
        pkgset_free(&ps);
        d822_doc_free(&tdoc);
        return 0;
    }

    int ok = 1;
    for (a = 0; a < argc; a++) {
        size_t exi = 0;
        if (!rec_find(&tdoc, argv[a], &exi))
            continue;
        if (run_cmd(t->remove_cmd, argv[a]) < 0)
            ok = 0;
        else
            printf("  removed %s\n", argv[a]);
    }
    pkgset_free(&ps);
    d822_doc_free(&tdoc);
    return ok ? 0 : -1;
}

/* ---------- autoremove ---------- */

int ops_autoremove(target_t *t, const char *list_dir, int argc, char **argv, int yes)
{
    (void)list_dir; (void)argc; (void)argv;
    if (!t->remove_cmd) {
        fprintf(stderr, "reposync: Remove-Cmd not set in reposync.conf\n");
        return -1;
    }
    pkgset_t ps;
    d822_doc tdoc;
    if (target_state(t, &ps, &tdoc) < 0) {
        pkgset_free(&ps);
        d822_doc_free(&tdoc);
        return -1;
    }

    /* referenced names: deps of every package + provides targets */
    str_list refs;
    sl_init(&refs);
    size_t i;
    for (i = 0; i < tdoc.n; i++) {
        dep_names(d822_get(&tdoc.recs[i], "depends"), &refs);
        dep_names(d822_get(&tdoc.recs[i], "pre-depends"), &refs);
        dep_names(d822_get(&tdoc.recs[i], "recommends"), &refs);
    }
    for (i = 0; i < tdoc.n; i++) {
        const char *pv = d822_get(&tdoc.recs[i], "provides");
        if (pv) {
            str_list l;
            sl_init(&l);
            sl_push_split(&l, pv);
            size_t j;
            for (j = 0; j < l.n; j++) {
                char tmp[256];
                snprintf(tmp, sizeof tmp, "%s", l.items[j]);
                char *p = strchr(tmp, '(');
                if (p)
                    *p = '\0';
                char *e = tmp + strlen(tmp);
                while (e > tmp && (e[-1] == ' ' || e[-1] == '\t'))
                    *--e = '\0';
                if (*tmp)
                    sl_push(&refs, tmp);
            }
            sl_free(&l);
        }
    }
    /* resolve virtual references: a package providing a referenced name is
       itself referenced (iterate to a fixpoint for provider chains) */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (i = 0; i < tdoc.n; i++) {
            const char *rname = d822_get(&tdoc.recs[i], "package");
            if (!rname || list_contains(&refs, rname))
                continue;
            const char *pv = d822_get(&tdoc.recs[i], "provides");
            if (!pv)
                continue;
            str_list l;
            sl_init(&l);
            sl_push_split(&l, pv);
            size_t j;
            for (j = 0; j < l.n; j++) {
                char tmp[256];
                snprintf(tmp, sizeof tmp, "%s", l.items[j]);
                char *p = strchr(tmp, '(');
                if (p)
                    *p = '\0';
                char *e = tmp + strlen(tmp);
                while (e > tmp && (e[-1] == ' ' || e[-1] == '\t'))
                    *--e = '\0';
                if (*tmp && list_contains(&refs, tmp)) {
                    sl_push(&refs, rname);
                    changed = 1;
                    break;
                }
            }
            sl_free(&l);
        }
    }

    str_list orph;
    sl_init(&orph);
    str_list manual;
    manual_load(&manual);
    for (i = 0; i < tdoc.n; i++) {
        const char *name = d822_get(&tdoc.recs[i], "package");
        if (!name)
            continue;
        size_t j;
        int found = 0;
        for (j = 0; j < refs.n; j++) {
            if (strcmp(refs.items[j], name) == 0) {
                found = 1;
                break;
            }
        }
        if (list_contains(&manual, name))
            continue; /* explicitly installed: never auto-removed */
        if (!found)
            sl_push(&orph, name);
    }
    sl_free(&manual);
    if (orph.n == 0) {
        printf("  no orphaned packages in target repo\n");
        sl_free(&orph);
        sl_free(&refs);
        pkgset_free(&ps);
        d822_doc_free(&tdoc);
        return 0;
    }
    printf("The following packages are no longer required and will be REMOVED:\n  ");
    for (i = 0; i < orph.n; i++) {
        if (i && i % 6 == 0)
            printf("\n  ");
        printf("%s ", orph.items[i]);
    }
    printf("\n%zu to remove.\n", orph.n);
    if (!confirm("Continue?", yes)) {
        printf("Abort.\n");
        sl_free(&orph);
        sl_free(&refs);
        pkgset_free(&ps);
        d822_doc_free(&tdoc);
        return 0;
    }

    int ok = 1;
    for (i = 0; i < orph.n; i++) {
        if (run_cmd(t->remove_cmd, orph.items[i]) < 0)
            ok = 0;
        else
            printf("  removed %s\n", orph.items[i]);
    }
    sl_free(&orph);
    sl_free(&refs);
    pkgset_free(&ps);
    d822_doc_free(&tdoc);
    return ok ? 0 : -1;
}
