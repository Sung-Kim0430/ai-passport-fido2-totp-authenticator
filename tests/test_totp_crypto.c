#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "totp_crypto.h"

static void test_sha1_basic(void)
{
    /* Test vector: "abc" -> a9993e364706816aba3e25717850c26c9cd0d89d */
    totp_sha1_ctx_t ctx;
    totp_sha1_init(&ctx);
    totp_sha1_update(&ctx, (const uint8_t *)"abc", 3);
    uint8_t digest[TOTP_SHA1_DIGEST_SIZE];
    totp_sha1_final(&ctx, digest);

    const uint8_t expected[20] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d
    };
    assert(memcmp(digest, expected, 20) == 0);
    printf("SHA-1 basic test vector: PASS\n");
}

static void test_hmac_sha1_rfc2202(void)
{
    /* RFC 2202 Test Case 1 */
    /* Key = 20 bytes of 0x0b */
    /* Data = "Hi There" */
    /* Digest = 0xb617318655057264e28bc0b6fb378c8ef146be00 */
    uint8_t key[20];
    memset(key, 0x0b, 20);
    const char *data = "Hi There";

    uint8_t out[TOTP_SHA1_DIGEST_SIZE];
    totp_hmac_sha1(key, 20, (const uint8_t *)data, strlen(data), out);

    const uint8_t expected[20] = {
        0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xe2, 0x8b,
        0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e, 0xf1, 0x46, 0xbe, 0x00
    };
    assert(memcmp(out, expected, 20) == 0);
    printf("HMAC-SHA1 RFC 2202 test: PASS\n");
}

static void test_base32_decode(void)
{
    /* "JBSWY3DPEHPK3PXP" is Base32 for "Hello!\xde\xad\xbe\xef" */
    uint8_t buf[32];
    int len = totp_base32_decode("JBSWY3DPEHPK3PXP", buf, sizeof(buf));
    assert(len == 10);
    assert(memcmp(buf, "Hello!", 6) == 0);

    /* Test spacing, lower case, hyphens */
    uint8_t buf2[32];
    int len2 = totp_base32_decode("jbsw y3dp-ehpk 3pxp==", buf2, sizeof(buf2));
    assert(len2 == 10);
    assert(memcmp(buf, buf2, 10) == 0);

    printf("Base32 decoding test: PASS\n");
}

static void test_totp_rfc6238_vectors(void)
{
    /*
     * RFC 6238 Appendix B test vectors for SHA1:
     * Secret = "12345678901234567890" (20 bytes ASCII, Base32: GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ)
     * Time: 59s -> 94287082 (8 digits) or 287082 (6 digits)
     * Time: 1111111109s -> 07081804 (8 digits) or 081804 (6 digits)
     * Time: 1111111111s -> 14050471 (8 digits) or 050471 (6 digits)
     * Time: 1234567890s -> 89005924 (8 digits) or 005924 (6 digits)
     * Time: 2000000000s -> 69279037 (8 digits) or 279037 (6 digits)
     */
    const uint8_t secret[20] = "12345678901234567890";
    char code[16];
    uint32_t rem = 0;

    /* 59s: step = 1 */
    assert(totp_generate_code(secret, 20, 59, 30, 8, code, &rem));
    assert(strcmp(code, "94287082") == 0);
    assert(rem == 1);

    assert(totp_generate_code(secret, 20, 59, 30, 6, code, &rem));
    assert(strcmp(code, "287082") == 0);

    /* 1111111109s */
    assert(totp_generate_code(secret, 20, 1111111109ULL, 30, 8, code, &rem));
    assert(strcmp(code, "07081804") == 0);

    assert(totp_generate_code(secret, 20, 1111111109ULL, 30, 6, code, &rem));
    assert(strcmp(code, "081804") == 0);

    /* 1234567890s */
    assert(totp_generate_code(secret, 20, 1234567890ULL, 30, 6, code, &rem));
    assert(strcmp(code, "005924") == 0);

    /* 2000000000s */
    assert(totp_generate_code(secret, 20, 2000000000ULL, 30, 6, code, &rem));
    assert(strcmp(code, "279037") == 0);

    /* Test Base32 generator */
    const char *b32_secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
    assert(totp_generate_from_base32(b32_secret, 2000000000ULL, 30, 6, code, &rem));
    assert(strcmp(code, "279037") == 0);

    printf("TOTP RFC 6238 standard test vectors: PASS\n");
}

static void test_extended_algorithms(void)
{
    static const uint64_t times[] = {59, 1111111109ULL, 1111111111ULL, 1234567890ULL, 2000000000ULL, 20000000000ULL};
    static const char *sha256[] = {"46119246", "68084774", "67062674", "91819424", "90698825", "77737706"};
    static const char *sha512[] = {"90693936", "25091201", "99943326", "93441116", "38618901", "47863826"};
    const uint8_t key[] = "1234567890123456789012345678901234567890123456789012345678901234";
    char code[TOTP_MAX_CODE_LEN];
    uint32_t remaining;
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
        assert(totp_generate_code_ex(key, 32, times[i], 30, 8, TOTP_ALGO_SHA256, code, &remaining));
        assert(strcmp(code, sha256[i]) == 0);
        assert(totp_generate_code_ex(key, 64, times[i], 30, 8, TOTP_ALGO_SHA512, code, &remaining));
        assert(strcmp(code, sha512[i]) == 0);
    }
    static const uint64_t steam_times[] = {0, 59, 1111111109ULL, 20000000000ULL};
    static const char *steam[] = {"GG5F5", "PV9M4", "PY4YB", "R5DMB"};
    for (size_t i = 0; i < sizeof(steam_times) / sizeof(steam_times[0]); i++) {
        assert(totp_generate_code_ex(key, 20, steam_times[i], 30, 5, TOTP_ALGO_STEAM, code, &remaining));
        assert(strcmp(code, steam[i]) == 0);
    }
    assert(!totp_generate_code_ex(key, 20, 59, 30, 8, (totp_algorithm_t)99, code, NULL));
    puts("SHA-256/SHA-512 RFC 6238 and Steam vectors: PASS");
}

int main(void)
{
    test_extended_algorithms();
    test_sha1_basic();
    test_hmac_sha1_rfc2202();
    test_base32_decode();
    test_totp_rfc6238_vectors();
    printf("All TOTP crypto host tests: PASS\n");
    return 0;
}
