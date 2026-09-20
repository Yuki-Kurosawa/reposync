#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define PATH_SEP '\\'
#define HOME_ENV "APPDATA"
#define CONF_DIR ""
#define CONF_FILE "\\reposync-gui.conf"
#define mkdir_one(p) _mkdir(p)
#else
#include <sys/stat.h>
#define PATH_SEP '/'
#define HOME_ENV "HOME"
#define CONF_DIR "/.config"
#define CONF_FILE "/reposync-gui.conf"
#define mkdir_one(p) mkdir((p), 0755)
#endif

/* ---------------- reposync.conf (global config, shared with the CLI) ------- */

/* Path table (需求.md): Linux /etc/reposync/reposync.conf (or $REPOSYNC_CONF),
   Windows %EXEDIR%\reposync.conf. */
char *gui_repo_config_path(void)
{
#ifdef _WIN32
    static char dir[MAX_PATH];
    dir[0] = '\0';
    DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        char *slash = strrchr(dir, '\\');
        if (slash)
            slash[1] = '\0';
    }
    size_t dn = strlen(dir);
    char *p = malloc(dn + 15);
    if (!p)
        return NULL;
    snprintf(p, dn + 15, "%sreposync.conf", dir);
    return p;
#else
    const char *env = getenv("REPOSYNC_CONF");
    if (env && *env)
        return strdup(env);
    const char *legacy = getenv("REPOSYNC_TARGET_CONF"); /* legacy alias */
    if (legacy && *legacy)
        return strdup(legacy);
    return strdup("/etc/reposync/reposync.conf");
#endif
}

/* Minimal DEB822 line parser for reposync.conf: records separated by blank
   lines, "Key: value" fields, keys compared case-insensitively. */
static void repo_parse_line(gui_app *app, const char *key, const char *value)
{
    if (strcasecmp(key, "active-repo") == 0) {
        snprintf(app->repo.active, sizeof app->repo.active, "%s", value);
    } else if (strcasecmp(key, "repo-type") == 0) {
        if (app->repo.ntypes < sizeof app->repo.types / sizeof app->repo.types[0]) {
            snprintf(app->repo.types[app->repo.ntypes],
                     sizeof app->repo.types[0], "%s", value);
            app->repo.ntypes++;
        }
    } else if (strcasecmp(key, "path") == 0) {
        snprintf(app->repo.path, sizeof app->repo.path, "%s", value);
    }
}

void gui_repo_load(gui_app *app)
{
    memset(&app->repo, 0, sizeof app->repo);
    char *path = gui_repo_config_path();
    if (!path)
        return;
    FILE *f = fopen(path, "r");
    free(path);
    if (!f)
        return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        /* fold continuation? reposync.conf command lines are single-line;
           skip folding for simplicity */
        char *s = line;
        size_t l = strlen(s);
        while (l && (s[l - 1] == '\n' || s[l - 1] == '\r'))
            s[--l] = '\0';
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s || *s == '#')
            continue;
        char *colon = strchr(s, ':');
        if (!colon)
            continue;
        *colon = '\0';
        char *key = s;
        while (colon > key && (colon[-1] == ' ' || colon[-1] == '\t'))
            colon--;
        *colon = '\0';
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t')
            val++;
        repo_parse_line(app, key, val);
    }
    fclose(f);
    /* default active = first declared type */
    if (!app->repo.active[0] && app->repo.ntypes > 0)
        snprintf(app->repo.active, sizeof app->repo.active, "%s",
                 app->repo.types[0]);
}

/* Rewrite only the Active-Repo: line; preserve everything else byte-for-byte.
   If no Active-Repo line exists, prepend one. */
