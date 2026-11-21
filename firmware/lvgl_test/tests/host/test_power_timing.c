/*
 * test_power_timing.c —— R2c-fix host regression (G0 唤醒后立刻息屏)
 *
 * 编译 board_power.c 的真实 power 状态机（PROTORIG_HOST_TEST + 最小 stub），
 * 复现 G0 时序：
 *   - 息屏期间 svc_power_tick 不跑 → s_now_ms 冻结；
 *   - 唤醒后第一拍必须把 idle 计时锚到真实 now，否则 1 tick 内又被息屏。
 *
 * 场景（Lead 指定）：
 *   screen_on @t=1000（先 tick 到 1000、熄屏、再唤醒）
 *   tick @t=20000 仍 on（修复前：19000ms > 15s -> off）
 *   tick @t=21000 仍 on
 *   tick @t=37000 off（唤醒后 17 s）
 */
#include <stdio.h>
#include <string.h>

#include "board_power.h"

static int s_failures;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        s_failures++;
    }
}

int main(void)
{
    printf("power timing host test (R2c-fix)\n");

    /* 1. 正常路径：tick 到 1000 后熄屏，模拟睡眠期间无 tick，再唤醒 */
    power_init();
    power_set_idle_ms(15000);
    svc_power_tick(1000);
    power_screen_off();
    check(!power_is_on(), "screen off at t=1000");

    /* 睡眠期间 power_note_touch 可能被调用；它拿到的是冻结的旧时间 */
    power_note_touch();

    power_screen_on();                  /* s_now_ms 仍 = 1000（睡眠中冻结） */
    check(power_is_on(), "wake at frozen t=1000");

    /* 修复点：唤醒后第一拍用真实 now 对齐，不能被 19 s 睡眠判超时 */
    svc_power_tick(20000);
    check(power_is_on(), "t=20000 still on (wake aligned, was 19000 ms stale)");
    check(power_last_on_ms() == 20000, "last_on_ms aligned to 20000");

    svc_power_tick(21000);
    check(power_is_on(), "t=21000 still on");

    svc_power_tick(37000);
    check(!power_is_on(), "t=37000 off (17 s after wake > 15 s idle)");

    /* 2. wake_redraw 请求路径：唤醒后第一拍仍要能取到整屏重画请求 */
    power_set_wake_redraw(1);
    svc_power_tick(40000);
    power_screen_off();
    power_screen_on();
    svc_power_tick(50000);
    check(power_is_on(), "wake with wake_redraw still on after tick");
    check(power_take_full_redraw() == 1, "full redraw request preserved");
    check(power_take_full_redraw() == 0, "full redraw request cleared after take");
    power_set_wake_redraw(0);

    /* 3. 连续两次睡眠/唤醒，idle 锚点都要更新 */
    svc_power_tick(60000);
    power_screen_off();
    power_screen_on();
    svc_power_tick(90000);
    check(power_is_on(), "second wake aligned (t=90000)");
    svc_power_tick(104000);
    check(power_is_on(), "second wake +14 s still on");
    svc_power_tick(105500);
    check(!power_is_on(), "second wake +15.5 s off");

    printf("%s (%d failure%s)\n",
           s_failures ? "FAILED" : "PASSED",
           s_failures, s_failures == 1 ? "" : "s");
    return s_failures ? 1 : 0;
}
