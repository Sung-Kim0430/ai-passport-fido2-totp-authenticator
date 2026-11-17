#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, ...) do { (void)(tag); if (0) fprintf(stderr, __VA_ARGS__); } while (0)
// 保留格式检查但不输出日志，避免恢复测试产生大量无关串口文本。
#define ESP_LOGI ESP_LOGE
#define ESP_LOGD ESP_LOGE
#define ESP_LOGW ESP_LOGE
