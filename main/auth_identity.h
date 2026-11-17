#ifndef AUTH_IDENTITY_H
#define AUTH_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

/* 使用固定长度存储，初始化后只读，避免热点启停时重复分配字符串。 */
typedef struct {
    char device_id[13]; /* 完整 MAC 的十六进制设备标识。 */
    char ssid[33];      /* 标准 SSID 容量；当前仅显示 Passport 与设备后六位。 */
    char password[9];   /* 8 位便利密码和结尾零，不包含在 SSID 中。 */
} auth_identity_t;

bool auth_identity_from_chip_id(const uint8_t chip_id[6], auth_identity_t *identity);

#endif
