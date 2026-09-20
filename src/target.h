#ifndef REPOSYNC_TARGET_H
#define REPOSYNC_TARGET_H

#include <stddef.h>
#include "deb822.h"
#include "sim.h"
#include "strbuf.h"

/* The remote repo reposync manages. Two DEB822 files:
   - target.sources  (/etc/$TOOLNAME/target.sources): a *pure apt source file*
     (Types/URIs/Suites/Components/Architectures/Trusted only; unsigned local
     repos need "Trusted: yes"). Dropping it into
     /etc/apt/sources.list.d/ lets apt consume the same repo directly.
   - reposync.conf   (/etc/$TOOLNAME/reposync.conf): the global config; the
     repo control command set (Path/Import-Cmd/Remove-Cmd, per Repo-Type,
     selected by Active-Repo) lives here. The old target.conf is retired.

   Real mutations (import/remove) run through Import-Cmd / Remove-Cmd (the
   normal repo toolset, e.g. reprepro, which re-indexes and validates);
   reposync only computes the dependency-closure order and fetches the .debs. */
typedef struct {
    char *path;       /* local repo root if known (from Path: or file: URI) */
    char *uri;        /* repo base URL as apt would use it */
    char *suite;
    char *comp;
    char *arch;
    char *import_cmd; /* command template; the .deb path is appended */
    char *remove_cmd; /* command template; the package name is appended */
} target_t;

/* Parse target.sources (apt view) plus reposync.conf (global config; the
   active repo-type's command set). conf_path may be NULL; a missing file is
   not an error. Returns 0 on success. */
int target_load(const char *sources_path, const char *conf_path, target_t *t);
void target_free(target_t *t);

/* Fetch and parse the target repo's Packages index into a pkgset of
   "installed" records (all treated as installed). Empty repo => empty set.
   doc_out (optional) receives the parsed records. Returns 0 on success. */
int target_state(const target_t *t, pkgset_t *ps, d822_doc *doc_out);

#endif
