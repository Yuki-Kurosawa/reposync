#ifndef REPOSYNC_OPS_H
#define REPOSYNC_OPS_H

#include "target.h"

/* Real operations on the managed target repository.
   list_dir holds the upstream cached indexes (from update).
   argv[0..argc) are plain package names (no flags). */

int ops_install(target_t *t, const char *list_dir, int argc, char **argv, int yes);
int ops_upgrade(target_t *t, const char *list_dir, int argc, char **argv, int yes);
int ops_remove(target_t *t, const char *list_dir, int argc, char **argv, int yes);
int ops_autoremove(target_t *t, const char *list_dir, int argc, char **argv, int yes);

#endif
