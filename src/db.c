#include "db.h"
#include "deb822.h"
#include "util.h"
#include <dirent.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS meta ("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS packages ("
    "  name TEXT PRIMARY KEY,"
    "  version TEXT NOT NULL,"
    "  architecture TEXT NOT NULL,"
    "  status TEXT,"
    "  essential INTEGER NOT NULL DEFAULT 0,"
    "  multi_arch TEXT,"
    "  section TEXT,"
    "  priority TEXT,"
    "  source TEXT,"
    "  depends TEXT,"
    "  pre_depends TEXT,"
    "  recommends TEXT,"
    "  suggests TEXT,"
    "  conflicts TEXT,"
    "  breaks TEXT,"
    "  replaces TEXT,"
    "  provides TEXT,"
    "  description TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_packages_status ON packages(status);"
    "CREATE TABLE IF NOT EXISTS index_packages ("
    "  name TEXT NOT NULL,"
    "  version TEXT NOT NULL,"
    "  architecture TEXT NOT NULL,"
    "  suite TEXT NOT NULL,"
    "  component TEXT NOT NULL,"
    "  section TEXT,"
    "  description TEXT,"
    "  PRIMARY KEY (name, version, architecture, suite, component)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_index_sc ON index_packages(suite, component);"
    "CREATE INDEX IF NOT EXISTS idx_index_name ON index_packages(name);";

static void die_sqlite(sqlite3 *db, const char *what)
{
    fprintf(stderr, "reposync: %s: %s\n", what, sqlite3_errmsg(db));
    sqlite3_close(db);
    exit(1);
}

static void bind_or_null(sqlite3_stmt *st, int idx, const char *v)
{
    if (v)
        sqlite3_bind_text(st, idx, v, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, idx);
}

static int is_essential(const d822_rec *r)
{
    const char *e = d822_get(r, "essential");
    return e && (strcasecmp(e, "yes") == 0);
}

int db_import(const char *db_path, const char *status_path)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "reposync: cannot open db %s: %s\n", db_path,
                db ? sqlite3_errmsg(db) : "unknown");
        if (db)
            sqlite3_close(db);
        return -1;
    }

    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN;", NULL, NULL, &err) != SQLITE_OK)
        die_sqlite(db, err ? err : "BEGIN");
    if (sqlite3_exec(db, "DROP TABLE IF EXISTS packages;", NULL, NULL, &err) != SQLITE_OK)
        die_sqlite(db, err ? err : "DROP");
    if (sqlite3_exec(db, SCHEMA, NULL, NULL, &err) != SQLITE_OK)
        die_sqlite(db, err ? err : "SCHEMA");

    const char *SQL =
        "INSERT OR REPLACE INTO packages"
        "(name, version, architecture, status, essential, multi_arch, section,"
        " priority, source, depends, pre_depends, recommends, suggests, conflicts,"
        " breaks, replaces, provides, description)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18);";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, SQL, -1, &st, NULL) != SQLITE_OK)
        die_sqlite(db, "prepare insert");

    sb_t content;
    sb_init(&content);
    if (read_file(status_path, &content) < 0) {
        fprintf(stderr, "reposync: cannot read %s\n", status_path);
        sqlite3_finalize(st);
        sqlite3_close(db);
        sb_free(&content);
        return -1;
    }

    d822_doc doc;
    if (d822_parse(content.s, content.len, &doc) < 0) {
        fprintf(stderr, "reposync: parse error in %s\n", status_path);
        sqlite3_finalize(st);
        sqlite3_close(db);
        sb_free(&content);
        return -1;
    }

    size_t i, total = 0, installed = 0;
    for (i = 0; i < doc.n; i++) {
        const d822_rec *r = &doc.recs[i];
        const char *name = d822_get(r, "package");
        const char *version = d822_get(r, "version");
        if (!name || !version)
            continue;
        const char *status = d822_get(r, "status");
        const char *arch = d822_get(r, "architecture");
        if (!arch || !*arch)
            arch = native_deb_arch();
        if (status && strcmp(status, "install ok installed") == 0)
            installed++;

        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, version, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, arch, -1, SQLITE_TRANSIENT);
        bind_or_null(st, 4, status);
        sqlite3_bind_int(st, 5, is_essential(r) ? 1 : 0);
        bind_or_null(st, 6, d822_get(r, "multi-arch"));
        bind_or_null(st, 7, d822_get(r, "section"));
        bind_or_null(st, 8, d822_get(r, "priority"));
        bind_or_null(st, 9, d822_get(r, "source"));
        bind_or_null(st, 10, d822_get(r, "depends"));
        bind_or_null(st, 11, d822_get(r, "pre-depends"));
        bind_or_null(st, 12, d822_get(r, "recommends"));
        bind_or_null(st, 13, d822_get(r, "suggests"));
        bind_or_null(st, 14, d822_get(r, "conflicts"));
        bind_or_null(st, 15, d822_get(r, "breaks"));
        bind_or_null(st, 16, d822_get(r, "replaces"));
        bind_or_null(st, 17, d822_get(r, "provides"));
        bind_or_null(st, 18, d822_get(r, "description"));

        if (sqlite3_step(st) != SQLITE_DONE)
            die_sqlite(db, "insert");
        total++;
    }
    sqlite3_finalize(st);

    /* meta */
    char ts[32];
    snprintf(ts, sizeof ts, "%lld", (long long)time(NULL));
    char nbuf[32], ibuf[32];
    snprintf(nbuf, sizeof nbuf, "%zu", total);
    snprintf(ibuf, sizeof ibuf, "%zu", installed);

    sqlite3_stmt *mst = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO meta(key,value) VALUES(?1,?2);", -1, &mst, NULL);
    const char *mk[][2] = {
        {"schema_version", "1"},
        {"imported_at", ts},
        {"status_file", status_path},
        {"host_arch", native_deb_arch()},
        {"record_count", nbuf},
        {"installed_count", ibuf},
    };
    size_t m;
    for (m = 0; m < sizeof mk / sizeof mk[0]; m++) {
        sqlite3_reset(mst);
        sqlite3_bind_text(mst, 1, mk[m][0], -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(mst, 2, mk[m][1], -1, SQLITE_TRANSIENT);
        if (sqlite3_step(mst) != SQLITE_DONE)
            die_sqlite(db, "meta insert");
    }
    sqlite3_finalize(mst);

    if (sqlite3_exec(db, "COMMIT;", NULL, NULL, &err) != SQLITE_OK)
        die_sqlite(db, err ? err : "COMMIT");
    sqlite3_close(db);

    d822_doc_free(&doc);
    sb_free(&content);

    printf("reposync: imported %zu package records (%zu installed) from %s -> %s\n",
           total, installed, status_path, db_path);
    return 0;
}

