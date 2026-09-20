#include "util.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define PATHSEP_CHAR '\\'
#else
#include <unistd.h>
#include <sys/wait.h>
#define PATHSEP_CHAR '/'
#endif

char path_sep(void)
{
    return PATHSEP_CHAR;
}

const char *native_deb_arch(void)
{
    static const char *cached = NULL;
    if (cached)
        return cached;
    static char buf[32];
#ifdef _WIN32
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    const char *m = "amd64";
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: m = "amd64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: m = "arm64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: m = "i386"; break;
    default: m = "amd64"; break;
    }
#else
    FILE *f = popen("uname -m", "r");
    const char *m = "amd64";
    if (f && fgets(buf, sizeof buf, f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
        m = buf;
    }
    if (f)
        pclose(f);
#endif

    if (strcmp(m, "x86_64") == 0 || strcmp(m, "amd64") == 0)
        cached = "amd64";
    else if (strcmp(m, "aarch64") == 0 || strcmp(m, "arm64") == 0)
        cached = "arm64";
    else if (strcmp(m, "i686") == 0 || strcmp(m, "i386") == 0 || strcmp(m, "x86") == 0)
        cached = "i386";
    else if (strcmp(m, "armv7l") == 0 || strcmp(m, "armhf") == 0)
        cached = "armhf";
    else if (strcmp(m, "riscv64") == 0)
        cached = "riscv64";
    else if (strcmp(m, "ppc64le") == 0)
        cached = "ppc64el";
    else if (strcmp(m, "s390x") == 0)
        cached = "s390x";
    else {
        static char fallback[32];
        snprintf(fallback, sizeof fallback, "%s", m);
        cached = fallback;
    }
    return cached;
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "reposync: out of memory\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "reposync: out of memory\n");
        exit(1);
    }
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *r = xmalloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

int mkdir_p(const char *path, mode_t mode)
{
    sb_t tmp;
    sb_init(&tmp);
    const char *p = path;
    int rc = 0;

#ifdef _WIN32
    /* skip drive prefix "C:" */
    if (p[0] && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) {
        sb_append(&tmp, p, 3);
        p += 3;
    }
    if (*p == '\\' || *p == '/')
        sb_append_char(&tmp, PATHSEP_CHAR);
#else
    if (*p == '/')
        sb_append_char(&tmp, '/');
#endif

    while (*p) {
#ifdef _WIN32
        while (*p == '\\' || *p == '/')
#else
        while (*p == '/')
#endif
            p++;
        if (!*p)
            break;
        const char *seg = p;
#ifdef _WIN32
        while (*p && *p != '\\' && *p != '/')
#else
        while (*p && *p != '/')
#endif
            p++;
        if (tmp.len && tmp.s[tmp.len - 1] != PATHSEP_CHAR)
            sb_append_char(&tmp, PATHSEP_CHAR);
        sb_append(&tmp, seg, (size_t)(p - seg));
#ifdef _WIN32
        if (_mkdir(tmp.s) < 0 && errno != EEXIST) {
#else
        if (mkdir(tmp.s, mode) < 0 && errno != EEXIST) {
#endif
            rc = -1;
            break;
        }
    }
    sb_free(&tmp);
    return rc;
}

char *path_join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    int slash = (la && a[la - 1] != '/' && a[la - 1] != '\\') ? 1 : 0;
    char *r = xmalloc(la + slash + lb + 1);
    memcpy(r, a, la);
    if (slash)
        r[la] = PATHSEP_CHAR;
    memcpy(r + la + slash, b, lb + 1);
    return r;
}

int read_file(const char *path, sb_t *b)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (sb_append(b, buf, n) < 0) {
            fclose(f);
            return -1;
        }
    }
    int err = ferror(f);
    fclose(f);
    return err ? -1 : 0;
}

int write_file(const char *path, const char *data, size_t len, mode_t mode)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    if (len && fwrite(data, 1, len, f) != len) {
        fclose(f);
        unlink(path);
        return -1;
    }
    if (fclose(f) < 0) {
        unlink(path);
        return -1;
    }
#ifndef _WIN32
    chmod(path, mode);
#else
    (void)mode;
#endif
    return 0;
}

void fnv1a64_hex(const char *s, char out[17])
{
    unsigned long long h = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        h ^= (unsigned long long)*p++;
        h *= 1099511628211ULL;
    }
    snprintf(out, 17, "%016llx", h);
}

int gzip_mem(const char *data, size_t len, sb_t *out)
{
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return -1;
    zs.next_in = (Bytef *)data;
    zs.avail_in = (uInt)(len > 0xFFFFFFFFu ? 0xFFFFFFFFu : len);
    unsigned char buf[65536];
    int ret;
    do {
        zs.next_out = buf;
        zs.avail_out = sizeof buf;
        ret = deflate(&zs, Z_FINISH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            deflateEnd(&zs);
            return -1;
        }
        size_t got = sizeof buf - zs.avail_out;
        if (got && sb_append(out, (const char *)buf, got) < 0) {
            deflateEnd(&zs);
            return -1;
        }
    } while (ret != Z_STREAM_END);
    deflateEnd(&zs);
    return 0;
}

/* ---- Debian version comparison (policy semantics, own implementation) ---- */

static int vorder(int c)
{
    if (c == '~')
        return -1;
    if (c == '\0')
        return 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return c;
    if (c >= '0' && c <= '9')
        return c + 256;
    return c + 512;
}

