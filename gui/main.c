#include "gui.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared entry: load config, run the UI, tear down the backend. */
int gui_main(int argc, char **argv)
{
    gui_app app;
    memset(&app, 0, sizeof app);
    if (gui_config_load(&app) < 0) {
#ifndef _WIN32
        fprintf(stderr, "reposync-gui: %s\n",
                app.errmsg[0] ? app.errmsg : "cannot load config");
#endif
        gui_cfg_default(&app.cfg);
    }
    /* hidden headless self-test: build the UI, run one test job through the
       configured backend, dump the log, then quit. */
    int i;
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--gui-selftest") == 0)
            app.selftest = 1;
    int rc = ui_run(&app);
    if (app.be)
        app.be->destroy(app.be);
    return rc;
}

/* POSIX entry point. On Windows, ui_win.c provides WinMain instead. */
#ifndef _WIN32
int main(int argc, char **argv)
{
    return gui_main(argc, argv);
}
#endif
