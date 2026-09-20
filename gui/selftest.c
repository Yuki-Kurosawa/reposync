/* reposync-gui - headless backend smoke test (Linux build, no GUI).
   Verifies the local fork/exec backend and the libssh backend end to end:
     ./selftest                      -> run "reposync help" locally
     ./selftest reposync update      -> run an arbitrary command locally
     ./selftest --ssh host port user pass [command...]
                                     -> run the command on host via libssh
   Exit 0 only when the transport worked AND the command exited 0. */

#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void out_cb(void *ctx, const char *d, size_t n)
{
    (void)ctx;
    fwrite(d, 1, n, stdout);
}

int main(int argc, char **argv)
{
    gui_app app;
    memset(&app, 0, sizeof app);
    gui_config_load(&app);

    int i = 1;
    if (i < argc && strcmp(argv[i], "--ssh") == 0 && i + 4 < argc) {
        app.cfg.use_ssh = 1;
        strncpy(app.cfg.ssh_host, argv[i + 1], sizeof app.cfg.ssh_host - 1);
        app.cfg.ssh_port = atoi(argv[i + 2]);
        strncpy(app.cfg.ssh_user, argv[i + 3], sizeof app.cfg.ssh_user - 1);
        strncpy(app.cfg.ssh_pass, argv[i + 4], sizeof app.cfg.ssh_pass - 1);
        i += 5;
    }

    char *cmd;
    if (i < argc) {
        size_t n = 1;
        int k;
        for (k = i; k < argc; k++)
            n += strlen(argv[k]) + 1;
        cmd = malloc(n);
        cmd[0] = '\0';
        for (k = i; k < argc; k++) {
            strcat(cmd, argv[k]);
            if (k + 1 < argc)
                strcat(cmd, " ");
        }
    } else {
        cmd = strdup("reposync help");
    }

    int exitcode = 0;
    printf("=== backend=%s cmd=%s ===\n",
           app.cfg.use_ssh ? "ssh" : "local", cmd);
    int rc = gui_run(&app, cmd, out_cb, NULL, &exitcode);
    printf("\n=== transport_rc=%d exitcode=%d", rc, exitcode);
    if (rc)
        printf(" err=%s", app.errmsg);
    printf(" ===\n");

    free(cmd);
    if (app.be)
        app.be->destroy(app.be);
    return rc ? 1 : (exitcode == 0 ? 0 : 1);
}
