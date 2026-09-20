#ifndef REPOSYNC_GUI_H
#define REPOSYNC_GUI_H

#include <stddef.h>
#include "backend.h"

/* The operations reposync-gui can drive on the reposync CLI. OP_LIST is
   special: it queries the *local* database directly (never via CLI/SSH). */
typedef enum {
    OP_UPDATE,
    OP_DOWNLOAD,
    OP_INSTALL,
    OP_REMOVE,
    OP_UPGRADE,
    OP_AUTOREMOVE,
    OP_SIM_INSTALL,
    OP_SIM_REMOVE,
    OP_SIM_UPGRADE,
    OP_LIST,                 /* query the local db: reposync list [pkg] */
    OP_COUNT
} op_kind;

/* The global reposync.conf (shared with the CLI on Linux; on Windows
   %EXEDIR%\reposync.conf): repo control command sets, several Repo-Type
   records, one selected by Active-Repo. The GUI reads it to show and switch
   the active repo type; the CLI does the actual command execution. */
typedef struct {
    char active[64];
    char types[16][64];
    size_t ntypes;
    char path[512];
} gui_repo;

typedef struct {
    /* Local (Linux) mode */
    char reposync_path[512];   /* binary name or path; default "reposync" */
    /* SSH mode */
    int  use_ssh;              /* always 1 on Windows */
    char ssh_host[256];
    int  ssh_port;
    char ssh_user[64];
    char ssh_pass[256];
    char remote_reposync_path[512]; /* reposync command on the remote host */
    char remote_db_path[512];       /* packages.db on the remote host; pulled
                                       locally after a remote update so that
                                       queries run offline */
    /* Misc */
    char download_dir[512];    /* target dir for the download op */
} gui_cfg;

typedef struct {
    gui_cfg cfg;
    gui_repo repo;             /* reposync.conf view (repo types + active) */
    backend_t *be;
    int connected;
    int selftest;              /* 1: headless GUI self-test (--gui-selftest) */
    char errmsg[512];          /* last user-facing error */
} gui_app;

/* Defaults for a fresh configuration. */
void gui_cfg_default(gui_cfg *c);

/* Path of the GUI config file (malloc'd; caller frees):
   Linux  $HOME/.config/reposync-gui.conf
   Win    %APPDATA%\reposync-gui.conf */
char *gui_config_path(void);

/* Path of the global reposync.conf (malloc'd; caller frees):
   Linux  $REPOSYNC_CONF or /etc/reposync/reposync.conf
   Win    %EXEDIR%\reposync.conf                                   */
char *gui_repo_config_path(void);

/* Parse reposync.conf into app->repo (missing file: defaults, no error). */
void gui_repo_load(gui_app *app);

/* Rewrite the Active-Repo: line of reposync.conf to type (keeping the rest
   of the file untouched). Returns 0 on success, -1 on failure (errmsg set). */
int gui_repo_set_active(gui_app *app, const char *type);

/* Load the config file over the defaults. Missing file is not an error.
   Returns 0 on success. */
int gui_config_load(gui_app *app);

/* Persist the current configuration. Returns 0 on success. */
int gui_config_save(const gui_app *app);

const char *gui_op_label(op_kind op);
int gui_op_needs_pkg(op_kind op);
int gui_op_needs_dir(op_kind op);

/* Build the shell command line for op (malloc'd). pkg/dir may be NULL or
   empty when the op does not need them. Inputs are charset-validated to keep
   the command line injection-safe. Returns NULL and sets app->errmsg on
   invalid input or an oversized command. */
char *gui_build_cmd(gui_app *app, op_kind op, const char *pkg,
                    const char *dir);

/* Make sure the backend exists and is connected (SSH handshake happens on
   first use). Returns 0 on success; errmsg set on failure. */
int gui_ensure_backend(gui_app *app);

/* Run one command through the backend, streaming combined output to cb.
   Returns 0 if executed; *exitcode receives the exit status. */
int gui_run(gui_app *app, const char *cmdline, backend_out_fn cb, void *cbctx,
            int *exitcode);

/* Path of the local packages database that queries read directly:
   Linux  $REPOSYNC_DB or /var/lib/reposync/packages.db
   Win    %EXEDIR%\packages.db
   (malloc'd; caller frees). */
char *gui_db_path(void);

/* Query the local db (index_packages table). pkg/suite/comp may be NULL.
   Every row is appended through cb as one tab-separated line:
   name<TAB>version<TAB>arch<TAB>suite<TAB>component<TAB>description
   *count receives the number of rows. This never touches the network.
   Returns 0 on success, -1 on failure (app->errmsg set). */
int gui_db_query(gui_app *app, const char *pkg, const char *suite,
                 const char *comp, backend_out_fn cb, void *cbctx,
                 int *count);

/* After a remote (SSH) update, pull the remote packages.db to the local
   path (gui_db_path) so subsequent queries are fully local. Uses SFTP.
   No-op when the backend has no file transport. Returns 0 on success. */
int gui_pull_remote_db(gui_app *app);

#endif
