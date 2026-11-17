/* SHA-1/HMAC、Base32 与基础 TOTP：使用固定大小栈缓冲，不逐次申请堆内存。
 * 动态截断按 RFC 4226/6238 保留 31 位，并保留验证码的前导零。
 */
#include "totp_crypto.h"
#include <string.h>
#include <stdio.h>

/* --- SHA-1 Implementation (FIPS 180-1) --- */

#define SHA1_ROL(val, bits) (((val) << (bits)) | ((val) >> (32 - (bits))))

static void sha1_transform(uint32_t state[5], const uint8_t buffer[64])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    uint32_t w[80];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)buffer[i * 4] << 24)
             | ((uint32_t)buffer[i * 4 + 1] << 16)
             | ((uint32_t)buffer[i * 4 + 2] << 8)
             | ((uint32_t)buffer[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = SHA1_ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    /* Round 1 */
    for (int i = 0; i < 20; i++) {
        uint32_t t = SHA1_ROL(a, 5) + ((b & c) | ((~b) & d)) + e + w[i] + 0x5A827999U;
        e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
    }
    /* Round 2 */
    for (int i = 20; i < 40; i++) {
        uint32_t t = SHA1_ROL(a, 5) + (b ^ c ^ d) + e + w[i] + 0x6ED9EBA1U;
        e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
    }
    /* Round 3 */
    for (int i = 40; i < 60; i++) {
        uint32_t t = SHA1_ROL(a, 5) + ((b & c) | (b & d) | (c & d)) + e + w[i] + 0x8F1BBCDCU;
        e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
    }
    /* Round 4 */
    for (int i = 60; i < 80; i++) {
        uint32_t t = SHA1_ROL(a, 5) + (b ^ c ^ d) + e + w[i] + 0xCA62C1D6U;
        e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void totp_sha1_init(totp_sha1_ctx_t *ctx)
{
    if (!ctx) return;
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xEFCDAB89U;
    ctx->state[2] = 0x98BADCFEU;
    ctx->state[3] = 0x10325476U;
    ctx->state[4] = 0xC3D2E1F0U;
    ctx->count[0] = 0;
    ctx->count[1] = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

void totp_sha1_update(totp_sha1_ctx_t *ctx, const uint8_t *data, size_t len)
{
    if (!ctx || !data || len == 0) return;

    size_t i = 0;
    size_t index = (ctx->count[0] >> 3) & 63;
    uint32_t bit_len = (uint32_t)(len << 3);

    if ((ctx->count[0] += bit_len) < bit_len) {
        ctx->count[1]++;
    }
    ctx->count[1] += (uint32_t)(len >> 29);

    size_t part_len = 64 - index;
    if (len >= part_len) {
        memcpy(&ctx->buffer[index], data, part_len);
        sha1_transform(ctx->state, ctx->buffer);
        for (i = part_len; i + 63 < len; i += 64) {
            sha1_transform(ctx->state, &data[i]);
        }
        index = 0;
    }
    memcpy(&ctx->buffer[index], &data[i], len - i);
}

void totp_sha1_final(totp_sha1_ctx_t *ctx, uint8_t digest[TOTP_SHA1_DIGEST_SIZE])
{
    if (!ctx || !digest) return;

    uint8_t final_count[8];
    for (int i = 0; i < 4; i++) {
        final_count[i] = (uint8_t)((ctx->count[1] >> ((3 - i) * 8)) & 255);
        final_count[i + 4] = (uint8_t)((ctx->count[0] >> ((3 - i) * 8)) & 255);
    }

    uint8_t pad = 0x80;
    totp_sha1_update(ctx, &pad, 1);
    uint8_t zero = 0x00;
    while ((ctx->count[0] & 504) != 448) {
        totp_sha1_update(ctx, &zero, 1);
    }
    totp_sha1_update(ctx, final_count, 8);

    for (int i = 0; i < 5; i++) {
        digest[i * 4]     = (uint8_t)((ctx->state[i] >> 24) & 255);
        digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 255);
        digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8)  & 255);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]         & 255);
    }
}

/* --- HMAC-SHA1 Implementation (RFC 2104) --- */

