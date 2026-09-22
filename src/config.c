#include "config.h"
#include "deb822.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rconf_init(rconf_t *rc)
{
    memset(rc, 0, sizeof *rc);
    rc->sync_sources = -1;
}

void rconf_free(rconf_t *rc)
{
    free(rc->active);
    size_t i;
    for (i = 0; i < rc->n; i++) {
        free(rc->cmds[i].type);
        free(rc->cmds[i].path);
        free(rc->cmds[i].import_cmd);
        free(rc->cmds[i].source_import_cmd);
        free(rc->cmds[i].remove_cmd);
    }
    free(rc->cmds);
    rconf_init(rc);
}

static char *dup0(const char *s)
{
    return s ? xstrdup(s) : NULL;
}

int rconf_load(const char *path, rconf_t *rc)
{
    sb_t content;
    sb_init(&content);
    if (read_file(path, &content) < 0) {
        sb_free(&content);
        return 0; /* missing file: empty config, not an error */
    }
    d822_doc doc;
    if (d822_parse(content.s, content.len, &doc) < 0) {
        fprintf(stderr, "reposync: parse error in %s\n", path);
        d822_doc_free(&doc);
        sb_free(&content);
        return -1;
    }
    size_t i;
    for (i = 0; i < doc.n; i++) {
        const d822_rec *r = &doc.recs[i];
        const char *act = d822_get(r, "active-repo");
        const char *rtype = d822_get(r, "repo-type");
        if (act) {
            free(rc->active);
            rc->active = dup0(act);
            const char *ss = d822_get(r, "sync-sources");
            if (ss) {
                if (strcasecmp(ss, "yes") == 0)
                    rc->sync_sources = 1;
                else if (strcasecmp(ss, "no") == 0)
                    rc->sync_sources = 0;
                else
                    fprintf(stderr, "reposync: warning: Sync-Sources must be yes or no (got '%s')\n", ss);
            }
            continue;
        }
        if (!rtype)
            continue; /* record without Repo-Type and without Active-Repo */
        if (rc->n == rc->cap) {
            rc->cap = rc->cap ? rc->cap * 2 : 4;
            rc->cmds = xrealloc(rc->cmds, rc->cap * sizeof(rcmd_t));
        }
        rcmd_t *c = &rc->cmds[rc->n++];
        memset(c, 0, sizeof *c);
        c->type = dup0(rtype);
        c->path = dup0(d822_get(r, "path"));
        c->import_cmd = dup0(d822_get(r, "import-cmd"));
        c->source_import_cmd = dup0(d822_get(r, "source-import-cmd"));
        c->remove_cmd = dup0(d822_get(r, "remove-cmd"));
    }
    d822_doc_free(&doc);
    sb_free(&content);
    return 0;
}

const rcmd_t *rconf_active(const rconf_t *rc)
{
    if (!rc || rc->n == 0)
        return NULL;
    if (rc->active) {
        size_t i;
        for (i = 0; i < rc->n; i++)
            if (rc->cmds[i].type && strcmp(rc->cmds[i].type, rc->active) == 0)
                return &rc->cmds[i];
    }
    /* fall back: first record that actually carries a command */
    size_t i;
    for (i = 0; i < rc->n; i++)
        if (rc->cmds[i].import_cmd || rc->cmds[i].remove_cmd)
            return &rc->cmds[i];
    return &rc->cmds[0];
}
