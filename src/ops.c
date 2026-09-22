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
    /* REPOSYNC_TMPDIR overrides the scratch dir (large source closures can
       exceed /tmp capacity on small tmpfs mounts) */
    const char *td = getenv("REPOSYNC_TMPDIR");
    char *d = xmalloc(strlen(td && *td ? td : "/tmp") + 1 + 22);
    sprintf(d, "%s/reposync-inst-XXXXXX", td && *td ? td : "/tmp");
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

/* Substitute %suite% / %comp% tokens in a command template. Templates without
   the tokens are returned unchanged, so single-suite legacy configs keep
   working. Returns malloc'd. */
static char *cmd_subst(const char *templ, const char *suite, const char *comp)
{
    sb_t out;
    sb_init(&out);
    const char *p = templ;
    while (*p) {
        if (strncmp(p, "%suite%", 7) == 0) {
            if (suite)
                sb_append_str(&out, suite);
            p += 7;
        } else if (strncmp(p, "%comp%", 6) == 0) {
            if (comp)
                sb_append_str(&out, comp);
            p += 6;
        } else {
            sb_append_char(&out, *p);
            p++;
        }
    }
    return sb_detach(&out);
}

/* Warn (once per import batch) when the target has several declared suites
   but the command template is not suite-parameterized. */
static void warn_no_suite_param(const target_t *t, const char *what,
                                const char *cmd)
{
    if (t->suites.n > 1 && !(cmd && strstr(cmd, "%suite%")))
        fprintf(stderr,
                "reposync: warning: target declares %zu suites but %s has no "
                "%%suite%% placeholder; imports use the command as configured\n",
                t->suites.n, what);
}

/* Fetch and import every package in changed (deps-first order) via
   target.import_cmd. Each package is routed to the target (suite, component)
   chosen by target_map_suite (same-name first; abc-* -> base fallback only);
   packages with no allowed mapping are skipped with a warning (best-effort
   sync). Downloads land in a temp dir. Returns 0 on success. */
static int import_changed(target_t *t, const char *list_dir, candset_t *cs,
                          str_list *changed)
{
    if (!t->import_cmd) {
        fprintf(stderr, "reposync: Import-Cmd not set in reposync.conf\n");
        return -1;
    }
    warn_no_suite_param(t, "Import-Cmd", t->import_cmd);
    char *tmpdir = mk_tmpdir();
    if (!tmpdir) {
        fprintf(stderr, "reposync: mkdtemp failed\n");
        return -1;
    }
    int ok = 1;
    size_t i;
    for (i = 0; i < changed->n; i++) {
        const char *pkg = changed->items[i];
        const cand_t *c = candset_find(cs, pkg);
        if (!c || !c->suite || !c->comp) {
            fprintf(stderr, "reposync: warning: skipping %s: source suite "
                    "unknown\n", pkg);
            continue;
        }
        char *ms = NULL, *mc = NULL;
        if (target_map_suite(t, c->suite, c->comp, &ms, &mc) == 0) {
            fprintf(stderr, "reposync: warning: skipping %s: no allowed suite "
                    "mapping for %s/%s (suite mapping rules)\n",
                    pkg, c->suite, c->comp);
            continue;
        }
        char *cmd = cmd_subst(t->import_cmd, ms, mc);
        free(ms);
        free(mc);
        char *saved = NULL;
        if (idx_download(list_dir, pkg, tmpdir, &saved) < 0) {
            fprintf(stderr, "reposync: cannot download %s\n", pkg);
            ok = 0;
            free(cmd);
            continue;
        }
        if (run_cmd(cmd, saved) < 0) {
            ok = 0;
        } else {
            printf("  imported %s\n", pkg);
        }
        free(saved);
        free(cmd);
    }
    rmdir(tmpdir);
    free(tmpdir);
    return ok ? 0 : -1;
}

/* Fetch and import every source package in srcs via target.source_import_cmd.
   Each source is routed by target_map_suite like binary packages; sources
   with no allowed mapping are skipped with a warning. Downloads land in a
   temp dir. Returns 0 on success. */
