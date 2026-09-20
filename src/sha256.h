#ifndef REPOSYNC_SHA256_H
#define REPOSYNC_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, unsigned char out[32]);

/* one-shot convenience */
void sha256_hash(const void *data, size_t len, unsigned char out[32]);

/* hex string into out[65] */
void sha256_hex(const void *data, size_t len, char out[65]);

#endif