int gui_repo_set_active(gui_app *app, const char *type)
{
    char *path = gui_repo_config_path();
    if (!path) {
        snprintf(app->errmsg, sizeof app->errmsg, "out of memory");
        return -1;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(app->errmsg, sizeof app->errmsg,
                 "cannot read %s: create it first", path);
        free(path);
        return -1;
    }
    /* read whole file */
    char *content = NULL;
    size_t len = 0, cap = 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        if (len + l + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            char *nc = realloc(content, cap);
            if (!nc) {
                fclose(f);
                free(content);
                free(path);
                snprintf(app->errmsg, sizeof app->errmsg, "out of memory");
                return -1;
            }
            content = nc;
        }
        memcpy(content + len, line, l + 1);
        len += l;
    }
    fclose(f);
    if (!content) {
        content = malloc(1);
        if (content)
            content[0] = '\0';
        len = 0;
    }

    /* locate/replace the Active-Repo: line (prefix match, case-insensitive) */
    char *out = NULL;
    size_t olen = 0, ocap = 0;
    int replaced = 0;
    char *p = content;
    while (p && *p) {
        char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        int is_ar = 0;
        char *t = p;
        while (t < p + llen && (*t == ' ' || *t == '\t'))
            t++;
        if (t + 12 <= p + llen && strncasecmp(t, "Active-Repo:", 12) == 0)
            is_ar = 1;
        char *seg = p;
        size_t seglen = llen + (eol ? 1 : 0);
        if (is_ar && !replaced) {
            char tmp[96];
            int n = snprintf(tmp, sizeof tmp, "Active-Repo: %s\n", type);
            if (n > 0) {
                seg = tmp;
                seglen = (size_t)n;
            }
            replaced = 1;
        }
        if (olen + seglen + 1 > ocap) {
            ocap = ocap ? ocap * 2 : 8192;
            char *nc = realloc(out, ocap);
            if (!nc) {
                free(out);
                free(content);
                free(path);
                snprintf(app->errmsg, sizeof app->errmsg, "out of memory");
                return -1;
            }
            out = nc;
        }
        memcpy(out + olen, seg, seglen);
        olen += seglen;
        out[olen] = '\0';
        p = eol ? eol + 1 : NULL;
    }
    if (!replaced) {
        char *pre = malloc(olen + 64);
        if (!pre) {
            free(out);
            free(content);
            free(path);
            snprintf(app->errmsg, sizeof app->errmsg, "out of memory");
            return -1;
        }
        int n = snprintf(pre, olen + 64, "Active-Repo: %s\n\n", type);
        memcpy(pre + n, out, olen + 1);
        free(out);
        out = pre;
    }
    free(content);

    FILE *w = fopen(path, "wb");
    if (!w) {
        snprintf(app->errmsg, sizeof app->errmsg,
                 "cannot write reposync.conf (need write access)");
        free(out);
        free(path);
        return -1;
    }
    if (olen && fwrite(out, 1, olen, w) != olen) {
        snprintf(app->errmsg, sizeof app->errmsg,
                 "cannot write reposync.conf (disk full?)");
        fclose(w);
        free(out);
        free(path);
        return -1;
    }
    fclose(w);
    free(out);
    free(path);
    gui_repo_load(app);
    return 0;
}

/* ---------------- defaults ---------------- */

void gui_cfg_default(gui_cfg *c)
{
    memset(c, 0, sizeof *c);
    strncpy(c->reposync_path, "reposync", sizeof c->reposync_path - 1);
    strncpy(c->remote_reposync_path, "reposync",
            sizeof c->remote_reposync_path - 1);
    strncpy(c->remote_db_path, "/var/lib/reposync/packages.db",
            sizeof c->remote_db_path - 1);
    strncpy(c->ssh_host, "127.0.0.1", sizeof c->ssh_host - 1);
    strncpy(c->ssh_user, "root", sizeof c->ssh_user - 1);
    strncpy(c->download_dir, ".", sizeof c->download_dir - 1);
    c->ssh_port = 22;
#ifdef _WIN32
    /* The CLI is Linux-only, so on Windows SSH is the only transport. */
    c->use_ssh = 1;
#endif
}

/* ---------------- config file ---------------- */

char *gui_config_path(void)
{
    const char *base = getenv(HOME_ENV);
    if (!base || !*base)
        base = ".";
    size_t n = strlen(base) + strlen(CONF_DIR) + strlen(CONF_FILE) + 1;
    char *p = malloc(n);
    if (!p)
        return NULL;
    snprintf(p, n, "%s%s%s", base, CONF_DIR, CONF_FILE);
    return p;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                 s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
    return s;
}

static void set_str(char *dst, size_t cap, const char *v)
{
    if (v)
        strncpy(dst, v, cap - 1);
}

