// main/key_guard.h —— 启动按键动作守卫，纯逻辑便于 host 测试。
// 每次启动都隔离已有手势，直到 BSP 确认所有手势结束且原始电压稳定松开。
// PRESS_UP 之后仍可能有延迟单击/双击，不能据此撤防；持续按住也不能超时放行。
#pragma once

#include <stdbool.h>

typedef struct {
    // 初始化按键前由启动任务武装，此后仅由按键定时器回调读写。
    bool armed;
} key_guard_t;

static inline void key_guard_arm(key_guard_t *g) {
    g->armed = true;
}

// 返回 true 表示吞掉事件；整板空闲通知即使在撤防后也不得派发给应用。
static inline bool key_guard_consume(key_guard_t *g, bool is_idle) {
    if (is_idle) {
        g->armed = false;
        return true;
    }
    return g->armed;
}
