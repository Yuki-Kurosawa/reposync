#ifndef REPOSYNC_SIM_H
#define REPOSYNC_SIM_H

#include <stddef.h>
#include "list.h"

/* One package state (superset used for installed db rows and candidates). */
typedef struct {
    char *name;
    char *version;
    char *arch;
    char *status;          /* db rows: "install ok installed" etc. */
    int   essential;
    char *multi_arch;
    char *section;
    char *priority;
    char *source;
    char *depends;
    char *pre_depends;
    char *recommends;
    char *suggests;
    char *conflicts;
    char *breaks;
    char *replaces;
    char *provides;
    char *description;
} pkg_t;

/* Indexed set of packages (hash by name -> index+1). */
typedef struct {
    pkg_t *items;
    size_t n, cap;
    int *hash;
    size_t hcap;
} pkgset_t;

/* Candidate from remote Packages index (extends pkg_t with sizes and the
   source-suite attribution needed for sync suite mapping). */
typedef struct {
    pkg_t p;
    long long size;
    long long installed_size;
    char *suite;   /* upstream suite this candidate came from (cache .meta) */
    char *comp;    /* upstream component this candidate came from */
} cand_t;

typedef struct {
    cand_t *items;
    size_t n, cap;
    int *hash;
    size_t hcap;
} candset_t;

void pkg_init(pkg_t *p);
void pkg_copy(pkg_t *dst, const pkg_t *src);
void pkg_free(pkg_t *p);

void pkgset_init(pkgset_t *ps);
void pkgset_free(pkgset_t *ps);
pkg_t *pkgset_find(pkgset_t *ps, const char *name);
/* Copy p into set, inserting or replacing (updating hash). */
int pkgset_put(pkgset_t *ps, const pkg_t *p);
/* Remove entry by name (hash updated). Returns 1 if removed. */
int pkgset_del(pkgset_t *ps, const char *name);

void candset_init(candset_t *cs);
void candset_free(candset_t *cs);
cand_t *candset_find(candset_t *cs, const char *name);
/* Merge candidate; keeps highest version per name. */
int candset_merge(candset_t *cs, const cand_t *c);

/* Load candidate index from cached lists dir. native_arch filters binary arch. */
int candset_load(const char *list_dir, candset_t *cs, const char *native_arch);

/* Dependency-closure solver shared by sim and repo ops.
   ps = current state (installed set), cs = upstream candidates,
   targets = requested package names. On success cl->changed holds every
   package that must be installed/upgraded (cl->upgraded = subset already in
   ps at a lower version). ps is updated as if the closure were applied. */
typedef struct {
    str_list queue;
    str_list changed;    /* packages that will change (new install or upgrade) */
    str_list upgraded;   /* subset of changed that were already installed */
} closure_t;
/* ensure_deps != 0 additionally checks hard deps of already-present packages
   and queues missing ones (repo repair). sim uses 0; repo ops use 1. */
int sim_closure_solve(pkgset_t *ps, candset_t *cs, const char *native,
                      const str_list *targets, closure_t *cl, int ensure_deps);

/* ---------------- Sources (source packages) ---------------- */

/* One source package record (a Sources index entry). */
typedef struct {
    char *name;           /* Package */
    char *version;        /* Version */
    char *binary;         /* Binary: comma-separated binary package names */
    char *build_depends;  /* Build-Depends */
    char *directory;      /* Directory (pool-relative path of the .dsc family) */
    char *files;          /* Files: raw multi-line value (md5 size name) */
    char *uri;            /* base URI of the source (from the cache .meta) */
    char *suite;          /* upstream suite this source came from */
    char *comp;           /* upstream component this source came from */
} srcpkg_t;

/* Indexed set of source packages, with a reverse hash
   "binary package name -> source package" built from Binary:. */
typedef struct {
    srcpkg_t *items;
    size_t n, cap;
    int *hash;
    size_t hcap;
    char **balias;    /* binary alias names (one per Binary: entry) */
    size_t *bsrc;     /* alias -> source items index */
    size_t bn, bcap;
    int *bhash;       /* alias name -> balias index+1 */
    size_t bhcap;
} srcset_t;

void srcset_init(srcset_t *ss);
void srcset_free(srcset_t *ss);
srcpkg_t *srcset_find(srcset_t *ss, const char *name);
/* Insert (or version-merge) a source record identified by name/version;
   returns the stored record (other fields zeroed/NULL). */
srcpkg_t *srcset_put(srcset_t *ss, const char *name, const char *version);
/* Reverse lookup: the source package that builds binary package bin. */
srcpkg_t *srcset_find_bin(srcset_t *ss, const char *bin);
/* Load source records from cached lists dir (*_Sources files + .meta URI).
   Returns 0 on success. */
int srcset_load(const char *list_dir, srcset_t *ss);

/* Bidirectional closure (需求.md "Sources 同步"):
   from the target package names, expand both the binary side
   (Depends + Pre-Depends of every reached binary package) and the source side
   (Build-Depends of every reached source package, plus Binary: of every
   reached source package); a reached binary pulls in its source package and a
   reached source pulls in its binary packages, recursing to a fixpoint.
   out_bin / out_src receive the reached binary / source package names
   (deduplicated, source side only the source names; the binary side is a
   superset needed to derive the sources — callers normally reuse their own
   binary closure instead). Returns 0 on success, -1 if a target cannot be
   located either as a binary or as a source package. */
int sim_source_closure(candset_t *cs, srcset_t *ss, const char *native,
                       const str_list *targets, str_list *out_bin, str_list *out_src);

/* Simulated maintenance operations. Return 0 on success.
   These never invoke dpkg/apt; they only update the SQLite db. */
int sim_install(const char *db_path, const char *list_dir, const char *pkg, int yes);
int sim_remove(const char *db_path, const char *pkg, int yes);
int sim_upgrade(const char *db_path, const char *list_dir, int yes);

#endif