int db_load_all(const char *db_path, pkgset_t *ps)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "reposync: cannot open db %s: %s\n", db_path,
                db ? sqlite3_errmsg(db) : "unknown");
        if (db)
            sqlite3_close(db);
        return -1;
    }
    const char *SQL =
        "SELECT name,version,architecture,status,essential,multi_arch,section,"
        "priority,source,depends,pre_depends,recommends,suggests,conflicts,"
        "breaks,replaces,provides,description FROM packages;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, SQL, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "reposync: db query: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        pkg_t p;
        pkg_init(&p);
        const char *v;
        v = (const char *)sqlite3_column_text(st, 0);
        if (v) p.name = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 1);
        if (v) p.version = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 2);
        if (v) p.arch = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 3);
        if (v) p.status = xstrdup(v);
        p.essential = sqlite3_column_int(st, 4);
        v = (const char *)sqlite3_column_text(st, 5);
        if (v) p.multi_arch = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 6);
        if (v) p.section = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 7);
        if (v) p.priority = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 8);
        if (v) p.source = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 9);
        if (v) p.depends = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 10);
        if (v) p.pre_depends = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 11);
        if (v) p.recommends = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 12);
        if (v) p.suggests = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 13);
        if (v) p.conflicts = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 14);
        if (v) p.breaks = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 15);
        if (v) p.replaces = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 16);
        if (v) p.provides = xstrdup(v);
        v = (const char *)sqlite3_column_text(st, 17);
        if (v) p.description = xstrdup(v);
        if (pkgset_put(ps, &p) < 0) {
            pkg_free(&p);
            sqlite3_finalize(st);
            sqlite3_close(db);
            return -1;
        }
        pkg_free(&p);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

