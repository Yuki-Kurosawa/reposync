#ifndef REPOSYNC_GUI_UI_H
#define REPOSYNC_GUI_UI_H

#include "gui.h"

/* Run the GUI main loop. Returns the process exit code (0 = ok); on
   Windows the --gui-selftest mode exits 1 when the test run failed. */
int ui_run(gui_app *app);

/* Shared entry point: load config, run UI, teardown the backend.
   Called by main() on POSIX and by WinMain on Windows.
   Recognizes --gui-selftest (headless self-test mode). */
int gui_main(int argc, char **argv);

#endif
