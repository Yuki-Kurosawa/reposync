/* reposync-gui - SDL3 self-drawn UI (Linux + Windows).
   Zero GPL/LGPL: SDL3 (zlib) + SDL3_ttf (zlib) + libssh2 (BSD).
   All widgets are drawn manually with SDL_Renderer; text input uses the
   SDL event queue; the download directory uses the native SDL3 folder
   dialog (SDL_ShowOpenFolderDialog). A worker thread runs each command
   through the backend and streams output into the log view.

   --gui-selftest: build the window, run one test job through the configured
   backend, dump the log to stdout, quit with the job's exit code. */

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "gui.h"
#include "ui.h"

#ifdef _WIN32
#include <windows.h>
#endif

/* ---------------- colors (light theme) ---------------- */
#define C_BG      0xf5, 0xf6, 0xf8
#define C_PANEL   0xec, 0xee, 0xf1
#define C_BTN     0xe2, 0xe6, 0xeb
#define C_BTN_HOT 0xd3, 0xda, 0xe2
#define C_BTN_ACT 0x1a, 0x73, 0xe8
#define C_EDGE    0xb4, 0xbb, 0xc4
#define C_TEXT    0x1f, 0x24, 0x2b
#define C_DIM     0x64, 0x6c, 0x76
#define C_LOG     0xff, 0xff, 0xff
#define C_STATUS  0xd9, 0xf2, 0xe3

#define WIN_W 1120
#define WIN_H 740
#define PAD 8
#define BTN_H 34
#define IN_H 30
#define ROW1_Y 12
#define ROW2_LABEL_Y 50
#define ROW2_Y 66
#define ROW3_LABEL_Y 102
#define ROW3_Y 118
#define ROW4_LABEL_Y 154
#define ROW4_Y 170
#define LOG_TOP 208
#define STAT_H 26
#define FONT_PT 15

/* ---------------- thread-safe log buffer ---------------- */
typedef struct {
    char *buf;
    size_t len, cap;
    SDL_Mutex *mu;
} logbuf;

static void lb_init(logbuf *lb)
{
    lb->buf = NULL;
    lb->len = lb->cap = 0;
    lb->mu = SDL_CreateMutex();
}

static void lb_free(logbuf *lb)
{
    if (lb->mu)
        SDL_DestroyMutex(lb->mu);
    free(lb->buf);
}

static void lb_append(logbuf *lb, const char *d, size_t n)
{
    if (!lb->mu)
        return;
    SDL_LockMutex(lb->mu);
    if (lb->len + n + 1 > lb->cap) {
        size_t nc = lb->cap ? lb->cap : 8192;
        while (lb->len + n + 1 > nc)
            nc *= 2;
        char *nb = realloc(lb->buf, nc);
        if (!nb) {
            SDL_UnlockMutex(lb->mu);
            return;
        }
        lb->buf = nb;
        lb->cap = nc;
    }
    memcpy(lb->buf + lb->len, d, n);
    lb->len += n;
    lb->buf[lb->len] = '\0';
    SDL_UnlockMutex(lb->mu);
}

/* copy the whole buffer (caller frees) */
static char *lb_snapshot(logbuf *lb, size_t *n)
{
    SDL_LockMutex(lb->mu);
    char *r = malloc(lb->len + 1);
    if (r) {
        memcpy(r, lb->buf, lb->len);
        r[lb->len] = '\0';
        *n = lb->len;
    } else {
        *n = 0;
    }
    SDL_UnlockMutex(lb->mu);
    return r;
}

/* ---------------- widgets ---------------- */
typedef struct {
    SDL_FRect r;
    char label[64];
    int id;              /* op_kind or widget id */
    int hover;
} button_t;

typedef struct {
    SDL_FRect r;
    char buf[512];
    int focus;
    int cursor;          /* byte offset into buf */
} input_t;

