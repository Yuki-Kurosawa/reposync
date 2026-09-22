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
    char *suite;      /* base/default suite = first declared in Suites: */
    char *comp;       /* base/default component = first declared in Components: */
    char *arch;
    str_list suites;  /* ALL suites declared in target.sources (mapping set) */
    str_list comps;   /* ALL components declared in target.sources */
    char *import_cmd; /* command template; %suite%/%comp% are substituted, the
                         .deb path is appended. Sync suite mapping requires
                         %suite% when more than one target suite is declared. */
    char *source_import_cmd; /* Source-Import-Cmd; the .dsc path is appended */
    char *remove_cmd; /* command template; the package name is appended */
    int   sync_sources; /* Sync-Sources: -1 unset, 0 no, 1 yes */
} target_t;

/* Parse target.sources (apt view) plus reposync.conf (global config; the
   active repo-type's command set). conf_path may be NULL; a missing file is
   not an error. Returns 0 on success. */
int target_load(const char *sources_path, const char *conf_path, target_t *t);
void target_free(target_t *t);

/* Suite/component mapping for sync (需求.md 套件映射, cli + gui share it):
   1. same-name first: source (S,C) -> target (S,C) when target declares
      suite S and component C;
   2. variant fallback: S matches "<base>-*" (base = first declared suite),
      the target declares NO suite S at all, and the target declares the base
      suite and component C -> (base, C);
   3. anything else (cross-component, base -> variant, unrelated suites) is
      not allowed: returns 0 and the caller skips the package with a warning.
   Returns 1 and sets malloc'd *out_suite / *out_comp on success. */
int target_map_suite(const target_t *t, const char *src_suite,
                     const char *src_comp, char **out_suite, char **out_comp);

/* Fetch and parse the target repo's Packages index into a pkgset of
   "installed" records (all treated as installed). Empty repo => empty set.
   doc_out (optional) receives the parsed records. Returns 0 on success. */
int target_state(const target_t *t, pkgset_t *ps, d822_doc *doc_out);

/* Fetch and parse the target repo's Sources index into ss (only the source
   package identity fields needed to skip already-synced sources). An empty
   or absent Sources index yields an empty set (not an error). Returns 0. */
int target_sources_state(const target_t *t, srcset_t *ss);

#endif
