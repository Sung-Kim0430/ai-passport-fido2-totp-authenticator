#ifndef AUTH_WIFI_H
#define AUTH_WIFI_H

#include <stdbool.h>
#include <stdint.h>
#include "auth_storage.h"
#include "auth_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "fido_protocol.h"

// HTTP 任务只投递命令；返回值表示是否入队，实际结果由应用任务回报。
typedef bool (*auth_wifi_cmd_cb_t)(const fido_parsed_cmd_t *cmd);

// 发布不含密钥的元数据快照，避免 HTTP 任务直接遍历正在修改的账号配置。
void auth_wifi_publish_config(const auth_config_t *cfg, bool unlocked);
void auth_wifi_command_result(uint32_t request_id, bool ok);
void auth_wifi_history_service(bool unlocked);

// 芯片身份只读初始化一次，不烧写 OTP/eFuse；返回数据由模块长期持有。
bool auth_wifi_init_identity(void);
const auth_identity_t *auth_wifi_get_identity(void);

// 启动设备专属热点、HTTP 与 DNS；中途失败按所有权回滚，先确保 BLE 已停止。
bool auth_wifi_ap_start(auth_config_t *cfg, auth_wifi_cmd_cb_t on_cmd);

// DNS 任务自行关闭 socket，等待其退出后再释放服务与驱动。
bool auth_wifi_ap_stop(void);

// 表示仍有热点相关资源；停止失败时也为 true，防止误启 BLE 或重复初始化。
bool auth_wifi_ap_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_WIFI_H */