typedef struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    TTF_Font *font;
    int font_pt;

    gui_app *app;

    /* widgets */
    button_t ops[OP_COUNT];
    input_t pkg_e, dir_e, host_e, port_e, user_e, pass_e;
    input_t suite_e, comp_e;
    button_t browse_btn, ssh_chk, filter_btn, repo_btn;

    /* log */
    logbuf log;
    int log_scroll;      /* scroll offset in lines (0 = bottom) */
    int log_lines;

    /* list output capture (reposync list -> filtered by suite/component) */
    logbuf listlb;
    int last_list_count;

    /* state */
    char status[512];
    int busy;
    int quit;
    int selftest_done;
    int selftest_rc;     /* exit code for --gui-selftest mode */

    /* jobs */
    SDL_Thread *worker;
    struct job *job;
} ui_state;

typedef struct job {
    ui_state *st;
    op_kind op;
    char *cmdline;
    char *pkg, *suite, *comp;  /* local query filters (OP_LIST) */
    int exitcode;
    int count;                 /* rows returned by a local query */
    int transport_ok;
    int capture_list;    /* capture stdout into st->listlb too */
} job_t;

/* ---------------- small helpers ---------------- */

static void set_status(ui_state *st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->status, sizeof st->status, fmt, ap);
    va_end(ap);
}

/* delete the last UTF-8 codepoint from s (in place) */
static void utf8_back(char *s)
{
    size_t n = strlen(s);
    if (n == 0)
        return;
    n--;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--;
    s[n] = '\0';
}

