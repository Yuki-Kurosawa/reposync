#ifndef REPOSYNC_CONFIG_H
#define REPOSYNC_CONFIG_H

#include <stddef.h>

/* reposync.conf — the global configuration file (DEB822 format).
   Located at /etc/$TOOLNAME/reposync.conf (Linux) or %EXEDIR%\reposync.conf
   (Windows).

   The file carries the repo control command sets. There is *more than one*
   command set: each DEB822 record describes one repo type
   (Repo-Type: <name> plus Path:/Import-Cmd:/Remove-Cmd:), and the active
   type is selected with Active-Repo: <name> (usually the first record).

   Example:
       Active-Repo: reprepro
       Sync-Sources: yes

       Repo-Type: reprepro
       Path: /srv/target
       Import-Cmd: reprepro -b /srv/target includedeb trixie
       Source-Import-Cmd: reprepro -b /srv/target includedsc trixie
       Remove-Cmd: reprepro -b /srv/target remove trixie

       Repo-Type: plain
       Path: /srv/plain
       Import-Cmd: cp
       Source-Import-Cmd: cp
       Remove-Cmd: rm

   Suite mapping (需求.md 套件映射, cli/gui 共用): binary/source packages
   are routed to the target (suite, component) by target_map_suite —
   same-name first; "<base>-*" variants fall back to the base suite only
   when the target declares no such variant suite at all; cross-component
   and base->variant (reverse) mappings are refused. With more than one
   target suite (target.sources Suites:), put %suite% (and %comp% if the
   backend needs it) in Import-Cmd / Source-Import-Cmd, e.g.
   "reprepro -b /srv/target includedeb %suite%". Templates without the
   placeholder keep the legacy single-suite behavior.

   Sync-Sources is MANDATORY (yes|no): reposync does not infer whether the
   backend manages a Sources index — only the repo administrator knows.
   "no" skips the whole source pipeline; "yes" requires Source-Import-Cmd
   to be set on the active repo type.

   The old target.conf (Path/Import-Cmd/Remove-Cmd) is retired: its fields
   live in reposync.conf now. */

typedef struct {
    char *type;       /* Repo-Type */
    char *path;       /* Path */
    char *import_cmd; /* Import-Cmd (binary .debs) */
    char *source_import_cmd; /* Source-Import-Cmd (.dsc + source files) */
    char *remove_cmd; /* Remove-Cmd */
} rcmd_t;

typedef struct {
    char *active;     /* Active-Repo value, or NULL */
    int   sync_sources; /* Sync-Sources: -1 unset, 0 no, 1 yes (mandatory) */
    rcmd_t *cmds;
    size_t n, cap;
} rconf_t;

void rconf_init(rconf_t *rc);
void rconf_free(rconf_t *rc);

/* Parse path into rc (rc must be rconf_init'ed). A missing or unreadable
   file is not an error: rc stays empty. Returns 0 on success. */
int rconf_load(const char *path, rconf_t *rc);

/* The command set selected by Active-Repo, or the first record that carries
   a command if Active-Repo is absent/unmatched. NULL if none. */
const rcmd_t *rconf_active(const rconf_t *rc);

#endif