static int verrevcmp(const char *a, const char *b)
{
    while (*a || *b) {
        int first_diff = 0;

        while ((*a && !isdigit((unsigned char)*a)) || (*b && !isdigit((unsigned char)*b))) {
            int ac = vorder((unsigned char)*a);
            int bc = vorder((unsigned char)*b);
            if (ac != bc)
                return ac - bc;
            a++;
            b++;
        }

        while (*a == '0')
            a++;
        while (*b == '0')
            b++;

        while (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            if (!first_diff)
                first_diff = (int)(unsigned char)*a - (int)(unsigned char)*b;
            a++;
            b++;
        }

        if (isdigit((unsigned char)*a) || isdigit((unsigned char)*b))
            return isdigit((unsigned char)*a) ? 1 : -1;
        if (first_diff)
            return first_diff;
    }
    return 0;
}

int deb_vercmp(const char *a, const char *b)
{
    const char *ea = strchr(a, ':');
    const char *eb = strchr(b, ':');
    long long epoch_a = 0, epoch_b = 0;
    const char *ua = a, *ub = b;

    if (ea) {
        char *end = NULL;
        long long v = strtoll(a, &end, 10);
        if (end == ea) {
            epoch_a = v;
            ua = ea + 1;
        }
    }
    if (eb) {
        char *end = NULL;
        long long v = strtoll(b, &end, 10);
        if (end == eb) {
            epoch_b = v;
            ub = eb + 1;
        }
    }
    if (epoch_a != epoch_b)
        return epoch_a < epoch_b ? -1 : 1;

    const char *ra = strrchr(ua, '-');
    const char *rb = strrchr(ub, '-');
    sb_t ua_s, ub_s, ra_s, rb_s;
    sb_init(&ua_s); sb_init(&ub_s); sb_init(&ra_s); sb_init(&rb_s);

    if (ra) {
        sb_append(&ua_s, ua, (size_t)(ra - ua));
        sb_append_str(&ra_s, ra + 1);
    } else {
        sb_append_str(&ua_s, ua);
    }
    if (rb) {
        sb_append(&ub_s, ub, (size_t)(rb - ub));
        sb_append_str(&rb_s, rb + 1);
    } else {
        sb_append_str(&ub_s, ub);
    }

    int rc = verrevcmp(ua_s.s, ub_s.s);
    if (rc == 0)
        rc = verrevcmp(ra_s.s, rb_s.s);

    sb_free(&ua_s); sb_free(&ub_s); sb_free(&ra_s); sb_free(&rb_s);
    return rc;
}

/* ---------------- platform: exe dir, user dir, subprocess ---------------- */

const char *exe_dir(void)
{
#ifdef _WIN32
    static char buf[MAX_PATH + 2];
    if (!buf[0]) {
        DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return ".";
        /* strip the exe name */
        char *slash = strrchr(buf, '\\');
        char *fslash = strrchr(buf, '/');
        char *last = fslash > slash ? fslash : slash;
        if (last)
            last[1] = '\0';
        else
            snprintf(buf, sizeof buf, ".\\");
    }
    return buf;
#else
    return "/"; /* Linux config paths are fixed under /etc and /var */
#endif
}

const char *user_dir(void)
{
#ifdef _WIN32
    static char buf[MAX_PATH];
    if (!buf[0]) {
        DWORD n = GetEnvironmentVariableA("USERPROFILE", buf, sizeof buf);
        if (n == 0 || n >= sizeof buf)
            return ".";
    }
    return buf;
#else
    const char *h = getenv("HOME");
    return h && *h ? h : ".";
#endif
}

#ifdef _WIN32

/* Quote every argument for cmd.exe: wrap in double quotes, escape embedded
   double quotes. Returns malloc'd command line. */
static char *build_cmdline(const char *const argv[])
{
    sb_t b;
    sb_init(&b);
    int i;
    for (i = 0; argv[i]; i++) {
        if (i)
            sb_append_char(&b, ' ');
        sb_append_char(&b, '"');
        const char *p = argv[i];
        while (*p) {
            if (*p == '"')
                sb_append_str(&b, "\\\"");
            else
                sb_append_char(&b, *p);
            p++;
        }
        sb_append_char(&b, '"');
    }
    return sb_detach(&b);
}

int plat_run_capture(const char *const argv[], sb_t *out)
{
    char *cmd = build_cmdline(argv);
    FILE *p = _popen(cmd, "r");
    if (!p) {
        fprintf(stderr, "reposync: popen: %s\n", strerror(errno));
        free(cmd);
        return 127;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) {
        if (sb_append(out, buf, n) < 0) {
            _pclose(p);
            free(cmd);
            return 127;
        }
    }
    int rc = _pclose(p);
    free(cmd);
    return rc == -1 ? 128 : rc;
}

int plat_run_simple(const char *const argv[])
{
    char *cmd = build_cmdline(argv);
    int rc = system(cmd);
    free(cmd);
    if (rc == -1)
        return 128;
    /* system() returns the cmd.exe status; exit code is the low 8 bits
       (cmd.exe /C convention). */
    return (rc >> 8) & 0xff;
}

#else /* POSIX */

int plat_run_capture(const char *const argv[], sb_t *b)
{
    int fds[2];
    if (pipe(fds) < 0) {
        perror("reposync: pipe");
        return 127;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("reposync: fork");
        close(fds[0]);
        close(fds[1]);
        return 127;
    }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(fds[1]);
        execvp((char *)argv[0], (char *const *)argv);
        fprintf(stderr, "reposync: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(fds[1]);
    char buf[65536];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) {
        if (sb_append(b, buf, (size_t)n) < 0) {
            close(fds[0]);
            int st = 0;
            waitpid(pid, &st, 0);
            return 127;
        }
    }
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return 128;
}

int plat_run_simple(const char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("reposync: fork");
        return 127;
    }
    if (pid == 0) {
        execvp((char *)argv[0], (char *const *)argv);
        fprintf(stderr, "reposync: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return 128;
}

#endif