static void meta_put_count(sqlite3 *db, const char *key, const char *val)
{
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO meta(key,value) VALUES(?1,?2);", -1, &st, NULL);
    if (st) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, val, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

int db_write_all(const char *db_path, const pkgset_t *ps)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "reposync: cannot open db %s\n", db_path);
        if (db)
            sqlite3_close(db);
        return -1;
    }
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN;", NULL, NULL, &err) != SQLITE_OK)
        die_sqlite(db, err ? err : "BEGIN");
    if (sqlite3_exec(db, "DELETE FROM packages;", NULL, NULL, &err) != SQLITE_OK)
        die_sqlite(db, err ? err : "DELETE");

    const char *SQL =
        "INSERT OR REPLACE INTO packages"
        "(name, version, architecture, status, essential, multi_arch, section,"
        " priority, source, depends, pre_depends, recommends, suggests, conflicts,"
        " breaks, replaces, provides, description)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18);";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, SQL, -1, &st, NULL) != SQLITE_OK)
        die_sqlite(db, "prepare insert");

    size_t i, installed = 0;
    for (i = 0; i < ps->n; i++) {
        const pkg_t *p = &ps->items[i];
        if (p->status && strcmp(p->status, "install ok installed") == 0)
            installed++;
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        bind_or_null(st, 1, p->name);
        bind_or_null(st, 2, p->version);
        bind_or_null(st, 3, p->arch);
        bind_or_null(st, 4, p->status);
        sqlite3_bind_int(st, 5, p->essential ? 1 : 0);
        bind_or_null(st, 6, p->multi_arch);
        bind_or_null(st, 7, p->section);
        bind_or_null(st, 8, p->priority);
        bind_or_null(st, 9, p->source);
        bind_or_null(st, 10, p->depends);
        bind_or_null(st, 11, p->pre_depends);
        bind_or_null(st, 12, p->recommends);
        bind_or_null(st, 13, p->suggests);
        bind_or_null(st, 14, p->conflicts);
        bind_or_null(st, 15, p->breaks);
        bind_or_null(st, 16, p->replaces);
        bind_or_null(st, 17, p->provides);
        bind_or_null(st, 18, p->description);
        if (sqlite3_step(st) != SQLITE_DONE)
            die_sqlite(db, "insert");
    }
    sqlite3_finalize(st);

    char nbuf[32], ibuf[32];
    snprintf(nbuf, sizeof nbuf, "%zu", ps->n);
    snprintf(ibuf, sizeof ibuf, "%zu", installed);
    meta_put_count(db, "record_count", nbuf);
    meta_put_count(db, "installed_count", ibuf);

    if (sqlite3_exec(db, "COMMIT;", NULL, NULL, &err) != SQLITE_OK)
        die_sqlite(db, err ? err : "COMMIT");
    sqlite3_close(db);
    return 0;
}

/* ---------------- candidates table (upstream index) ---------------- */

/* The candidates table holds the upstream package index: built by
   update (from the cached *_Packages files + their .meta), queried by
   list / search. Query operations never touch the network. */

