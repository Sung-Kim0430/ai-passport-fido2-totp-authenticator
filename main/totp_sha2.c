/* SHA-256/SHA-512 与扩展 OTP：消息调度使用 16 槽循环数组，限制任务栈占用。
 * SHA-256 对中间量进行 32 位约束；Steam 使用固定字母表输出五字符令牌。
 */
#include "totp_sha2.h"
#include "totp_crypto.h"
#include <stdio.h>
#include <string.h>

static const uint64_t k256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static const uint64_t k512[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

typedef struct {
    uint64_t state[8];
    uint64_t bytes;
    size_t used;
    bool sha512;
    uint8_t block[128];
} sha2_ctx_t;

static uint64_t rotate(uint64_t x, unsigned n, unsigned width)
{
    return ((x >> n) | (x << (width - n))) & (width == 64 ? UINT64_MAX : UINT32_MAX);
}

static void transform(sha2_ctx_t *ctx)
{
    unsigned width = ctx->sha512 ? 64 : 32;
    unsigned word = width / 8;
    uint64_t mask = ctx->sha512 ? UINT64_MAX : UINT32_MAX;
    uint64_t w[16];
    for (unsigned i = 0; i < 16; i++) {
        w[i] = 0;
        for (unsigned j = 0; j < word; j++) w[i] = (w[i] << 8) | ctx->block[i * word + j];
    }
    uint64_t a=ctx->state[0], b=ctx->state[1], c=ctx->state[2], d=ctx->state[3];
    uint64_t e=ctx->state[4], f=ctx->state[5], g=ctx->state[6], h=ctx->state[7];
    for (unsigned i = 0; i < (ctx->sha512 ? 80U : 64U); i++) {
        if (i >= 16) {
            uint64_t x = w[(i + 1) & 15], y = w[(i + 14) & 15];
            uint64_t s0 = rotate(x, ctx->sha512 ? 1 : 7, width) ^ rotate(x, ctx->sha512 ? 8 : 18, width) ^ (x >> (ctx->sha512 ? 7 : 3));
            uint64_t s1 = rotate(y, ctx->sha512 ? 19 : 17, width) ^ rotate(y, ctx->sha512 ? 61 : 19, width) ^ (y >> (ctx->sha512 ? 6 : 10));
            w[i & 15] = (w[i & 15] + s0 + w[(i + 9) & 15] + s1) & mask;
        }
        uint64_t s0 = rotate(a, ctx->sha512 ? 28 : 2, width) ^ rotate(a, ctx->sha512 ? 34 : 13, width) ^ rotate(a, ctx->sha512 ? 39 : 22, width);
        uint64_t s1 = rotate(e, ctx->sha512 ? 14 : 6, width) ^ rotate(e, ctx->sha512 ? 18 : 11, width) ^ rotate(e, ctx->sha512 ? 41 : 25, width);
        uint64_t t1 = h + s1 + ((e & f) ^ (~e & g)) + (ctx->sha512 ? k512[i] : k256[i]) + w[i & 15];
        uint64_t t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
        h=g; g=f; f=e; e=(d+t1)&mask; d=c; c=b; b=a; a=(t1+t2)&mask;
    }
    const uint64_t state[8] = {a,b,c,d,e,f,g,h};
    for (unsigned i = 0; i < 8; i++) ctx->state[i] = (ctx->state[i] + state[i]) & mask;
}

static void init(sha2_ctx_t *ctx, bool sha512)
{
    static const uint64_t initial256[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    static const uint64_t initial512[8] = {0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
    memset(ctx, 0, sizeof(*ctx));
    ctx->sha512 = sha512;
    memcpy(ctx->state, sha512 ? initial512 : initial256, sizeof(ctx->state));
}

static void update(sha2_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t block_size = ctx->sha512 ? 128 : 64;
    ctx->bytes += len;
    while (len > 0) {
        size_t count = block_size - ctx->used;
        if (count > len) count = len;
        memcpy(ctx->block + ctx->used, data, count);
        ctx->used += count;
        data += count;
        len -= count;
        if (ctx->used == block_size) {
            transform(ctx);
            ctx->used = 0;
        }
    }
}

static void finish(sha2_ctx_t *ctx, uint8_t *out)
{
    size_t block_size = ctx->sha512 ? 128 : 64;
    size_t length_size = ctx->sha512 ? 16 : 8;
    uint64_t bytes = ctx->bytes;
    ctx->block[ctx->used++] = 0x80;
    if (ctx->used > block_size - length_size) {
        memset(ctx->block + ctx->used, 0, block_size - ctx->used);
        transform(ctx);
        ctx->used = 0;
    }
    memset(ctx->block + ctx->used, 0, block_size - ctx->used);
    for (unsigned i = 0; i < 8; i++) {
        ctx->block[block_size - 1 - i] = (uint8_t)((bytes << 3) >> (i * 8));
        if (ctx->sha512) ctx->block[block_size - 9 - i] = (uint8_t)((bytes >> 61) >> (i * 8));
    }
    transform(ctx);
    unsigned word = ctx->sha512 ? 8 : 4;
    for (unsigned i = 0; i < 8; i++) {
        for (unsigned j = 0; j < word; j++) out[i * word + j] = (uint8_t)(ctx->state[i] >> (8 * (word - 1 - j)));
    }
}

void totp_hmac_sha2(const uint8_t *key, size_t key_len, const uint8_t *data,
                     size_t data_len, bool sha512, uint8_t *out)
{
    size_t block_size = sha512 ? 128 : 64;
    size_t digest_size = sha512 ? 64 : 32;
    sha2_ctx_t ctx;
    uint8_t key_hash[64], inner[64], pad[128];
    if (key_len > block_size) {
        init(&ctx, sha512);
        update(&ctx, key, key_len);
        finish(&ctx, key_hash);
        key = key_hash;
        key_len = digest_size;
    }
    memset(pad, 0x36, block_size);
    for (size_t i = 0; i < key_len; i++) pad[i] ^= key[i];
    init(&ctx, sha512);
    update(&ctx, pad, block_size);
    update(&ctx, data, data_len);
    finish(&ctx, inner);
    memset(pad, 0x5c, block_size);
    for (size_t i = 0; i < key_len; i++) pad[i] ^= key[i];
    init(&ctx, sha512);
    update(&ctx, pad, block_size);
    update(&ctx, inner, digest_size);
    finish(&ctx, out);
    volatile uint8_t *p = (volatile uint8_t *)&ctx;
    for (size_t i = 0; i < sizeof(ctx); i++) p[i] = 0;
    p = pad;
    for (size_t i = 0; i < sizeof(pad); i++) p[i] = 0;
    p = key_hash;
    for (size_t i = 0; i < sizeof(key_hash); i++) p[i] = 0;
    p = inner;
    for (size_t i = 0; i < sizeof(inner); i++) p[i] = 0;
}

bool totp_generate_code_ex(const uint8_t *key, size_t key_len,
                           uint64_t timestamp, uint32_t period, uint8_t digits,
                           totp_algorithm_t algorithm, char *out, uint32_t *remaining)
{
    if (out) out[0] = '\0';
    if (!key || key_len == 0 || !out) return false;
    if (algorithm < TOTP_ALGO_SHA1 || algorithm > TOTP_ALGO_STEAM) return false;
    if (period == 0) period = 30;
    if (algorithm == TOTP_ALGO_STEAM) {
        if (period != 30 || digits != 5) return false;
    } else if (digits < 6 || digits > 8) {
        return false;
    }
    uint64_t counter = timestamp / period;
    uint8_t message[8], hash[64];
    for (int i = 7; i >= 0; i--) {
        message[i] = (uint8_t)counter;
        counter >>= 8;
    }
    size_t digest_len = 20;
    if (algorithm == TOTP_ALGO_SHA256 || algorithm == TOTP_ALGO_SHA512) {
        bool sha512 = algorithm == TOTP_ALGO_SHA512;
        digest_len = sha512 ? 64 : 32;
        totp_hmac_sha2(key, key_len, message, sizeof(message), sha512, hash);
    } else {
        totp_hmac_sha1(key, key_len, message, sizeof(message), hash);
    }
    size_t offset = hash[digest_len - 1] & 15;
    uint32_t value = ((uint32_t)(hash[offset] & 0x7f) << 24)
                   | ((uint32_t)hash[offset + 1] << 16)
                   | ((uint32_t)hash[offset + 2] << 8) | hash[offset + 3];
    if (algorithm == TOTP_ALGO_STEAM) {
        static const char alphabet[] = "23456789BCDFGHJKMNPQRTVWXY";
        for (unsigned i = 0; i < 5; i++) {
            out[i] = alphabet[value % 26];
            value /= 26;
        }
        out[5] = '\0';
    } else {
        uint32_t modulus = 1;
        for (uint8_t i = 0; i < digits; i++) modulus *= 10;
        snprintf(out, TOTP_MAX_CODE_LEN, "%0*u", (int)digits, (unsigned)(value % modulus));
    }
    if (remaining) *remaining = period - (uint32_t)(timestamp % period);
    volatile uint8_t *p = hash;
    for (size_t i = 0; i < sizeof(hash); i++) p[i] = 0;
    return true;
}

bool totp_generate_from_base32_ex(const char *secret, uint64_t timestamp,
                                  uint32_t period, uint8_t digits,
                                  totp_algorithm_t algorithm, char *out, uint32_t *remaining)
{
    if (out) out[0] = '\0';
    if (!secret || !out) return false;
    uint8_t key[80];
    int key_len = totp_base32_decode(secret, key, sizeof(key));
    bool ok = key_len > 0 && totp_generate_code_ex(key, (size_t)key_len, timestamp,
                                                 period, digits, algorithm, out, remaining);
    volatile uint8_t *p = key;
    for (size_t i = 0; i < sizeof(key); i++) p[i] = 0;
    return ok;
}