int gui_config_load(gui_app *app)
{
    gui_cfg_default(&app->cfg);
    char *path = gui_config_path();
    if (!path)
        return 0;
    FILE *f = fopen(path, "r");
    free(path);
    if (!f)
        return 0;

    char line[512];
    char section[32] = "";
    int first = 1;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        if (first) {
            /* tolerate a UTF-8 BOM that e.g. Notepad adds when saving */
            unsigned char *u = (unsigned char *)s;
            if (u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF)
                s += 3;
            first = 0;
        }
        s = trim(s);
        if (!*s || *s == '#')
            continue;
        if (*s == '[') {
            char *c = strchr(s, ']');
            if (c) {
                *c = '\0';
                strncpy(section, s + 1, sizeof section - 1);
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *k = trim(s);
        char *v = trim(eq + 1);
        if (strcmp(section, "general") == 0) {
            if (strcmp(k, "reposync_path") == 0)
                set_str(app->cfg.reposync_path, sizeof app->cfg.reposync_path, v);
            else if (strcmp(k, "download_dir") == 0)
                set_str(app->cfg.download_dir, sizeof app->cfg.download_dir, v);
        } else if (strcmp(section, "ssh") == 0) {
            if (strcmp(k, "enabled") == 0)
                app->cfg.use_ssh = atoi(v);
            else if (strcmp(k, "host") == 0)
                set_str(app->cfg.ssh_host, sizeof app->cfg.ssh_host, v);
            else if (strcmp(k, "port") == 0)
                app->cfg.ssh_port = atoi(v);
            else if (strcmp(k, "user") == 0)
                set_str(app->cfg.ssh_user, sizeof app->cfg.ssh_user, v);
            else if (strcmp(k, "password") == 0)
                set_str(app->cfg.ssh_pass, sizeof app->cfg.ssh_pass, v);
            else if (strcmp(k, "remote_reposync_path") == 0)
                set_str(app->cfg.remote_reposync_path,
                        sizeof app->cfg.remote_reposync_path, v);
            else if (strcmp(k, "remote_db_path") == 0)
                set_str(app->cfg.remote_db_path,
                        sizeof app->cfg.remote_db_path, v);
        }
    }
    fclose(f);
#ifdef _WIN32
    app->cfg.use_ssh = 1; /* Windows has no local CLI */
#endif
    return 0;
}

int gui_config_save(const gui_app *app)
{
    char *path = gui_config_path();
    if (!path)
        return -1;
    char *slash = strrchr(path, PATH_SEP);
    if (slash && slash != path) {
        *slash = '\0';
        mkdir_one(path);
        *slash = PATH_SEP;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        free(path);
        return -1;
    }
    fprintf(f, "# reposync-gui configuration\n"
               "[general]\n"
               "reposync_path = %s\n"
               "download_dir = %s\n"
               "\n"
               "[ssh]\n"
               "enabled = %d\n"
               "host = %s\n"
               "port = %d\n"
               "user = %s\n"
               "password = %s\n"
               "remote_reposync_path = %s\n"
               "remote_db_path = %s\n",
               app->cfg.reposync_path, app->cfg.download_dir,
               app->cfg.use_ssh ? 1 : 0,
               app->cfg.ssh_host, app->cfg.ssh_port, app->cfg.ssh_user,
               app->cfg.ssh_pass, app->cfg.remote_reposync_path,
               app->cfg.remote_db_path);
    fclose(f);
    free(path);
    return 0;
}

/* ---------------- op metadata ---------------- */

const char *gui_op_label(op_kind op)
{
    static const char *names[OP_COUNT] = {
        "Update", "Download", "Install", "Remove", "Upgrade", "Autoremove",
        "Sim Install", "Sim Remove", "Sim Upgrade", "List"
    };
    if ((int)op < 0 || op >= OP_COUNT)
        return "?";
    return names[op];
}

int gui_op_needs_pkg(op_kind op)
{
    return op == OP_DOWNLOAD || op == OP_INSTALL || op == OP_REMOVE ||
           op == OP_SIM_INSTALL || op == OP_SIM_REMOVE;
}

int gui_op_needs_dir(op_kind op)
{
    return op == OP_DOWNLOAD;
}

/* ---------------- command building ---------------- */

/* Package names / version fields: letters, digits and + . _ : ~ -
   Spaces separate multiple package names. Anything else is rejected so the
   value can never break out of the command line. */
static int valid_pkg(const char *s)
{
    if (!s || !*s)
        return 0;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || strchr("+_.:~- ", c)))
            return 0;
    }
    return 1;
}

/* Directory: same safe set plus '/'; no spaces (keeps it a single shell arg
   on the remote side without quoting games). */
static int valid_dir(const char *s)
{
    if (!s || !*s)
        return 0;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || strchr("/_.:~-", c)))
            return 0;
    }
    return 1;
}

