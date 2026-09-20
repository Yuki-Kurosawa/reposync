/* Local transport for reposync-gui (Linux only).
   Fork/exec /bin/sh -c and stream combined stdout+stderr.
   On Windows this backend does not exist (no local CLI); backend_local_new
   fails cleanly so a misconfigured use_ssh=0 still reports an error instead
   of crashing. */

#include "backend.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

backend_t *backend_local_new(void)
{
    (void)errno;
    return NULL;
}

#else

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int local_connect(backend_t *b)
{
    (void)b;
    return 0;
}

static void local_destroy(backend_t *b)
{
    free(b->priv);
    free(b);
}

static int local_run(backend_t *b, const char *cmdline,
                     backend_out_fn cb, void *cbctx, int *exitcode)
{
    int fds[2];
    if (pipe(fds) < 0) {
        snprintf(b->errmsg, sizeof b->errmsg, "pipe: %s", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(b->errmsg, sizeof b->errmsg, "fork: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    char buf[8192];
    for (;;) {
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n > 0) {
            if (cb)
                cb(cbctx, buf, (size_t)n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (exitcode)
        *exitcode = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
    return 0;
}

backend_t *backend_local_new(void)
{
    backend_t *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->name = "local";
    b->connect = local_connect;
    b->run = local_run;
    b->destroy = local_destroy;
    return b;
}

#endif /* _WIN32 */
