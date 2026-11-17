// 启动按键守卫的纯逻辑测试：只有整板空闲才能结束隔离，时间和抬起均不能撤防。
#include <assert.h>
#include <stdio.h>

#include "key_guard.h"

int main(void) {
    key_guard_t g = {0};

    assert(!key_guard_consume(&g, false));
    assert(key_guard_consume(&g, true));
    assert(!g.armed);

    key_guard_arm(&g);
    assert(g.armed);
    // 按下、长按、抬起、延迟单击/双击及第二次按下都不是整板空闲。
    for (int event = 0; event < 100; ++event) {
        assert(key_guard_consume(&g, false));
        assert(g.armed);
    }
    assert(key_guard_consume(&g, true));
    assert(!g.armed);
    assert(!key_guard_consume(&g, false));

    // 后续空闲通知本身始终被吞掉，但不影响下一次独立手势。
    assert(key_guard_consume(&g, true));
    assert(!key_guard_consume(&g, false));

    // 每次启动均重新武装；初始化前就松开的情况同样等待定时器确认空闲。
    key_guard_arm(&g);
    assert(key_guard_consume(&g, true));
    assert(!g.armed);
    assert(!key_guard_consume(&g, false));

    puts("key wake guard state-machine tests: PASS");
    return 0;
}
