/* 验证码算法接口：密钥由调用方提供，输出写入固定缓冲，不在这里持久化或返回密钥。
 * 扩展接口支持 SHA-1/256/512 与 Steam；普通 OTP 的位数和周期仍须在输入边界校验。
 * 输出缓冲建议使用 TOTP_MAX_CODE_LEN，剩余时间是当前周期内的倒计时，不是单调时钟。
 */
#ifndef TOTP_CRYPTO_H
#define TOTP_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOTP_SHA1_DIGEST_SIZE 20
#define TOTP_SHA1_BLOCK_SIZE  64
#define TOTP_MAX_CODE_LEN     16

typedef enum {
    TOTP_ALGO_SHA1 = 0,
    TOTP_ALGO_SHA256,
    TOTP_ALGO_SHA512,
    TOTP_ALGO_STEAM,
} totp_algorithm_t;

bool totp_generate_code_ex(const uint8_t *key, size_t key_len,
                           uint64_t timestamp, uint32_t period, uint8_t digits,
                           totp_algorithm_t algorithm, char *out, uint32_t *remaining);
bool totp_generate_from_base32_ex(const char *secret, uint64_t timestamp,
                                  uint32_t period, uint8_t digits,
                                  totp_algorithm_t algorithm, char *out, uint32_t *remaining);

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t  buffer[64];
} totp_sha1_ctx_t;

void totp_sha1_init(totp_sha1_ctx_t *ctx);
void totp_sha1_update(totp_sha1_ctx_t *ctx, const uint8_t *data, size_t len);
void totp_sha1_final(totp_sha1_ctx_t *ctx, uint8_t digest[TOTP_SHA1_DIGEST_SIZE]);

/**
 * @brief Computes HMAC-SHA1 according to RFC 2104.
 */
void totp_hmac_sha1(const uint8_t *key, size_t key_len,
                    const uint8_t *data, size_t data_len,
                    uint8_t out[TOTP_SHA1_DIGEST_SIZE]);

/**
 * @brief 按 RFC 4648 将 Base32 密钥解码到调用方的有界缓冲。
 * 接受大小写、空格、连字符和尾部填充 '='，不允许写出 out_max 边界。
 * @return 返回解码字节数，字符无效或容量不足时返回 -1。
 */
int totp_base32_decode(const char *encoded, uint8_t *out, size_t out_max);

/**
 * @brief 按 RFC 6238 生成带结尾零的 TOTP 字符串，保留前导零。
 * @param secret_key 二进制密钥，由调用方持有。
 * @param secret_len 密钥字节数。
 * @param current_timestamp UTC 秒级时间戳。
 * @param time_step 时间周期，通常为 30 秒。
 * @param digits 数字位数，通常为 6，最多为 8。
 * @param out_code 输出缓冲，至少具有 digits + 1 字节。
 * @param out_seconds_remaining 可选，接收当前周期剩余秒数。
 * @return 成功返回 true，输入无效或计算失败返回 false。
 */
bool totp_generate_code(const uint8_t *secret_key, size_t secret_len,
                        uint64_t current_timestamp,
                        uint32_t time_step,
                        uint8_t digits,
                        char *out_code,
                        uint32_t *out_seconds_remaining);

/**
 * @brief Generates a TOTP code directly from a Base32 secret string.
 */
bool totp_generate_from_base32(const char *base32_secret,
                              uint64_t current_timestamp,
                              uint32_t time_step,
                              uint8_t digits,
                              char *out_code,
                              uint32_t *out_seconds_remaining);

#ifdef __cplusplus
}
#endif

#endif /* TOTP_CRYPTO_H */
