/* reposync (CLI). Linux uses the fixed /etc|/var layout; Windows uses the
   portable %EXEDIR% layout (see 需求.md): config files and state live next
   to the executable. */

#include "config.h"
#include "deb822.h"
#include "db.h"
#include "fetch.h"
#include "index.h"
#include "ops.h"
#include "sim.h"
#include "target.h"
#include "util.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#ifndef TOOLNAME
#define TOOLNAME "reposync"
#endif

/* Config layout (Linux fixed /etc|/var; Windows %EXEDIR%):
     reposync.conf        global config (repo control command sets)
     target.sources       target repo apt view
     sources.list.d + "*.sources" files   upstream sources (DEB822 only)
     lists/               cached indexes
     packages.db          sqlite database (sim / install closure)        */
static char *cfg_dir_path(void)
{
#ifdef _WIN32
    return path_join(exe_dir(), "sources.list.d");
#else
    return xstrdup("/etc/" TOOLNAME "/sources.list.d");
#endif
}

static char *conf_file_path(void)
{
#ifdef _WIN32
    return path_join(exe_dir(), "reposync.conf");
#else
    return xstrdup("/etc/" TOOLNAME "/reposync.conf");
#endif
}

static char *target_src_path(void)
{
#ifdef _WIN32
    return path_join(exe_dir(), "target.sources");
#else
    return xstrdup("/etc/" TOOLNAME "/target.sources");
#endif
}

static char *list_dir_path(void)
{
#ifdef _WIN32
    return path_join(exe_dir(), "lists");
#else
    return xstrdup("/var/lib/" TOOLNAME "/lists");
#endif
}

static char *db_path(void)
{
#ifdef _WIN32
    return path_join(exe_dir(), "packages.db");
#else
    return xstrdup("/var/lib/" TOOLNAME "/packages.db");
#endif
}

static void usage(const char *prog)
{
    char *cfg = cfg_dir_path();
    char *tsrc = target_src_path();
    char *lst = list_dir_path();
    char *db = db_path();
    printf("usage:\n"
           "  %s update                 fetch Release + Packages indexes from configured sources\n"
           "  %s list [pkg]             list packages from cached indexes (name/version/arch/suite/component/desc)\n"
           "  %s download <pkg> [-o dir]  download .deb for pkg into dir (default ./)\n"
           "  %s fetch <url> <outfile>    raw download primitive\n"
           "  %s db-import               import dpkg status into SQLite database\n"
           "  %s install [-y] <pkg>...   pull packages from upstream into the target repo\n"
           "  %s upgrade [-y]            update packages in the target repo to newer versions\n"
           "  %s remove [-y] <pkg>...    delete packages from the target repo\n"
           "  %s autoremove [-y]         remove orphaned packages from the target repo\n"
           "  %s sim install [-y] <pkg>   simulate install (dep closure), db-only\n"
           "  %s sim remove [-y] <pkg>    simulate remove (reverse-dep checked), db-only\n"
           "  %s sim upgrade [-y]         simulate upgrade, db-only\n"
           "  %s help\n\n"
           "config:  %s\n"
           "global:  %s (repo control command sets)\n"
           "target:  %s (apt view)\n"
           "cache:   %s\n"
           "db:      %s\n"
           "env:     REPOSYNC_CONFIG_DIR, REPOSYNC_LIST_DIR, REPOSYNC_DB, REPOSYNC_STATUS,\n"
           "         REPOSYNC_TARGET, REPOSYNC_CONF (also REPOSYNC_TARGET_CONF), REPOSYNC_MANUAL override\n",
           prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
           prog, prog, prog, cfg, conf_file_path(), tsrc, lst, db);
    free(cfg);
    free(tsrc);
    free(lst);
    free(db);
}