static int point_in(const SDL_FRect *r, float x, float y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* ---------------- rendering helpers ---------------- */

static void draw_rect(ui_state *st, const SDL_FRect *r, Uint8 cr, Uint8 cg,
                      Uint8 cb, int filled)
{
    SDL_SetRenderDrawColor(st->ren, cr, cg, cb, 255);
    if (filled)
        SDL_RenderFillRect(st->ren, r);
    else
        SDL_RenderRect(st->ren, r);
}

static void draw_text(ui_state *st, const char *s, float x, float y,
                      SDL_Color c)
{
    if (!st->font || !s || !*s)
        return;
    SDL_Surface *surf = TTF_RenderText_Blended(st->font, s, 0, c);
    if (!surf)
        return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(st->ren, surf);
    SDL_DestroySurface(surf);
    if (!tex)
        return;
    SDL_FRect dst = { x, y, 0, 0 };
    float tw = 0, th = 0;
    SDL_GetTextureSize(tex, &tw, &th);
    dst.w = tw;
    dst.h = th;
    SDL_RenderTexture(st->ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
}

static void draw_text_centered(ui_state *st, const char *s, const SDL_FRect *r,
                               SDL_Color c)
{
    if (!st->font)
        return;
    int w = 0, h = 0;
    if (TTF_GetStringSize(st->font, s ? s : "", 0, &w, &h) != true)
        return;
    draw_text(st, s, r->x + (r->w - (float)w) / 2,
              r->y + (r->h - (float)h) / 2, c);
}

static void draw_button(ui_state *st, button_t *b)
{
    Uint8 r, g, bl;
    SDL_Color tc;
    if (b->id == 1000) { /* primary action: blue fill, white text */
        r = 0x1a;
        g = 0x73;
        bl = 0xe8;
        tc = (SDL_Color){ 0xff, 0xff, 0xff, 255 };
    } else if (b->hover) {
        r = 0xd3;
        g = 0xda;
        bl = 0xe2;
        tc = (SDL_Color){ C_TEXT, 255 };
    } else {
        r = 0xe2;
        g = 0xe6;
        bl = 0xeb;
        tc = (SDL_Color){ C_TEXT, 255 };
    }
    draw_rect(st, &b->r, r, g, bl, 1);
    draw_rect(st, &b->r, C_EDGE, 0);
    draw_text_centered(st, b->label, &b->r, tc);
}

static void draw_input(ui_state *st, input_t *in, const char *label)
{
    draw_rect(st, &in->r, C_LOG, 1);
    if (in->focus)
        draw_rect(st, &in->r, C_BTN_ACT, 0);
    else
        draw_rect(st, &in->r, C_EDGE, 0);
    if (label && *label)
        draw_text(st, label, in->r.x, in->r.y - 18,
                  (SDL_Color){ C_DIM, 255 });
    if (in->buf[0])
        draw_text(st, in->buf, in->r.x + 6, in->r.y + 5,
                  (SDL_Color){ C_TEXT, 255 });
    if (in->focus) {
        /* cursor line after the text */
        int w = 0, h = 0;
        TTF_GetStringSize(st->font, in->buf, 0, &w, &h);
        SDL_SetRenderDrawColor(st->ren, C_TEXT, 255);
        float cx = in->r.x + 8 + (float)w;
        SDL_RenderLine(st->ren, cx, in->r.y + 4, cx, in->r.y + in->r.h - 4);
    }
}

/* ---------------- font ---------------- */

static TTF_Font *load_font(int pt)
{
    static const char *candidates[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\msyhbd.ttc",
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\simsun.ttc",
        "C:\\Windows\\Fonts\\arial.ttf",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
#endif
        NULL
    };
    size_t i;
    for (i = 0; candidates[i]; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (!f)
            continue;
        fclose(f);
        TTF_Font *ft = TTF_OpenFont(candidates[i], pt);
        if (ft)
            return ft;
    }
    return NULL;
}

/* ---------------- layout ---------------- */

static void layout(ui_state *st)
{
    float x = PAD;
    size_t i;
    /* operation buttons */
    for (i = 0; i < OP_COUNT; i++) {
        button_t *b = &st->ops[i];
        b->r.x = x;
        b->r.y = ROW1_Y;
        b->r.w = 104;
        b->r.h = BTN_H;
        snprintf(b->label, sizeof b->label, "%s", gui_op_label((op_kind)i));
        b->id = (int)i;
        x += 104 + 6;
    }
    /* package + dir row */
    st->pkg_e.r = (SDL_FRect){ PAD, ROW2_Y, 220, IN_H };
    st->browse_btn.r = (SDL_FRect){ PAD + 220 + 8, ROW2_Y - 2, 84, BTN_H };
    snprintf(st->browse_btn.label, sizeof st->browse_btn.label, "Browse");
    st->browse_btn.id = 2000;
    st->dir_e.r = (SDL_FRect){ PAD + 220 + 8 + 84 + 8, ROW2_Y, 380, IN_H };
    /* ssh row */
    st->ssh_chk.r = (SDL_FRect){ PAD, ROW3_Y, 22, 22 };
    snprintf(st->ssh_chk.label, sizeof st->ssh_chk.label, "SSH");
    st->ssh_chk.id = 2001;
    st->host_e.r = (SDL_FRect){ PAD + 62, ROW3_Y, 150, IN_H };
    st->port_e.r = (SDL_FRect){ PAD + 62 + 150 + 8, ROW3_Y, 64, IN_H };
    st->user_e.r = (SDL_FRect){ PAD + 62 + 150 + 8 + 64 + 8, ROW3_Y, 110, IN_H };
    st->pass_e.r = (SDL_FRect){ PAD + 62 + 150 + 8 + 64 + 8 + 110 + 8,
                                ROW3_Y, 170, IN_H };
    st->pass_e.buf[0] = '\0';
    /* repo + filter row */
    st->repo_btn.r = (SDL_FRect){ PAD, ROW4_Y - 2, 170, BTN_H };
    snprintf(st->repo_btn.label, sizeof st->repo_btn.label, "Repo: -");
    st->repo_btn.id = 3000;
    st->suite_e.r = (SDL_FRect){ PAD + 170 + 8, ROW4_Y, 150, IN_H };
    st->comp_e.r = (SDL_FRect){ PAD + 170 + 8 + 150 + 8, ROW4_Y, 150, IN_H };
    st->filter_btn.r = (SDL_FRect){ PAD + 170 + 8 + 150 + 8 + 150 + 8,
                                    ROW4_Y - 2, 90, BTN_H };
    snprintf(st->filter_btn.label, sizeof st->filter_btn.label, "Filter");
    st->filter_btn.id = 3001;
}

/* ---------------- jobs ---------------- */

static void job_out_cb(void *ctx, const char *d, size_t n)
{
    job_t *j = ctx;
    lb_append(&j->st->log, d, n);
    if (j->capture_list)
        lb_append(&j->st->listlb, d, n);
}

/* returns number of lines in buf */
static int count_lines(const char *s, size_t n)
{
    int lines = 1;
    size_t i;
    for (i = 0; i < n; i++)
        if (s[i] == '\n')
            lines++;
    return lines;
}

static void append_log_line(ui_state *st, const char *line)
{
    lb_append(&st->log, line, strlen(line));
    lb_append(&st->log, "\n", 1);
}

static int SDLCALL worker_main(void *data)
{
    job_t *j = data;
    ui_state *st = j->st;
    if (j->capture_list) {
        /* OP_LIST queries the *local* database directly - no CLI process,
           no SSH, no network (Linux and Windows alike). `reposync update`
           filled the db; apt keeps its lists locally the same way. */
        j->transport_ok = (gui_db_query(st->app, j->pkg, j->suite, j->comp,
                                        job_out_cb, j, &j->count) == 0);
        j->exitcode = j->transport_ok ? 0 : 1;
    } else {
        j->transport_ok = (gui_run(st->app, j->cmdline, job_out_cb, j,
                                   &j->exitcode) == 0);
        /* after a remote update, pull the remote packages.db back so all
           subsequent queries are local (offline-capable) */
        if (j->transport_ok && j->exitcode == 0 && j->op == OP_UPDATE &&
            st->app->cfg.use_ssh) {
            if (gui_pull_remote_db(st->app) != 0) {
                char msg[600];
                snprintf(msg, sizeof msg, "\r\n[db sync] %s\r\n",
                         st->app->errmsg);
                job_out_cb(j, msg, strlen(msg));
                j->exitcode = 2;
            } else {
                char msg[160];
                snprintf(msg, sizeof msg,
                         "\r\n[db sync] local database updated - "
                         "queries are now offline\r\n");
                job_out_cb(j, msg, strlen(msg));
            }
        }
    }
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_USER;
    ev.user.code = 1;
    ev.user.data1 = j;
    SDL_PushEvent(&ev);
    return 0;
}

static void start_job(ui_state *st, op_kind op)
{
    if (st->busy) {
        set_status(st, "busy - wait for the current job to finish");
        return;
    }
    /* sync fields from widgets into the config */
    snprintf(st->app->cfg.ssh_host, sizeof st->app->cfg.ssh_host, "%.255s",
             st->host_e.buf);
    st->app->cfg.ssh_port = atoi(st->port_e.buf);
    snprintf(st->app->cfg.ssh_user, sizeof st->app->cfg.ssh_user, "%.63s",
             st->user_e.buf);
    snprintf(st->app->cfg.ssh_pass, sizeof st->app->cfg.ssh_pass, "%.255s",
             st->pass_e.buf);
    snprintf(st->app->cfg.download_dir, sizeof st->app->cfg.download_dir,
             "%s", st->dir_e.buf);

    char *cmd;
    if (op == OP_LIST) {
        /* local query: only shown in the log as a marker; nothing is
           executed, no CLI/SSH configuration is required */
        char tmp[1024];
        if (st->pkg_e.buf[0])
            snprintf(tmp, sizeof tmp, "[local db] list %s", st->pkg_e.buf);
        else
            snprintf(tmp, sizeof tmp, "[local db] list");
        cmd = strdup(tmp);
    } else {
        cmd = gui_build_cmd(st->app, op, st->pkg_e.buf, st->dir_e.buf);
    }
    if (!cmd) {
        set_status(st, "%s", st->app->errmsg);
        return;
    }
    job_t *j = calloc(1, sizeof *j);
    if (!j) {
        free(cmd);
        set_status(st, "out of memory");
        return;
    }
    j->st = st;
    j->op = op;
    j->cmdline = cmd;
    j->exitcode = -1;
    j->capture_list = (op == OP_LIST);
    if (j->capture_list) {
        /* fresh capture for the next filter run */
        lb_free(&st->listlb);
        lb_init(&st->listlb);
        st->last_list_count = 0;
        /* snapshot the local query filters for the worker thread */
        if (st->pkg_e.buf[0])
            j->pkg = strdup(st->pkg_e.buf);
        if (st->suite_e.buf[0])
            j->suite = strdup(st->suite_e.buf);
        if (st->comp_e.buf[0])
            j->comp = strdup(st->comp_e.buf);
    }
    st->job = j;

    char hdr[4300];
    int hn = snprintf(hdr, sizeof hdr, "\r\n$ %s\r\n", cmd);
    lb_append(&st->log, hdr, (size_t)hn > sizeof hdr ? sizeof hdr : (size_t)hn);
    st->log_scroll = 0;
    set_status(st, "running...");
    st->busy = 1;
    st->worker = SDL_CreateThread(worker_main, "reposync-job", j);
    if (!st->worker) {
        set_status(st, "cannot create thread: %s", SDL_GetError());
        st->busy = 0;
        free(cmd);
        free(j);
        st->job = NULL;
        return;
    }
}

static void finish_job(ui_state *st, job_t *j)
{
    char line[80];
    snprintf(line, sizeof line, "[exit %d]%s", j->exitcode,
             j->transport_ok ? "" : "  (transport error)");
    append_log_line(st, line);
    if (!j->transport_ok)
        set_status(st, "%s", st->app->errmsg[0] ? st->app->errmsg
                                                : "transport failed");
    else if (j->exitcode != 0)
        set_status(st, "command exited with %d", j->exitcode);
    else if (j->capture_list) {
        st->last_list_count = j->count;
        set_status(st, "listed %d package(s) - filter by suite/component or type package name and List again",
                   j->count);
    } else {
        set_status(st, "ok");
    }
    st->busy = 0;
    st->job = NULL;

    if (st->app->selftest) {
        size_t n = 0;
        char *all = lb_snapshot(&st->log, &n);
        printf("GUI-SELFTEST BEGIN\n%s\nGUI-SELFTEST END\n"
               "GUI-SELFTEST transport_ok=%d exitcode=%d\n",
               all ? all : "", j->transport_ok, j->exitcode);
        fflush(stdout);
        free(all);
        st->selftest_rc = (j->transport_ok && j->exitcode == 0) ? 0 : 1;
        st->selftest_done = 1;
        st->quit = 1;
    }
    free(j->cmdline);
    free(j->pkg);
    free(j->suite);
    free(j->comp);
    free(j);
}

/* ---------------- folder dialog callback ---------------- */

static void SDLCALL folder_cb(void *userdata, const char * const *filelist,
                              int filter)
{
    (void)userdata;
    (void)filter;
    if (!filelist || !filelist[0])
        return; /* cancelled */
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_USER;
    ev.user.code = 2;
    ev.user.data1 = strdup(filelist[0]);
    SDL_PushEvent(&ev);
}

/* ---------------- rendering ---------------- */

static void render(ui_state *st)
{
    SDL_SetRenderDrawColor(st->ren, C_BG, 255);
    SDL_RenderClear(st->ren);

    size_t i;
    for (i = 0; i < OP_COUNT; i++)
        draw_button(st, &st->ops[i]);

    /* labels */
    draw_text(st, "Package", PAD, ROW2_LABEL_Y, (SDL_Color){ C_DIM, 255 });
    draw_text(st, "Dir", PAD + 220 + 8 + 84 + 8, ROW2_LABEL_Y,
              (SDL_Color){ C_DIM, 255 });

    draw_input(st, &st->pkg_e, "");
    draw_input(st, &st->dir_e, "");
    draw_button(st, &st->browse_btn);

    /* ssh checkbox */
    if (st->app->cfg.use_ssh)
        draw_rect(st, &st->ssh_chk.r, C_BTN_ACT, 1);
    else
        draw_rect(st, &st->ssh_chk.r, C_LOG, 1);
    draw_rect(st, &st->ssh_chk.r, C_EDGE, 0);
    if (st->app->cfg.use_ssh)
        draw_text(st, "x", st->ssh_chk.r.x + 5, st->ssh_chk.r.y + 1,
                  (SDL_Color){ 0xff, 0xff, 0xff, 255 });
    draw_text(st, "SSH", PAD + 30, ROW3_Y + 5, (SDL_Color){ C_TEXT, 255 });
    draw_input(st, &st->host_e, "host");
    draw_input(st, &st->port_e, "");
    draw_input(st, &st->user_e, "user");
    draw_input(st, &st->pass_e, "pass");

    /* repo + filter row */
    draw_button(st, &st->repo_btn);
    draw_input(st, &st->suite_e, "Suite");
    draw_input(st, &st->comp_e, "Component");
    draw_button(st, &st->filter_btn);

    /* log view */
    SDL_FRect lr = { PAD, LOG_TOP, WIN_W - 2 * PAD, WIN_H - LOG_TOP - PAD - STAT_H - 4 };
    draw_rect(st, &lr, C_LOG, 1);
    draw_rect(st, &lr, C_EDGE, 0);

    size_t n = 0;
    char *all = lb_snapshot(&st->log, &n);
    if (all) {
        /* collect visible lines from the bottom, honouring scroll */
        const char *end = all + n;
        int total = count_lines(all, n);
        int vis = (int)(lr.h / 20);
        int skip = total - vis - st->log_scroll;
        if (skip < 0)
            skip = 0;
        /* walk to the start line */
        const char *line_start = all;
        int ln = 0;
        while (ln < skip && line_start < end) {
            const char *nl = strchr(line_start, '\n');
            if (!nl)
                break;
            line_start = nl + 1;
            ln++;
        }
        float y = lr.y + 4;
        while (line_start < end && y + 18 < lr.y + lr.h) {
            const char *nl = strchr(line_start, '\n');
            size_t llen = nl ? (size_t)(nl - line_start) : (size_t)(end - line_start);
            if (llen > 0) {
                char *tmp = malloc(llen + 1);
                if (tmp) {
                    memcpy(tmp, line_start, llen);
                    tmp[llen] = '\0';
                    draw_text(st, tmp, lr.x + 6, y, (SDL_Color){ C_TEXT, 255 });
                    free(tmp);
                }
            }
            if (!nl)
                break;
            line_start = nl + 1;
            y += 20;
        }
        free(all);
    }

    /* status bar */
    SDL_FRect sr = { PAD, WIN_H - STAT_H - 6, WIN_W - 2 * PAD, STAT_H };
    draw_rect(st, &sr, C_STATUS, 1);
    draw_text(st, st->status, sr.x + 8, sr.y + 4, (SDL_Color){ C_TEXT, 255 });

    SDL_RenderPresent(st->ren);
}

/* ---------------- list filter + repo type switch ---------------- */

/* Callback for a synchronous filter query: mirror the rows into the log
   and into the list capture buffer. */
static void filter_cb(void *ctx, const char *d, size_t n)
{
    ui_state *st = ctx;
    lb_append(&st->log, d, n);
    lb_append(&st->listlb, d, n);
}

/* The GUI's "分类查找" (suite/component classification search): query the
   local database directly with the two filter boxes (empty = wildcard).
   No CLI process, no SSH, no network - the db was filled by update. */
static void do_filter(ui_state *st)
{
    if (st->busy) {
        set_status(st, "busy - wait for the current job to finish");
        return;
    }
    const char *suite = st->suite_e.buf;
    const char *comp = st->comp_e.buf;
    if (!suite[0] && !comp[0]) {
        set_status(st,
                   "type a suite and/or component, then press Filter "
                   "(or press List for everything)");
        return;
    }
    lb_free(&st->listlb);
    lb_init(&st->listlb);
    st->last_list_count = 0;
    int count = 0;
    /* small local query: run synchronously */
    if (gui_db_query(st->app, NULL, suite, comp, filter_cb, st, &count) < 0) {
        set_status(st, "%s", st->app->errmsg);
        return;
    }
    st->log_scroll = 0;
    set_status(st, "%d package(s) match suite='%s' component='%s'",
               count, suite, comp);
}

/* Cycle to the next Repo-Type declared in reposync.conf and persist it as
   Active-Repo. The CLI reads the same file, so the next operation uses the
   switched command set (跨组件/跨套件控制仓库对应组件). */
static void cycle_repo(ui_state *st)
{
    gui_repo *rp = &st->app->repo;
    if (rp->ntypes == 0) {
        set_status(st, "no Repo-Type records found in reposync.conf");
        return;
    }
    size_t cur = 0;
    size_t i;
    for (i = 0; i < rp->ntypes; i++)
        if (strcmp(rp->types[i], rp->active) == 0) {
            cur = i;
            break;
        }
    size_t next = (cur + 1) % rp->ntypes;
    if (gui_repo_set_active(st->app, rp->types[next]) != 0) {
        set_status(st, "%s", st->app->errmsg);
        return;
    }
    snprintf(st->repo_btn.label, sizeof st->repo_btn.label, "Repo: %.56s",
             st->app->repo.active);
    set_status(st, "active repo type switched to '%s'%s", st->app->repo.active,
               st->app->repo.path[0] ? " (next op uses its command set)" : "");
}

/* ---------------- input ---------------- */

static input_t *hit_input(ui_state *st, float x, float y)
{
    input_t *ins[] = { &st->pkg_e, &st->dir_e, &st->host_e, &st->port_e,
                       &st->user_e, &st->pass_e, &st->suite_e, &st->comp_e,
                       NULL };
    size_t i;
    for (i = 0; ins[i]; i++)
        if (point_in(&ins[i]->r, x, y))
            return ins[i];
    return NULL;
}

static void set_focus(ui_state *st, input_t *in)
{
    input_t *ins[] = { &st->pkg_e, &st->dir_e, &st->host_e, &st->port_e,
                       &st->user_e, &st->pass_e, &st->suite_e, &st->comp_e,
                       NULL };
    size_t i;
    for (i = 0; ins[i]; i++)
        ins[i]->focus = (ins[i] == in);
    if (in) {
        SDL_Rect r = { (int)in->r.x, (int)in->r.y, (int)in->r.w, (int)in->r.h };
        SDL_SetTextInputArea(st->win, &r, (int)in->r.x);
    }
}

/* ---------------- events ---------------- */

static void handle_mouse(ui_state *st, float mx, float my)
{
    size_t i;
    for (i = 0; i < OP_COUNT; i++) {
        if (point_in(&st->ops[i].r, mx, my)) {
            start_job(st, (op_kind)i);
            return;
        }
    }
    if (point_in(&st->browse_btn.r, mx, my)) {
        SDL_ShowOpenFolderDialog(folder_cb, st, st->win,
                                 st->app->cfg.download_dir[0]
                                     ? st->app->cfg.download_dir : NULL,
                                 false);
        return;
    }
    if (point_in(&st->ssh_chk.r, mx, my)) {
        st->app->cfg.use_ssh = !st->app->cfg.use_ssh;
        return;
    }
    if (point_in(&st->repo_btn.r, mx, my)) {
        cycle_repo(st);
        return;
    }
    if (point_in(&st->filter_btn.r, mx, my)) {
        do_filter(st);
        return;
    }
    input_t *in = hit_input(st, mx, my);
    set_focus(st, in);
}

static void handle_text(ui_state *st, const char *t)
{
    input_t *in = NULL;
    input_t *ins[] = { &st->pkg_e, &st->dir_e, &st->host_e, &st->port_e,
                       &st->user_e, &st->pass_e, &st->suite_e, &st->comp_e,
                       NULL };
    size_t i;
    for (i = 0; ins[i]; i++)
        if (ins[i]->focus)
            in = ins[i];
    if (!in)
        return;
    size_t l = strlen(in->buf), tl = strlen(t);
    if (l + tl + 1 < sizeof in->buf)
        memcpy(in->buf + l, t, tl + 1);
}

static void handle_key(ui_state *st, SDL_Keycode k)
{
    input_t *in = NULL;
    input_t *ins[] = { &st->pkg_e, &st->dir_e, &st->host_e, &st->port_e,
                       &st->user_e, &st->pass_e, &st->suite_e, &st->comp_e,
                       NULL };
    size_t i;
    for (i = 0; ins[i]; i++)
        if (ins[i]->focus)
            in = ins[i];
    if (!in)
        return;
    if (k == SDLK_BACKSPACE)
        utf8_back(in->buf);
}

/* ---------------- ui_run ---------------- */

int ui_run(gui_app *app)
{
    /* SDL3 APIs return bool: true on success, false on failure. */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!TTF_Init()) {
        fprintf(stderr, "TTF_Init: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    ui_state st;
    memset(&st, 0, sizeof st);
    st.app = app;
    lb_init(&st.log);

    st.win = SDL_CreateWindow("reposync-gui", WIN_W, WIN_H,
                              SDL_WINDOW_RESIZABLE);
    if (!st.win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        fprintf(stderr, "tip: on a headless box set SDL_VIDEODRIVER=dummy\n");
        lb_free(&st.log);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    st.ren = SDL_CreateRenderer(st.win, NULL);
    if (!st.ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(st.win);
        lb_free(&st.log);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    st.font = load_font(FONT_PT);
    if (!st.font)
        fprintf(stderr, "warning: no usable system font found; UI text "
                        "will be empty (install a CJK-capable font)\n");

    layout(&st);
    set_status(&st, "ready - pick an operation");

    /* initialise widgets from config */
    snprintf(st.host_e.buf, sizeof st.host_e.buf, "%s", app->cfg.ssh_host);
    snprintf(st.port_e.buf, sizeof st.port_e.buf, "%d", app->cfg.ssh_port);
    snprintf(st.user_e.buf, sizeof st.user_e.buf, "%s", app->cfg.ssh_user);
    snprintf(st.pass_e.buf, sizeof st.pass_e.buf, "%s", app->cfg.ssh_pass);
    snprintf(st.dir_e.buf, sizeof st.dir_e.buf, "%s", app->cfg.download_dir);

    if (app->selftest)
        start_job(&st, OP_UPDATE);

    while (!st.quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                st.quit = 1;
            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                       ev.button.button == SDL_BUTTON_LEFT) {
                handle_mouse(&st, ev.button.x, ev.button.y);
            } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                size_t i;
                for (i = 0; i < OP_COUNT; i++)
                    st.ops[i].hover = point_in(&st.ops[i].r, ev.motion.x,
                                               ev.motion.y);
                st.browse_btn.hover = point_in(&st.browse_btn.r, ev.motion.x,
                                               ev.motion.y);
            } else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                st.log_scroll += (int)ev.wheel.y;
                if (st.log_scroll < 0)
                    st.log_scroll = 0;
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                handle_key(&st, ev.key.key);
            } else if (ev.type == SDL_EVENT_TEXT_INPUT) {
                handle_text(&st, ev.text.text);
            } else if (ev.type == SDL_EVENT_USER) {
                if (ev.user.code == 1)
                    finish_job(&st, ev.user.data1);
                else if (ev.user.code == 2) {
                    char *path = ev.user.data1;
                    if (path) {
                        snprintf(st.dir_e.buf, sizeof st.dir_e.buf, "%s",
                                 path);
                        snprintf(st.app->cfg.download_dir,
                                 sizeof st.app->cfg.download_dir, "%s", path);
                        free(path);
                    }
                }
            }
        }
        render(&st);
        SDL_Delay(16);
    }

    if (st.worker)
        SDL_WaitThread(st.worker, NULL);

    gui_config_save(app);
    lb_free(&st.log);
    if (st.font)
        TTF_CloseFont(st.font);
    SDL_DestroyRenderer(st.ren);
    SDL_DestroyWindow(st.win);
    TTF_Quit();
    SDL_Quit();
    return app->selftest ? st.selftest_rc : 0;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine,
                   int nCmdShow)
{
    (void)hInst;
    (void)hPrev;
    (void)nCmdShow;
    SDL_SetMainReady();
    char *argv[4];
    int argc = 1;
    argv[0] = "reposync-gui";
    if (lpCmdLine && strstr(lpCmdLine, "--gui-selftest"))
        argv[argc++] = "--gui-selftest";
    return gui_main(argc, argv);
}
#endif
