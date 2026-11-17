#ifndef AUTH_BLE_H
#define AUTH_BLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "fido_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// 回调运行在 NimBLE 上下文，只投递应用事件，不在这里操作 LVGL 或执行慢操作。
typedef void (*auth_ble_req_cb_t)(const fido_parsed_cmd_t *cmd);

// 初始化单连接外设、NimBLE 与 FIDO GATT；失败时不应继续启动另一个射频栈。
bool auth_ble_init(auth_ble_req_cb_t on_req);

// 启动实验性 FT_FIDO 广播；实际启动仍由协议栈同步状态决定。
void auth_ble_start_advertising(void);

// 向已连接并订阅的 companion 客户端发送文本响应。
bool auth_ble_send_response(const char *response_str);

// 应用任务提交用户在设备上的确认/拒绝，不把按键存在当作用户验证。
void auth_ble_fido_user_confirm(bool approved);

// 处理 keep-alive 与请求超时；停止协议栈前必须等本次发送退出。
void auth_ble_tick(void);

// 区分可用、已连接以及正在停止的状态，不能只根据 Lab 偏好推断运行状态。
bool auth_ble_is_enabled(void);
bool auth_ble_is_connected(void);

// 等待 host 退出并释放控制器；返回 false 时资源仍由本模块持有，不能覆盖重建。
bool auth_ble_stop(void);

// 擦除后仅使主密钥缓存失效，下一次合法操作再加载/生成。
void auth_ble_reload_seed(void);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_BLE_H */