static int target_op(int argc, char **argv,
                     int (*fn)(target_t *, const char *, int, char **, int))
{
    char *def_tsrc = target_src_path();
    char *def_tconf = conf_file_path();
    char *def_lst = list_dir_path();
    const char *tsrc = getenv("REPOSYNC_TARGET");
    if (!tsrc || !*tsrc)
        tsrc = def_tsrc;
    const char *tconf = getenv("REPOSYNC_CONF");
    if (!tconf || !*tconf)
        tconf = getenv("REPOSYNC_TARGET_CONF"); /* legacy alias */
    if (!tconf || !*tconf)
        tconf = def_tconf;
    const char *lst = getenv("REPOSYNC_LIST_DIR");
    if (!lst || !*lst)
        lst = def_lst;
    target_t t;
    if (target_load(tsrc, tconf, &t) < 0) {
        free(def_tsrc);
        free(def_tconf);
        free(def_lst);
        return 1;
    }
    int yes = 0;
    char **args = xmalloc((size_t)(argc + 1) * sizeof(char *));
    int n = 0;
    int i;
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-y") == 0)
            yes = 1;
        else
            args[n++] = argv[i];
    }
    int rc = fn(&t, lst, n, args, yes);
    free(args);
    target_free(&t);
    free(def_tsrc);
    free(def_tconf);
    free(def_lst);
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    /* libcurl global init (must precede any easy handle creation) */
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "reposync: libcurl init failed\n");
        return 1;
    }

    char *def_cfg = cfg_dir_path();
    char *def_lst = list_dir_path();
    const char *cfg = getenv("REPOSYNC_CONFIG_DIR");
    const char *lst = getenv("REPOSYNC_LIST_DIR");
    if (!cfg || !*cfg)
        cfg = def_cfg;
    if (!lst || !*lst)
        lst = def_lst;

    if (argc < 2) {
        usage(argv[0]);
        free(def_cfg);
        free(def_lst);
        return 1;
    }

    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        free(def_cfg);
        free(def_lst);
        return 0;
    }

    if (strcmp(argv[1], "update") == 0) {
        src_t *srcs = NULL;
        size_t n = 0;
        if (src_load_dir(cfg, &srcs, &n) < 0) {
            free(def_cfg);
            free(def_lst);
            return 1;
        }
        printf("reposync: %zu source(s) from %s\n", n, cfg);
        int rc = idx_update(lst, srcs, n);
        /* after a successful index fetch, rebuild the local candidates db
           (list/search then read the db, never the network) */
        char *def_db = db_path();
        const char *db = getenv("REPOSYNC_DB");
        if (!db || !*db)
            db = def_db;
        if (rc == 0) {
            if (db_candidates_rebuild(db, lst) < 0)
                rc = -1;
        }
        free(def_db);
        src_free_all(srcs, n);
        free(def_cfg);
        free(def_lst);
        return rc == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "list") == 0) {
        char *def_db = db_path();
        const char *db = getenv("REPOSYNC_DB");
        const char *pkg = argc >= 3 ? argv[2] : NULL;
        if (!db || !*db)
            db = def_db;
        int count = 0;
        int rc = db_candidates_query(db, pkg, NULL, NULL, &count);
        if (rc == 0)
            printf("reposync: %d package(s)\n", count);
        free(def_db);
        free(def_cfg);
        free(def_lst);
        return rc == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "download") == 0 && argc >= 3) {
        const char *pkg = argv[2];
        const char *outdir = ".";
        if (argc >= 5 && strcmp(argv[3], "-o") == 0)
            outdir = argv[4];
        char *saved = NULL;
        int rc = idx_download(lst, pkg, outdir, &saved);
        if (rc == 0 && saved)
            printf("reposync: OK %s\n", saved);
        free(saved);
        free(def_cfg);
        free(def_lst);
        return rc == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "fetch") == 0 && argc >= 4) {
        if (fetch_url_to_file(argv[2], argv[3]) < 0) {
            fprintf(stderr, "reposync: fetch failed\n");
            free(def_cfg);
            free(def_lst);
            return 1;
        }
        printf("reposync: OK %s\n", argv[3]);
        free(def_cfg);
        free(def_lst);
        return 0;
    }

    if (strcmp(argv[1], "db-import") == 0) {
        char *def_db = db_path();
        const char *db = getenv("REPOSYNC_DB");
        const char *st = getenv("REPOSYNC_STATUS");
        if (!db || !*db)
            db = def_db;
        if (!st || !*st)
            st = "/var/lib/dpkg/status";
        int rc = db_import(db, st) == 0 ? 0 : 1;
        free(def_db);
        free(def_cfg);
        free(def_lst);
        return rc;
    }

    if (strcmp(argv[1], "install") == 0 && argc >= 3)
        return target_op(argc, argv, ops_install);

    if (strcmp(argv[1], "remove") == 0 && argc >= 3)
        return target_op(argc, argv, ops_remove);

    if (strcmp(argv[1], "upgrade") == 0)
        return target_op(argc, argv, ops_upgrade);

    if (strcmp(argv[1], "autoremove") == 0)
        return target_op(argc, argv, ops_autoremove);

    if (strcmp(argv[1], "sim") == 0 && argc >= 3) {
        char *def_db = db_path();
        const char *db = getenv("REPOSYNC_DB");
        const char *lstd = getenv("REPOSYNC_LIST_DIR");
        if (!db || !*db)
            db = def_db;
        if (!lstd || !*lstd)
            lstd = lst;
        int yes = 0;
        int pos = 2;
        if (strcmp(argv[2], "-y") == 0) {
            yes = 1;
            pos = 3;
        }
        if (strcmp(argv[pos], "install") == 0) {
            const char *pkg = NULL;
            int y = yes;
            int i;
            for (i = pos + 1; i < argc; i++) {
                if (strcmp(argv[i], "-y") == 0)
                    y = 1;
                else if (!pkg)
                    pkg = argv[i];
            }
            if (pkg) {
                int rc = sim_install(db, lstd, pkg, y) == 0 ? 0 : 1;
                free(def_db);
                free(def_cfg);
                free(def_lst);
                return rc;
            }
        }
        if (strcmp(argv[pos], "remove") == 0) {
            const char *pkg = NULL;
            int y = yes;
            int i;
            for (i = pos + 1; i < argc; i++) {
                if (strcmp(argv[i], "-y") == 0)
                    y = 1;
                else if (!pkg)
                    pkg = argv[i];
            }
            if (pkg) {
                int rc = sim_remove(db, pkg, y) == 0 ? 0 : 1;
                free(def_db);
                free(def_cfg);
                free(def_lst);
                return rc;
            }
        }
        if (strcmp(argv[pos], "upgrade") == 0) {
            int y = yes;
            int i;
            for (i = pos + 1; i < argc; i++)
                if (strcmp(argv[i], "-y") == 0)
                    y = 1;
            int rc = sim_upgrade(db, lstd, y) == 0 ? 0 : 1;
            free(def_db);
            free(def_cfg);
            free(def_lst);
            return rc;
        }
        free(def_db);
    }

    usage(argv[0]);
    free(def_cfg);
    free(def_lst);
    return 1;
}
