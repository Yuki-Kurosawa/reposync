#ifndef REPOSYNC_DB_H
#define REPOSYNC_DB_H

#include "sim.h"

/* Import installed package state from a dpkg status file into a SQLite
   database (create/recreate schema). Returns 0 on success. */
int db_import(const char *db_path, const char *status_path);

/* Load all rows of packages into ps (replacing its contents). */
int db_load_all(const char *db_path, pkgset_t *ps);

/* Replace whole packages table with ps contents (transaction), then refresh
   meta counts. Returns 0 on success. */
int db_write_all(const char *db_path, const pkgset_t *ps);

/* Rebuild the candidates table (the upstream package index) from the cached
   *_Packages files + .meta under list_dir. Drop-and-rebuild inside one
   transaction; meta gains candidate_count. Returns 0 on success. */
int db_candidates_rebuild(const char *db_path, const char *list_dir);

/* Query candidates (local database; query operations never go remote).
   pkg/suite/comp may be NULL for no filter. Prints tab-separated lines:
   name<TAB>version<TAB>arch<TAB>suite<TAB>component<TAB>description
   *count receives the number of lines printed. Returns 0 on success. */
int db_candidates_query(const char *db_path, const char *pkg,
                        const char *suite, const char *comp, int *count);

#endif