static int import_sources(target_t *t, const char *list_dir, srcset_t *ss,
                          str_list *srcs)
{
    if (!t->source_import_cmd) {
        fprintf(stderr, "reposync: Sync-Sources: yes but Source-Import-Cmd not set in reposync.conf\n");
        return -1;
    }
    warn_no_suite_param(t, "Source-Import-Cmd", t->source_import_cmd);
    char *tmpdir = mk_tmpdir();
    if (!tmpdir) {
        fprintf(stderr, "reposync: mkdtemp failed\n");
        return -1;
    }
    int ok = 1;
    size_t i;
    for (i = 0; i < srcs->n; i++) {
        const char *src = srcs->items[i];
        const srcpkg_t *s = srcset_find(ss, src);
        if (!s || !s->suite || !s->comp) {
            fprintf(stderr, "reposync: warning: skipping source %s: source "
                    "suite unknown\n", src);
            continue;
        }
        char *ms = NULL, *mc = NULL;
        if (target_map_suite(t, s->suite, s->comp, &ms, &mc) == 0) {
            fprintf(stderr, "reposync: warning: skipping source %s: no allowed "
                    "suite mapping for %s/%s (suite mapping rules)\n",
                    src, s->suite, s->comp);
            continue;
        }
        char *cmd = cmd_subst(t->source_import_cmd, ms, mc);
        free(ms);
        free(mc);
        char *dsc = NULL;
        if (idx_download_source(list_dir, src, tmpdir, &dsc) < 0) {
            fprintf(stderr, "reposync: cannot download source %s\n", src);
            ok = 0;
            free(cmd);
            continue;
        }
        if (run_cmd(cmd, dsc) < 0) {
            ok = 0;
        } else {
            printf("  imported source %s\n", src);
        }
        free(dsc);
        free(cmd);
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

    /* Unified closure (需求.md "仓库后端与 Sources 同步"): with
       Sync-Sources: yes the import set is the bidirectional closure
       (binary Depends/Pre-Depends <-> source Build-Depends/Binary),
       otherwise the plain binary closure. Both sides are filtered against
       what the target repo already carries at >= upstream version. */
    srcset_t ss;
    srcset_init(&ss);
    str_list bin_import, src_import, src_out, src_bin;
    sl_init(&bin_import); sl_init(&src_import);
    sl_init(&src_out); sl_init(&src_bin);
    int have_sources = 0;
    pkgset_t tps;            /* snapshot of the REAL target state: sim_closure_solve
                                 mutates ps as if the closure were applied, so the
                                 already-present check must not use ps */
    pkgset_init(&tps);
    if (target_state(t, &tps, NULL) < 0) {
        pkgset_free(&tps);
        sl_free(&targets);
        sl_free(&cl.queue);
        sl_free(&cl.changed);
        sl_free(&cl.upgraded);
        pkgset_free(&ps);
        candset_free(&cs);
        d822_doc_free(&tdoc);
        return -1;
    }
    if (t->sync_sources > 0) {
        if (srcset_load(list_dir, &ss) == 0 &&
            sim_source_closure(&cs, &ss, t->arch, &targets,
                               &src_bin, &src_out) == 0) {
            srcset_t tss;
            srcset_init(&tss);
            target_sources_state(t, &tss);
            size_t si;
            for (si = 0; si < src_out.n; si++) {
                const char *sn = src_out.items[si];
                srcpkg_t *have = srcset_find(&tss, sn);
                srcpkg_t *up = srcset_find(&ss, sn);
                if (have && up && deb_vercmp(have->version, up->version) >= 0)
                    continue; /* already synced at >= upstream */
                sl_push(&src_import, sn);
            }
            srcset_free(&tss);
            for (si = 0; si < src_bin.n; si++) {
                const char *bn = src_bin.items[si];
                cand_t *up = candset_find(&cs, bn);
                pkg_t *have = pkgset_find(&tps, bn);
                if (!up)
                    continue;
                if (have && deb_vercmp(have->version, up->p.version) >= 0)
                    continue; /* already at >= upstream */
                sl_push(&bin_import, bn);
            }
            have_sources = 1;
        }
    } else {
        size_t si;
        for (si = 0; si < cl.changed.n; si++)
            sl_push(&bin_import, cl.changed.items[si]);
    }
    sl_free(&src_out);
    sl_free(&src_bin);


    if (bin_import.n == 0 && src_import.n == 0) {
        printf("  all requested package(s) already in target repo\n");
        sl_free(&targets);
        sl_free(&cl.queue);
        sl_free(&cl.changed);
        sl_free(&cl.upgraded);
        sl_free(&bin_import);
        sl_free(&src_import);
        pkgset_free(&tps);
        srcset_free(&ss);
        pkgset_free(&ps);
        candset_free(&cs);
        d822_doc_free(&tdoc);
        return 0;
    }

    str_list newonly, upgraded;
    sl_init(&newonly); sl_init(&upgraded);
    size_t i;
    for (i = 0; i < bin_import.n; i++) {
        if (pkgset_find(&tps, bin_import.items[i]))
            sl_push(&upgraded, bin_import.items[i]);
        else
            sl_push(&newonly, bin_import.items[i]);
    }
    printf("The following NEW packages will be imported:\n  ");
    for (i = 0; i < newonly.n; i++) {
        if (i && i % 6 == 0)
            printf("\n  ");
        printf("%s ", newonly.items[i]);
    }
    printf("\n");
    if (upgraded.n) {
        printf("The following packages will be upgraded in target:\n  ");
        for (i = 0; i < upgraded.n; i++) {
            if (i && i % 6 == 0)
                printf("\n  ");
            printf("%s ", upgraded.items[i]);
        }
        printf("\n");
    }
    printf("%zu new, %zu upgraded.\n", newonly.n, upgraded.n);
    sl_free(&newonly); sl_free(&upgraded);
    if (have_sources && src_import.n) {
        printf("The following SOURCE packages will be imported:\n  ");
        for (i = 0; i < src_import.n; i++) {
            if (i && i % 6 == 0)
                printf("\n  ");
            printf("%s ", src_import.items[i]);
        }
        printf("\n%zu source(s).\n", src_import.n);
    }

    if (!confirm("Continue?", yes)) {
        printf("Abort.\n");
        sl_free(&targets);
        sl_free(&cl.queue);
        sl_free(&cl.changed);
        sl_free(&cl.upgraded);
        sl_free(&bin_import);
        sl_free(&src_import);
        pkgset_free(&tps);
        srcset_free(&ss);
        pkgset_free(&ps);
        candset_free(&cs);
        d822_doc_free(&tdoc);
        return 0;
    }

    int rc = import_changed(t, list_dir, &cs, &bin_import);
    if (rc == 0 && have_sources)
        rc = import_sources(t, list_dir, &ss, &src_import);
    if (rc == 0)
        manual_add(&targets); /* explicit requests survive autoremove */
    sl_free(&targets);
    sl_free(&cl.queue);
    sl_free(&cl.changed);
    sl_free(&cl.upgraded);
    sl_free(&bin_import);
    sl_free(&src_import);
    pkgset_free(&tps);
    srcset_free(&ss);
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

    int rc = import_changed(t, list_dir, &cs, &cl.changed);
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
