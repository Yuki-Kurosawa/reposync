/* qt.c - link against gui app.o to exercise gui_db_query() without SDL.
   Usage: qt [pkg] [suite] [comp]
   Prints tab-separated rows + count. Exits 0 on success. */
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
    const char *pkg = argc > 1 && argv[1][0] ? argv[1] : NULL;
    const char *suite = argc > 2 && argv[2][0] ? argv[2] : NULL;
    const char *comp = argc > 3 && argv[3][0] ? argv[3] : NULL;
    gui_app app;
    memset(&app, 0, sizeof app);
    int count = 0;
    if (gui_db_query(&app, pkg, suite, comp, out_cb, NULL, &count) < 0) {
        fprintf(stderr, "query failed: %s\n", app.errmsg);
        return 1;
    }
    fprintf(stderr, "count=%d\n", count);
    return 0;
}
