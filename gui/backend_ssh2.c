/* reposync-gui - SSH backend via libssh2 (BSD 3-clause, no GPL/LGPL).
   Replaces the former libssh (LGPL) backend. Runs one shell command line on
   a remote Linux host over SSH and streams combined stdout+stderr.

   Auth order: password when configured, otherwise try the user's default
   OpenSSH private keys (~/.ssh/id_ed25519, ~/.ssh/id_rsa) with no
   passphrase. No ssh client binary is required. */

#include "backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
typedef SOCKET ssh_sock_t;
#define ssh_close(s) closesocket(s)
#define SSH_SOCK_INVALID INVALID_SOCKET
#define SSH_ERRNO WSAGetLastError()
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
typedef int ssh_sock_t;
#define ssh_close(s) close(s)
#define SSH_SOCK_INVALID (-1)
#define SSH_ERRNO errno
#endif

#define SSH_TIMEOUT_MS 20000

typedef struct {
    char host[256];
    int  port;
    char user[64];
    char pass[256];
    ssh_sock_t sock;
    LIBSSH2_SESSION *ses;
    LIBSSH2_CHANNEL *ch;
    int connected;
} ssh2_priv;

static void be_ssh2_seterr(backend_t *b, LIBSSH2_SESSION *s, const char *what)
{
    if (s) {
        char *emsg = NULL;
        int len = 0;
        libssh2_session_last_error(s, &emsg, &len, 0);
        if (emsg && len > 0)
            snprintf(b->errmsg, sizeof b->errmsg, "%s: %.*s",
                     what, len, emsg);
        else
            snprintf(b->errmsg, sizeof b->errmsg, "%s", what);
    } else {
        snprintf(b->errmsg, sizeof b->errmsg, "%s", what);
    }
}

/* ---- transport: socket + handshake + auth ---- */

static ssh_sock_t ssh_connect_sock(const char *host, int port, char *err,
                                   size_t errsz)
{
    struct addrinfo hints, *res = NULL, *it;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
#ifdef _WIN32
    hints.ai_family = AF_INET; /* keep it simple on Windows */
#endif
    int rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        snprintf(err, errsz, "cannot resolve %.255s:%d: %s",
                 host, port, gai_strerror(rc));
        return SSH_SOCK_INVALID;
    }
    ssh_sock_t s = SSH_SOCK_INVALID;
    for (it = res; it; it = it->ai_next) {
        s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s == SSH_SOCK_INVALID)
            continue;
        if (connect(s, it->ai_addr, (int)it->ai_addrlen) == 0)
            break;
        ssh_close(s);
        s = SSH_SOCK_INVALID;
    }
    freeaddrinfo(res);
    if (s == SSH_SOCK_INVALID)
        snprintf(err, errsz, "cannot connect to %s:%d", host, port);
    return s;
}

/* Try the user's default OpenSSH keys (no passphrase).
   Path table (需求.md): Linux ~/.ssh/id_* , Windows %USERDIR%\.ssh\id_*
   (%USERDIR% = C:\Users\%USERNAME% = USERPROFILE). */
static int try_publickey(LIBSSH2_SESSION *ses, const char *user)
{
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home)
        home = getenv("USERPROFILE");
#endif
#ifndef _WIN32
    const char *keys[] = { "id_ed25519", "id_rsa", "id_ecdsa", "id_dsa" };
#else
    const char *keys[] = { "id_ed25519", "id_rsa", "id_ecdsa" };
#endif
    size_t i;
    for (i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        char path[1024];
        if (home && *home)
            snprintf(path, sizeof path, "%s/.ssh/%s", home, keys[i]);
        else
            snprintf(path, sizeof path, ".ssh/%s", keys[i]);
        if (libssh2_userauth_publickey_fromfile(ses, user, NULL, path,
                                                NULL) == 0)
            return 1;
    }
    return 0;
}

static int ssh2_connect(backend_t *b)
{
    ssh2_priv *p = b->priv;
    if (p->connected)
        return 0;

    char err[512];
    p->sock = ssh_connect_sock(p->host, p->port, err, sizeof err);
    if (p->sock == SSH_SOCK_INVALID) {
        snprintf(b->errmsg, sizeof b->errmsg, "ssh connect: %.490s", err);
        return -1;
    }

    p->ses = libssh2_session_init();
    if (!p->ses) {
        snprintf(b->errmsg, sizeof b->errmsg, "ssh connect: session init failed");
        ssh_close(p->sock);
        p->sock = SSH_SOCK_INVALID;
        return -1;
    }
    libssh2_session_set_timeout(p->ses, SSH_TIMEOUT_MS);
    libssh2_session_set_blocking(p->ses, 1);

    int rc = libssh2_session_handshake(p->ses, (int)p->sock);
    if (rc != 0) {
        be_ssh2_seterr(b, p->ses, "ssh connect");
        libssh2_session_free(p->ses);
        p->ses = NULL;
        ssh_close(p->sock);
        p->sock = SSH_SOCK_INVALID;
        return -1;
    }

    const char *user = p->user[0] ? p->user : "root";
    int ok = 0;
    if (p->pass[0]) {
        if (libssh2_userauth_password(p->ses, user, p->pass) == 0)
            ok = 1;
        else
            snprintf(b->errmsg, sizeof b->errmsg,
                     "authentication failed (password)");
    } else {
        ok = try_publickey(p->ses, user);
        if (!ok)
            snprintf(b->errmsg, sizeof b->errmsg,
                     "authentication failed (no password configured and no "
                     "usable SSH key found)");
    }
    if (!ok) {
        libssh2_session_disconnect(p->ses, "auth failed");
        libssh2_session_free(p->ses);
        p->ses = NULL;
        ssh_close(p->sock);
        p->sock = SSH_SOCK_INVALID;
        return -1;
    }

    p->connected = 1;
    return 0;
}

