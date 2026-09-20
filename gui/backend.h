#ifndef REPOSYNC_GUI_BACKEND_H
#define REPOSYNC_GUI_BACKEND_H

#include <stddef.h>

/* Streaming sink: called with chunks of combined stdout+stderr. */
typedef void (*backend_out_fn)(void *ctx, const char *data, size_t len);

typedef struct backend {
    const char *name;      /* "local" or "ssh" */
    void *priv;
    char errmsg[512];      /* last transport error, for the UI to display */

    /* Establish the transport (lazy; the local backend always succeeds).
       Returns 0 on success. */
    int (*connect)(struct backend *b);

    /* Run one shell command line, streaming combined output to cb.
       *exitcode receives the command exit status (>= 0). Returns 0 when the
       command was executed at all, -1 on transport failure (errmsg set). */
    int (*run)(struct backend *b, const char *cmdline,
               backend_out_fn cb, void *cbctx, int *exitcode);

    /* Optional file transport (SFTP): copy remote file to a local path.
       May be NULL when the transport cannot copy files (local backend:
       the db already is local). Returns 0 on success, -1 on failure. */
    int (*get_file)(struct backend *b, const char *remote,
                    const char *local);

    /* Tear down the transport and free the backend. */
    void (*destroy)(struct backend *b);
} backend_t;

/* Local transport: fork/exec /bin/sh -c on this (Linux) host.
   reposync CLI is Linux-only, so this backend is only built on POSIX. */
backend_t *backend_local_new(void);

/* SSH transport via libssh2 (BSD license) to a Linux host running the
   reposync CLI. password may be empty to rely on the user's default
   OpenSSH private keys (~/.ssh/id_ed25519, id_rsa, ...). */
backend_t *backend_ssh_new(const char *host, int port,
                           const char *user, const char *password);

#endif