char *gui_build_cmd(gui_app *app, op_kind op, const char *pkg,
                    const char *dir)
{
    const char *bin = app->cfg.use_ssh ? app->cfg.remote_reposync_path
                                       : app->cfg.reposync_path;
    if (!bin || !*bin) {
        snprintf(app->errmsg, sizeof app->errmsg,
                 "no reposync command configured");
        return NULL;
    }
    char buf[4096];
    int n = -1;
    switch (op) {
    case OP_UPDATE:
        n = snprintf(buf, sizeof buf, "%s update", bin);
        break;
    case OP_LIST:
        /* reposync list [pkg] — queries the local db, never the network */
        if (pkg && *pkg) {
            if (!valid_pkg(pkg)) {
                snprintf(app->errmsg, sizeof app->errmsg,
                         "invalid package name (allowed: letters, digits, + . _ : ~ - and spaces)");
                return NULL;
            }
            n = snprintf(buf, sizeof buf, "%s list %s", bin, pkg);
        } else {
            n = snprintf(buf, sizeof buf, "%s list", bin);
        }
        break;
    case OP_DOWNLOAD:
        if (!valid_pkg(pkg)) {
            snprintf(app->errmsg, sizeof app->errmsg,
                     "invalid package name (allowed: letters, digits, + . _ : ~ - and spaces)");
            return NULL;
        }
        if (!valid_dir(dir)) {
            snprintf(app->errmsg, sizeof app->errmsg,
                     "invalid output dir (no spaces allowed)");
            return NULL;
        }
        n = snprintf(buf, sizeof buf, "%s download %s -o %s", bin, pkg, dir);
        break;
    case OP_INSTALL:
        if (!valid_pkg(pkg)) {
            snprintf(app->errmsg, sizeof app->errmsg,
                     "invalid package name (allowed: letters, digits, + . _ : ~ - and spaces)");
            return NULL;
        }
        n = snprintf(buf, sizeof buf, "%s install -y %s", bin, pkg);
        break;
    case OP_REMOVE:
        if (!valid_pkg(pkg)) {
            snprintf(app->errmsg, sizeof app->errmsg,
                     "invalid package name (allowed: letters, digits, + . _ : ~ - and spaces)");
            return NULL;
        }
        n = snprintf(buf, sizeof buf, "%s remove -y %s", bin, pkg);
        break;
    case OP_UPGRADE:
        n = snprintf(buf, sizeof buf, "%s upgrade -y", bin);
        break;
    case OP_AUTOREMOVE:
        n = snprintf(buf, sizeof buf, "%s autoremove -y", bin);
        break;
    case OP_SIM_INSTALL:
        if (!valid_pkg(pkg)) {
            snprintf(app->errmsg, sizeof app->errmsg,
                     "invalid package name (allowed: letters, digits, + . _ : ~ - and spaces)");
            return NULL;
        }
        n = snprintf(buf, sizeof buf, "%s sim install -y %s", bin, pkg);
        break;
    case OP_SIM_REMOVE:
        if (!valid_pkg(pkg)) {
            snprintf(app->errmsg, sizeof app->errmsg,
                     "invalid package name (allowed: letters, digits, + . _ : ~ - and spaces)");
            return NULL;
        }
        n = snprintf(buf, sizeof buf, "%s sim remove -y %s", bin, pkg);
        break;
    case OP_SIM_UPGRADE:
        n = snprintf(buf, sizeof buf, "%s sim upgrade -y", bin);
        break;
    default:
        snprintf(app->errmsg, sizeof app->errmsg, "unknown operation");
        return NULL;
    }
    if (n < 0 || (size_t)n >= sizeof buf) {
        snprintf(app->errmsg, sizeof app->errmsg, "command line too long");
        return NULL;
    }
    char *r = malloc((size_t)n + 1);
    if (!r)
        return NULL;
    memcpy(r, buf, (size_t)n + 1);
    return r;
}

/* ---------------- backend lifecycle ---------------- */