void totp_hmac_sha1(const uint8_t *key, size_t key_len,
                    const uint8_t *data, size_t data_len,
                    uint8_t out[TOTP_SHA1_DIGEST_SIZE])
{
    uint8_t k_pad[TOTP_SHA1_BLOCK_SIZE];
    uint8_t tk[TOTP_SHA1_DIGEST_SIZE];

    if (key_len > TOTP_SHA1_BLOCK_SIZE) {
        totp_sha1_ctx_t tctx;
        totp_sha1_init(&tctx);
        totp_sha1_update(&tctx, key, key_len);
        totp_sha1_final(&tctx, tk);
        key = tk;
        key_len = TOTP_SHA1_DIGEST_SIZE;
    }

    /* Inner hash */
    memset(k_pad, 0x36, sizeof(k_pad));
    for (size_t i = 0; i < key_len; i++) {
        k_pad[i] ^= key[i];
    }
    totp_sha1_ctx_t ictx;
    totp_sha1_init(&ictx);
    totp_sha1_update(&ictx, k_pad, TOTP_SHA1_BLOCK_SIZE);
    totp_sha1_update(&ictx, data, data_len);
    uint8_t inner_hash[TOTP_SHA1_DIGEST_SIZE];
    totp_sha1_final(&ictx, inner_hash);

    /* Outer hash */
    memset(k_pad, 0x5C, sizeof(k_pad));
    for (size_t i = 0; i < key_len; i++) {
        k_pad[i] ^= key[i];
    }
    totp_sha1_ctx_t octx;
    totp_sha1_init(&octx);
    totp_sha1_update(&octx, k_pad, TOTP_SHA1_BLOCK_SIZE);
    totp_sha1_update(&octx, inner_hash, TOTP_SHA1_DIGEST_SIZE);
    totp_sha1_final(&octx, out);
}

/* --- Base32 Decoding (RFC 4648) --- */

int totp_base32_decode(const char *encoded, uint8_t *out, size_t out_max)
{
    if (!encoded || !out) return -1;

    size_t out_len = 0;
    uint32_t buffer = 0;
    int bits_left = 0;

    for (const char *p = encoded; *p != '\0'; p++) {
        char c = *p;
        if (c == ' ' || c == '-' || c == '=') {
            continue; /* Skip formatting and padding */
        }
        int val;
        if (c >= 'A' && c <= 'Z') {
            val = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            val = c - 'a';
        } else if (c >= '2' && c <= '7') {
            val = c - '2' + 26;
        } else {
            return -1; /* Invalid Base32 character */
        }

        buffer = (buffer << 5) | (uint32_t)(val & 0x1F);
        bits_left += 5;

        if (bits_left >= 8) {
            bits_left -= 8;
            if (out_len >= out_max) {
                return -1; /* Buffer overflow */
            }
            out[out_len++] = (uint8_t)((buffer >> bits_left) & 0xFF);
        }
    }

    return (int)out_len;
}

/* --- TOTP Code Generation (RFC 6238 / RFC 4226) --- */

bool totp_generate_code(const uint8_t *secret_key, size_t secret_len,
                        uint64_t current_timestamp,
                        uint32_t time_step,
                        uint8_t digits,
                        char *out_code,
                        uint32_t *out_seconds_remaining)
{
    if (!secret_key || secret_len == 0 || !out_code) return false;
    if (time_step == 0) time_step = 30;
    if (digits < 6 || digits > 8) digits = 6;

    uint64_t step_count = current_timestamp / time_step;
    if (out_seconds_remaining) {
        *out_seconds_remaining = (uint32_t)(time_step - (current_timestamp % time_step));
    }

    /* 8-byte big-endian counter */
    uint8_t counter[8];
    for (int i = 7; i >= 0; i--) {
        counter[i] = (uint8_t)(step_count & 0xFF);
        step_count >>= 8;
    }

    uint8_t hash[TOTP_SHA1_DIGEST_SIZE];
    totp_hmac_sha1(secret_key, secret_len, counter, sizeof(counter), hash);

    /* Dynamic Truncation (RFC 4226 Section 5.4) */
    int offset = hash[19] & 0x0F;
    uint32_t binary = ((uint32_t)(hash[offset] & 0x7F) << 24)
                    | ((uint32_t)(hash[offset + 1] & 0xFF) << 16)
                    | ((uint32_t)(hash[offset + 2] & 0xFF) << 8)
                    | ((uint32_t)(hash[offset + 3] & 0xFF));

    uint32_t mod = 1;
    for (uint8_t i = 0; i < digits; i++) {
        mod *= 10;
    }
    uint32_t otp = binary % mod;

    if (digits == 6) {
        snprintf(out_code, TOTP_MAX_CODE_LEN, "%06u", (unsigned int)otp);
    } else if (digits == 7) {
        snprintf(out_code, TOTP_MAX_CODE_LEN, "%07u", (unsigned int)otp);
    } else {
        snprintf(out_code, TOTP_MAX_CODE_LEN, "%08u", (unsigned int)otp);
    }

    return true;
}

bool totp_generate_from_base32(const char *base32_secret,
                              uint64_t current_timestamp,
                              uint32_t time_step,
                              uint8_t digits,
                              char *out_code,
                              uint32_t *out_seconds_remaining)
{
    if (!base32_secret || !out_code) return false;
    uint8_t raw_key[64];
    int decoded_len = totp_base32_decode(base32_secret, raw_key, sizeof(raw_key));
    if (decoded_len <= 0) return false;

    return totp_generate_code(raw_key, (size_t)decoded_len,
                              current_timestamp, time_step, digits,
                              out_code, out_seconds_remaining);
}
