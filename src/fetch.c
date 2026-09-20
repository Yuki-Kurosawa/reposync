#include "fetch.h"
#include "util.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <zlib.h>
#include <zstd.h>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

/* Downloading is done through libcurl (linked in) — no curl / curl.exe
   binary is required on any platform. */

static size_t mem_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t n = size * nmemb;
    if (sb_append((sb_t *)userdata, ptr, n) < 0)
        return 0; /* abort the transfer */
    return n;
}

static CURL *easy_init(const char *url, long timeout)
{
    CURL *c = curl_easy_init();
    if (!c)
        return NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout);
    return c;
}

int fetch_url_to_mem(const char *url, sb_t *b)
{
    CURL *c = easy_init(url, 300);
    if (!c)
        return -1;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, b);
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        fprintf(stderr, "reposync: fetch %s: %s\n", url,
                curl_easy_strerror(rc));
        return -1;
    }
    if (status >= 400) {
        fprintf(stderr, "reposync: fetch %s: HTTP %ld\n", url, status);
        return -1;
    }
    return 0;
}

static size_t file_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    FILE *f = (FILE *)userdata;
    return fwrite(ptr, size, nmemb, f);
}

int fetch_url_to_file(const char *url, const char *path)
{
    sb_t tmp;
    sb_init(&tmp);
    sb_printf(&tmp, "%s.tmp.%ld", path, (long)getpid());
    FILE *f = fopen(tmp.s, "wb");
    if (!f) {
        fprintf(stderr, "reposync: cannot open %s: %s\n", tmp.s,
                strerror(errno));
        sb_free(&tmp);
        return -1;
    }
    CURL *c = easy_init(url, 600);
    if (!c) {
        fclose(f);
        unlink(tmp.s);
        sb_free(&tmp);
        return -1;
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, file_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    int err = ferror(f);
    fclose(f);
    if (rc != CURLE_OK) {
        fprintf(stderr, "reposync: fetch %s: %s\n", url,
                curl_easy_strerror(rc));
        unlink(tmp.s);
        sb_free(&tmp);
        return -1;
    }
    if (err || status >= 400) {
        if (status >= 400)
            fprintf(stderr, "reposync: fetch %s: HTTP %ld\n", url, status);
        unlink(tmp.s);
        sb_free(&tmp);
        return -1;
    }
    if (rename(tmp.s, path) < 0) {
        perror("reposync: rename");
        unlink(tmp.s);
        sb_free(&tmp);
        return -1;
    }
    sb_free(&tmp);
    return 0;
}

static int gunzip_mem(const unsigned char *in, size_t inlen, sb_t *out)
{
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, 15 + 32) != Z_OK)
        return -1;
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)(inlen > 0xFFFFFFFFu ? 0xFFFFFFFFu : inlen);
    unsigned char buf[65536];
    int ret = Z_OK;
    do {
        zs.next_out = buf;
        zs.avail_out = sizeof buf;
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            return -1;
        }
        size_t got = sizeof buf - zs.avail_out;
        if (got && sb_append(out, (const char *)buf, got) < 0) {
            inflateEnd(&zs);
            return -1;
        }
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return 0;
}

static int zstd_mem(const unsigned char *in, size_t inlen, sb_t *out)
{
    ZSTD_DCtx *d = ZSTD_createDCtx();
    if (!d)
        return -1;
    ZSTD_inBuffer ib = { in, inlen, 0 };
    unsigned char buf[65536];
    int rc = -1;
    int ended = 0;
    while (ib.pos < ib.size) {
        ZSTD_outBuffer ob = { buf, sizeof buf, 0 };
        size_t r = ZSTD_decompressStream(d, &ob, &ib);
        if (ZSTD_isError(r)) {
            fprintf(stderr, "reposync: zstd: %s\n", ZSTD_getErrorName(r));
            goto done;
        }
        if (ob.pos && sb_append(out, (const char *)buf, ob.pos) < 0)
            goto done;
        if (r == 0) {
            ended = 1;
            break;
        }
    }
    rc = 0;
    (void)ended;
done:
    ZSTD_freeDCtx(d);
    return rc;
}

int decompress_mem(const char *data, size_t len, const char *fmt, sb_t *out)
{
    if (strcmp(fmt, "gz") == 0)
        return gunzip_mem((const unsigned char *)data, len, out);
    if (strcmp(fmt, "zst") == 0)
        return zstd_mem((const unsigned char *)data, len, out);
    /* plain */
    return sb_append(out, data, len);
}