int gui_ensure_backend(gui_app *app)
{
    if (!app->be) {
        if (app->cfg.use_ssh) {
            app->be = backend_ssh_new(app->cfg.ssh_host, app->cfg.ssh_port,
                                      app->cfg.ssh_user, app->cfg.ssh_pass);
        } else {
            app->be = backend_local_new();
        }
        if (!app->be) {
#ifdef _WIN32
            snprintf(app->errmsg, sizeof app->errmsg,
                     "local backend unavailable on Windows - "
                     "SSH mode is required");
#else
            snprintf(app->errmsg, sizeof app->errmsg,
                     "cannot create backend");
#endif
            return -1;
        }
    }
    if (!app->connected) {
        if (app->be->connect(app->be) < 0) {
            snprintf(app->errmsg, sizeof app->errmsg, "%s", app->be->errmsg);
            return -1;
        }
        app->connected = 1;
    }
    return 0;
}

int gui_run(gui_app *app, const char *cmdline, backend_out_fn cb, void *cbctx,
            int *exitcode)
{
    if (gui_ensure_backend(app) < 0)
        return -1;
    int rc = app->be->run(app->be, cmdline, cb, cbctx, exitcode);
    if (rc < 0)
        snprintf(app->errmsg, sizeof app->errmsg, "%s", app->be->errmsg);
    return rc;
}

/* ---------------- local database queries ---------------- */

/* Query operations run against the local SQLite index (index_packages),
   which `reposync update` fills — exactly like apt's local lists. They never
   spawn the CLI and never touch the network, on Linux and Windows alike. */

char *gui_db_path(void)
{
    const char *env = getenv("REPOSYNC_DB");
    if (env && *env)
        return strdup(env);
#ifdef _WIN32
    char dir[MAX_PATH];
    dir[0] = '\0';
    DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        char *slash = strrchr(dir, '\\');
        if (slash)
            slash[1] = '\0';
    }
    size_t dn = strlen(dir);
    char *p = malloc(dn + 16);
    if (!p)
        return NULL;
    snprintf(p, dn + 16, "%spackages.db", dir);
    return p;
#else
    return strdup("/var/lib/reposync/packages.db");
#endif
}

static void db_bind_or_null(sqlite3_stmt *st, int idx, const char *v)
{
    if (v && *v)
        sqlite3_bind_text(st, idx, v, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, idx);
}

int gui_db_query(gui_app *app, const char *pkg, const char *suite,
                 const char *comp, backend_out_fn cb, void *cbctx,
                 int *count)
{
    *count = 0;
    char *path = gui_db_path();
    if (!path) {
        snprintf(app->errmsg, sizeof app->errmsg, "out of memory");
        return -1;
    }
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        snprintf(app->errmsg, sizeof app->errmsg,
                 "cannot open local db %s: %s", path,
                 db ? sqlite3_errmsg(db) : "unknown");
        if (db)
            sqlite3_close(db);
        free(path);
        return -1;
    }
    const char *SQL =
        "SELECT name,version,architecture,suite,component,description"
        " FROM index_packages"
        " WHERE (?1 IS NULL OR name=?1)"
        "   AND (?2 IS NULL OR suite=?2)"
        "   AND (?3 IS NULL OR component=?3)"
        " ORDER BY name, version;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, SQL, -1, &st, NULL) != SQLITE_OK) {
        snprintf(app->errmsg, sizeof app->errmsg,
                 "local db has no index yet - press Update first (%s)",
                 sqlite3_errmsg(db));
        sqlite3_close(db);
        free(path);
        return -1;
    }
    db_bind_or_null(st, 1, pkg);
    db_bind_or_null(st, 2, suite);
    db_bind_or_null(st, 3, comp);
    while (sqlite3_step(st) == SQLITE_ROW) {
        char line[4096];
        int n = 0;
        const char *f[6];
        int i;
        for (i = 0; i < 6; i++) {
            const char *v = (const char *)sqlite3_column_text(st, i);
            f[i] = v ? v : "";
        }
        n = snprintf(line, sizeof line, "%s\t%s\t%s\t%s\t%s\t%s\n",
                     f[0], f[1], f[2], f[3], f[4], f[5]);
        if (n > 0 && cb)
            cb(cbctx, line, (size_t)n);
        (*count)++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    free(path);
    return 0;
}

int gui_pull_remote_db(gui_app *app)
{
    if (!app->be || !app->be->get_file)
        return 0; /* local mode: the db is already local */
    char *local = gui_db_path();
    if (!local) {
        snprintf(app->errmsg, sizeof app->errmsg, "out of memory");
        return -1;
    }
    if (app->be->get_file(app->be, app->cfg.remote_db_path, local) < 0) {
        snprintf(app->errmsg, sizeof app->errmsg, "%s", app->be->errmsg);
        free(local);
        return -1;
    }
    free(local);
    return 0;
}