static sqlite3 *db_open_path(const char *db_path)
{
    /* make sure the parent dir exists (/var/lib/reposync, %EXEDIR%) */
    const char *slash = strrchr(db_path, '/');
#ifdef _WIN32
    {
        const char *bs = strrchr(db_path, '\\');
        if (bs > slash)
            slash = bs;
    }
#endif
    if (slash && slash != db_path) {
        char *dir = xmalloc((size_t)(slash - db_path) + 1);
        memcpy(dir, db_path, (size_t)(slash - db_path));
        dir[slash - db_path] = '\0';
        if (mkdir_p(dir, 0755) != 0)
            fprintf(stderr, "reposync: cannot create %s: %s\n", dir,
                    strerror(errno));
        free(dir);
    }
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "reposync: cannot open db %s: %s\n", db_path,
                db ? sqlite3_errmsg(db) : "unknown");
        if (db)
            sqlite3_close(db);
        return NULL;
    }
    char *err = NULL;
    if (sqlite3_exec(db, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "reposync: db schema: %s\n", err ? err : "schema");
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

int db_candidates_rebuild(const char *db_path, const char *list_dir)
{
    DIR *d = opendir(list_dir);
    if (!d) {
        fprintf(stderr, "reposync: cannot open %s: %s\n", list_dir,
                strerror(errno));
        return -1;
    }
    sqlite3 *db = db_open_path(db_path);
    if (!db) {
        closedir(d);
        return -1;
    }
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN;", NULL, NULL, &err) != SQLITE_OK)
        die_sqlite(db, err ? err : "BEGIN");
    if (sqlite3_exec(db, "DELETE FROM index_packages;", NULL, NULL, &err) !=
        SQLITE_OK)
        die_sqlite(db, err ? err : "DELETE");

    const char *SQL =
        "INSERT OR REPLACE INTO index_packages"
        "(name, version, architecture, suite, component, section, description)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7);";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, SQL, -1, &st, NULL) != SQLITE_OK)
        die_sqlite(db, "prepare candidates insert");

    size_t total = 0;
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
        /* suite/component from the .meta written at update time */
        char *mp = xmalloc(strlen(p) + 6);
        sprintf(mp, "%s.meta", p);
        sb_t m;
        sb_init(&m);
        char *suite = NULL, *comp = NULL;
        if (read_file(mp, &m) == 0) {
            d822_doc md;
            d822_parse(m.s, m.len, &md);
            if (md.n > 0) {
                const char *su = d822_get(&md.recs[0], "suite");
                const char *co = d822_get(&md.recs[0], "component");
                if (su)
                    suite = xstrdup(su);
                if (co)
                    comp = xstrdup(co);
            }
            d822_doc_free(&md);
        }
        sb_free(&m);
        free(mp);

        d822_doc doc;
        d822_parse(content.s, content.len, &doc);
        size_t i;
        for (i = 0; i < doc.n; i++) {
            const d822_rec *r = &doc.recs[i];
            const char *name = d822_get(r, "package");
            const char *version = d822_get(r, "version");
            if (!name || !version)
                continue;
            const char *arch = d822_get(r, "architecture");
            if (!arch || !*arch)
                arch = "all";
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
            sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, version, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, arch, -1, SQLITE_TRANSIENT);
            bind_or_null(st, 4, suite);
            bind_or_null(st, 5, comp);
            bind_or_null(st, 6, d822_get(r, "section"));
            bind_or_null(st, 7, d822_get(r, "description"));
            if (sqlite3_step(st) != SQLITE_DONE)
                die_sqlite(db, "candidates insert");
            total++;
        }
        d822_doc_free(&doc);
        free(suite);
        free(comp);
        free(p);
        sb_free(&content);
    }
    closedir(d);
    sqlite3_finalize(st);

    char nbuf[32];
    snprintf(nbuf, sizeof nbuf, "%zu", total);
    meta_put_count(db, "candidate_count", nbuf);

    if (sqlite3_exec(db, "COMMIT;", NULL, NULL, &err) != SQLITE_OK)
        die_sqlite(db, err ? err : "COMMIT");
    sqlite3_close(db);
    printf("reposync: %zu candidate record(s) indexed in %s\n", total, db_path);
    return 0;
}

static void print_tab_field(const char *s)
{
    if (!s)
        return;
    for (; *s; s++) {
        if (*s == '\t' || *s == '\n' || *s == '\r')
            putchar(' ');
        else
            putchar((unsigned char)*s);
    }
}

int db_candidates_query(const char *db_path, const char *pkg,
                        const char *suite, const char *comp, int *count)
{
    *count = 0;
    sqlite3 *db = db_open_path(db_path);
    if (!db)
        return -1;

    const char *SQL =
        "SELECT name,version,architecture,suite,component,description"
        " FROM index_packages"
        " WHERE (?1 IS NULL OR name=?1)"
        "   AND (?2 IS NULL OR suite=?2)"
        "   AND (?3 IS NULL OR component=?3)"
        " ORDER BY name, version;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, SQL, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "reposync: db query: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    bind_or_null(st, 1, pkg);
    bind_or_null(st, 2, suite);
    bind_or_null(st, 3, comp);

    printf("# name\tversion\tarch\tsuite\tcomponent\tdescription\n");
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        const char *ver = (const char *)sqlite3_column_text(st, 1);
        const char *arch = (const char *)sqlite3_column_text(st, 2);
        const char *su = (const char *)sqlite3_column_text(st, 3);
        const char *co = (const char *)sqlite3_column_text(st, 4);
        const char *desc = (const char *)sqlite3_column_text(st, 5);
        print_tab_field(name ? name : "");
        putchar('\t');
        print_tab_field(ver ? ver : "");
        putchar('\t');
        print_tab_field(arch ? arch : "");
        putchar('\t');
        print_tab_field(su ? su : "");
        putchar('\t');
        print_tab_field(co ? co : "");
        putchar('\t');
        print_tab_field(desc ? desc : "");
        putchar('\n');
        (*count)++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}