/* ---- run one command, streaming combined output ---- */

static int ssh2_run(backend_t *b, const char *cmdline,
                    backend_out_fn cb, void *cbctx, int *exitcode)
{
    ssh2_priv *p = b->priv;
    *exitcode = -1;
    if (ssh2_connect(b) != 0)
        return -1;

    p->ch = libssh2_channel_open_session(p->ses);
    if (!p->ch) {
        be_ssh2_seterr(b, p->ses, "ssh exec");
        return -1;
    }
    if (libssh2_channel_exec(p->ch, cmdline) != 0) {
        be_ssh2_seterr(b, p->ses, "ssh exec");
        libssh2_channel_free(p->ch);
        p->ch = NULL;
        return -1;
    }

    char buf[8192];
    int ran = 0;
    for (;;) {
        ssize_t n = libssh2_channel_read(p->ch, buf, sizeof buf);
        if (n < 0) {
            if (n == LIBSSH2_ERROR_EAGAIN)
                continue;
            break;
        }
        if (n == 0) {
            if (libssh2_channel_eof(p->ch)) {
                /* drain any stderr still buffered */
                for (;;) {
                    n = libssh2_channel_read_stderr(p->ch, buf, sizeof buf);
                    if (n <= 0)
                        break;
                    cb(cbctx, buf, (size_t)n);
                }
                break;
            }
            continue;
        }
        ran = 1;
        cb(cbctx, buf, (size_t)n);
    }
    libssh2_channel_wait_eof(p->ch);
    libssh2_channel_wait_closed(p->ch);
    *exitcode = libssh2_channel_get_exit_status(p->ch);
    if (*exitcode < 0)
        *exitcode = ran ? 0 : -1;
    libssh2_channel_free(p->ch);
    p->ch = NULL;
    return 0;
}

/* ---- SFTP file transfer (pull remote db to local) ---- */

static int ssh2_get_file(backend_t *b, const char *remote, const char *local)
{
    ssh2_priv *p = b->priv;
    if (ssh2_connect(b) != 0)
        return -1;

    LIBSSH2_SFTP *sftp = libssh2_sftp_init(p->ses);
    if (!sftp) {
        be_ssh2_seterr(b, p->ses, "sftp init");
        return -1;
    }
    LIBSSH2_SFTP_HANDLE *fh =
        libssh2_sftp_open(sftp, remote, LIBSSH2_FXF_READ, 0);
    if (!fh) {
        char *emsg = NULL;
        int len = 0;
        libssh2_session_last_error(p->ses, &emsg, &len, 0);
        snprintf(b->errmsg, sizeof b->errmsg,
                 "sftp open %s: %.*s", remote,
                 (emsg && len > 0) ? len : 20, emsg ? emsg : "failed");
        libssh2_sftp_shutdown(sftp);
        return -1;
    }
    FILE *f = fopen(local, "wb");
    if (!f) {
        snprintf(b->errmsg, sizeof b->errmsg, "cannot write %s", local);
        libssh2_sftp_close_handle(fh);
        libssh2_sftp_shutdown(sftp);
        return -1;
    }
    char buf[16384];
    int rc = -1;
    for (;;) {
        ssize_t n = libssh2_sftp_read(fh, buf, sizeof buf);
        if (n < 0) {
            if (n == LIBSSH2_ERROR_EAGAIN)
                continue;
            be_ssh2_seterr(b, p->ses, "sftp read");
            break;
        }
        if (n == 0) {
            rc = 0;
            break;
        }
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            snprintf(b->errmsg, sizeof b->errmsg, "cannot write %s", local);
            break;
        }
    }
    fclose(f);
    libssh2_sftp_close_handle(fh);
    libssh2_sftp_shutdown(sftp);
    if (rc != 0) {
        remove(local);
        return -1;
    }
    return 0;
}

/* ---- teardown ---- */

static void ssh2_destroy(backend_t *b)
{
    ssh2_priv *p = b->priv;
    if (p) {
        if (p->ch) {
            libssh2_channel_free(p->ch);
            p->ch = NULL;
        }
        if (p->ses) {
            libssh2_session_disconnect(p->ses, "bye");
            libssh2_session_free(p->ses);
            p->ses = NULL;
        }
        if (p->sock != SSH_SOCK_INVALID) {
            ssh_close(p->sock);
            p->sock = SSH_SOCK_INVALID;
        }
        free(p);
    }
    free(b);
}

backend_t *backend_ssh_new(const char *host, int port,
                           const char *user, const char *password)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return NULL;
#endif
    static int inited = 0;
    if (!inited) {
        if (libssh2_init(0) != 0)
            return NULL;
        inited = 1;
    }

    backend_t *b = calloc(1, sizeof *b);
    ssh2_priv *p = calloc(1, sizeof *p);
    if (!b || !p) {
        free(b);
        free(p);
        return NULL;
    }
    b->name = "ssh2";
    b->priv = p;
    b->connect = ssh2_connect;
    b->run = ssh2_run;
    b->get_file = ssh2_get_file;
    b->destroy = ssh2_destroy;
    b->errmsg[0] = '\0';

    snprintf(p->host, sizeof p->host, "%s", host ? host : "");
    p->port = port > 0 ? port : 22;
    snprintf(p->user, sizeof p->user, "%s", user ? user : "");
    snprintf(p->pass, sizeof p->pass, "%s", password ? password : "");
    p->sock = SSH_SOCK_INVALID;
    return b;
}
