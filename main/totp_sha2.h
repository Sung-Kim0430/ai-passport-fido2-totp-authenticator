#ifndef TOTP_SHA2_H
#define TOTP_SHA2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 复用固定大小的块缓冲计算 HMAC，不为每个验证码申请堆内存。
// sha512 为 false 输出 32 字节，为 true 输出 64 字节；调用方提供足够空间。
void totp_hmac_sha2(const uint8_t *key, size_t key_len,
                    const uint8_t *data, size_t data_len,
                    bool sha512, uint8_t *out);

#endif
