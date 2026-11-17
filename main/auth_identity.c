#include "auth_identity.h"
#include "totp_crypto.h"
#include <stdio.h>
#include <string.h>

/* 仅从出厂 MAC 派生稳定身份，不写入 OTP/eFuse，也不把公开标识当成强随机密钥。 */
bool auth_identity_from_chip_id(const uint8_t chip_id[6], auth_identity_t *identity)
{
    if (!chip_id || !identity) return false;
    static const uint8_t context[] = "Passport-Auth WiFi v1";
    static const char hex[] = "0123456789ABCDEF";
    uint8_t digest[TOTP_SHA1_DIGEST_SIZE];
    memset(identity, 0, sizeof(*identity));
    for (size_t i = 0; i < 6; i++) {
        identity->device_id[i * 2] = hex[chip_id[i] >> 4];
        identity->device_id[i * 2 + 1] = hex[chip_id[i] & 15];
    }
    totp_hmac_sha1(context, sizeof(context) - 1, chip_id, 6, digest);
    for (size_t i = 0; i < 4; i++) {
        identity->password[i * 2] = hex[digest[i] >> 4];
        identity->password[i * 2 + 1] = hex[digest[i] & 15];
    }
    snprintf(identity->ssid, sizeof(identity->ssid), "Passport-%.6s",
             identity->device_id + 6);
    return true;
}
