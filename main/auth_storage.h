#ifndef AUTH_STORAGE_H
#define AUTH_STORAGE_H

#include <stdbool.h>
#include <stdint.h>
#include "rtc_clock.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_MAX_ACCOUNTS 16
#define AUTH_ISSUER_MAX_LEN 32
#define AUTH_ACCOUNT_MAX_LEN 32
#define AUTH_SECRET_MAX_LEN 128

// 显示偏好独立保存为一个 NVS 字节，不改变账号配置 blob 的布局。
typedef enum {
    AUTH_TOTP_VIEW_CARD = 0,
    AUTH_TOTP_VIEW_LIST3 = 3,
    AUTH_TOTP_VIEW_LIST4 = 4,
} auth_totp_view_t;

// 字符串固定容量且包含结尾零；algorithm 取 totp_algorithm_t 对应值。
typedef struct {
    char issuer[AUTH_ISSUER_MAX_LEN];
    char account[AUTH_ACCOUNT_MAX_LEN];
    char secret[AUTH_SECRET_MAX_LEN];
    uint8_t digits;
    uint8_t period;
    uint8_t algorithm;
} auth_totp_account_t;

typedef struct {
    uint8_t brightness;
    bool sound_enabled;
    bool auto_wipe_enabled;
    uint32_t passcode_pattern;
    uint8_t passcode_len;
    int64_t unix_time_offset;
    uint16_t auto_sleep_sec;
    int8_t timezone_offset;
    bool time_format_24h;
    bool battery_protect_80;
    uint8_t account_count;
    auth_totp_account_t accounts[AUTH_MAX_ACCOUNTS];
} auth_config_t;

// 实验功能使用独立键，避免结构体尾部填充导致旧配置迁移失真。
typedef struct {
    bool ble_fido_enabled;
    bool console_log_enabled;
} auth_lab_config_t;

// 初始化或迁移配置；异常时返回失败并保留原数据，不自动擦除整片 NVS。
bool auth_storage_init(auth_config_t *cfg);
bool auth_storage_save(const auth_config_t *cfg);
uint32_t auth_storage_account_revision(void);

// 只保存完成的错误提交；启动不写，成功解锁或完成自动擦除后才清零。
bool auth_storage_pin_failures_load(uint8_t *failures);
bool auth_storage_pin_failures_save(uint8_t failures);
bool auth_storage_totp_view_load(auth_totp_view_t *view);
bool auth_storage_totp_view_save(auth_totp_view_t view);

// 元数据修改成功才推进版本；调用方借版本拒绝旧列表索引上的误操作。
bool auth_storage_add_account(auth_config_t *cfg, const char *issuer,
                              const char *account, const char *secret);
bool auth_storage_add_account_ex(auth_config_t *cfg, const auth_totp_account_t *account);
bool auth_storage_update_account(auth_config_t *cfg, uint8_t index,
                                 const auth_totp_account_t *account);
bool auth_storage_remove_account(auth_config_t *cfg, uint8_t index);
bool auth_storage_wipe_all(auth_config_t *cfg);

// 运行时间使用 SDK RTC/HRT；未校时返回 0，深睡可保留已校时状态。
void auth_time_init(bool deep_sleep_wakeup);
bool auth_time_is_set(void);
uint64_t auth_get_current_timestamp(const auth_config_t *cfg);

// 校时更新运行时与 RTC，不为每次校时重写账号 blob。
bool auth_set_current_timestamp(auth_config_t *cfg, uint64_t unix_ts);
bool auth_set_reference_timestamp(auth_config_t *cfg, const rtc_clock_reference_t *reference);

#define AUTH_MAX_FIDO_RPS 8
#define AUTH_FIDO_RP_KEY_MAX 16

// 当前实验凭据模型以 RP 键为单位；不在显示列表中暴露主密钥或盐值。
bool auth_storage_lab_load(auth_lab_config_t *lab);
bool auth_storage_lab_save(const auth_lab_config_t *lab);
uint8_t auth_storage_fido_list(char rps[][AUTH_FIDO_RP_KEY_MAX], uint8_t max_rps);
bool auth_storage_fido_delete(const char *rp_id_key);
bool auth_storage_fido_reset_all(void);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_STORAGE_H */
