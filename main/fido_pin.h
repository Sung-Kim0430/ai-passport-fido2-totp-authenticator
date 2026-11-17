#ifndef FIDO_PIN_H
#define FIDO_PIN_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// clientPIN 协议 1/2 的状态与密钥协商；这是实验实现，不等同认证或完整兼容。
void fido_pin_init(void);
bool fido_pin_is_set(void);
uint8_t fido_pin_retries_left(void);

// 输入为 clientPIN 请求 CBOR；输出含 CTAP 状态字节，容量不足或解析失败不能写越界。
size_t fido_pin_handle_client_pin(const uint8_t *payload, size_t len,
                                  uint8_t *out, size_t max_out);

// 按 token 的协商协议检查 pinUvAuthParam，不伪造 UV 成功。
bool fido_pin_verify_pin_auth(const uint8_t client_data_hash[32],
                              const uint8_t *pin_uv_auth_param,
                              size_t pin_uv_auth_param_len);

// 断开/停止传输时使 token 失效；工厂重置只能在设备确认流程后调用。
void fido_pin_reset_token(void);
void fido_pin_factory_reset(void);

#endif /* FIDO_PIN_H */
