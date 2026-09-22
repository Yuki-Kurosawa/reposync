#ifndef REPOSYNC_INDEX_H
#define REPOSYNC_INDEX_H

#include <stddef.h>
#include "deb822.h"
#include "list.h"

typedef struct {
    char *uri;
    char *suite;
    str_list comps;
    str_list arches;
} src_t;

/* Read the .meta file written next to a cached index (Kind/URI/Suite/
   Component/Architecture). uri/suite/comp are malloc'd when present (any may
   be NULL on input; *x stays NULL when the field is absent). Returns 0 when
   the file was read and the URI field is present. */
int idx_read_meta(const char *meta_path, char **uri, char **suite, char **comp);

/* Load all *.sources under dir. Returns 0 on success. */
int src_load_dir(const char *dir, src_t **out, size_t *n);
void src_free_all(src_t *s, size_t n);

/* Fetch Release, Packages indexes (and Sources when sync_sources > 0) for
   all sources into list_dir. Returns 0. */
int idx_update(const char *list_dir, src_t *srcs, size_t n, int sync_sources);

/* Locate pkg in cached indexes and download its .deb into outdir.
   saved_path is malloc'd on success. Returns 0 on success. */
int idx_download(const char *list_dir, const char *pkg, const char *outdir, char **saved_path);

/* Locate the source package srcname in cached Sources indexes and download
   its whole file family (.dsc + orig/debian tarballs, per the Files: field)
   into outdir. *saved_dsc is malloc'd with the .dsc path on success.
   Returns 0 on success. */
int idx_download_source(const char *list_dir, const char *srcname,
                        const char *outdir, char **saved_dsc);

/* Locate the best (highest-version, arch-compatible) record for pkg in cached
   indexes and deep-copy it into out (caller frees with d822_rec_free).
   Returns 0 on success. */
int idx_best(const char *list_dir, const char *pkg, d822_rec *out);

#endif
