/*
 * motor.h —— 振动马达（U4 = LCM0720A3134F，V1.5 起沿用；IO30 -> Q2 低边驱动）
 *
 * 硬件事实见 motor.c 顶部注释。对外只暴露三件事：
 *   · motor_init()  上电初始化成"停"（IO30 低，Q2 关）
 *   · motor_set(pct) 占空比调速 0..100（0 = 停）
 *   · motor_pulse()  阻塞脉冲，只给 shell 调试用
 */
#ifndef _MOTOR_H_
#define _MOTOR_H_

#include <stdint.h>

/* 上电初始化：IO30 输出低（= 马达停）。可以重复调用。 */
void motor_init(void);

/* 纯 GPIO 开关（不用 PWM）：1 = 满速，0 = 停。
 * 用途是"最朴素的通路验证" —— 如果这个都不转，PWM 怎么样都不会转。 */
void motor_gpio(int on);

/* PWM 占空比 0..100，0 = 停。内部按需初始化 PWM。 */
void motor_set(int pct);

/* 当前占空比（-1 = 还没设过） */
int motor_pct(void);

/* 当前 PWM 频率（Hz） */
uint32_t motor_pwm_hz(void);

/* 改 PWM 频率（Hz），会重建阈值；返回实际生效值 */
uint32_t motor_set_pwm_hz(uint32_t hz);

/* 阻塞脉冲：按 pct 占空比震 ms 毫秒，然后停。只给 shell 调试用，
 * 不要在 LVGL 任务里调（会卡住渲染）。 */
void motor_pulse(int pct, uint32_t ms);

/* ------------------------------------------------------------------ */
/* 上板自检（2026-10-17：电机完全不转，用来分清是 MCU 没驱动还是板子/马达的问题）*/
/* ------------------------------------------------------------------ */

/* 读 IO30 的输入值（GLB_GPIO_CFG0 的 I 位，bit28）。
 * 注意：只有 IE=1 时这个位才是真的脚电平；GPIO 输出模式要额外开 GPIO_INPUT。 */
int motor_pad_read(void);

/* 把 CFG0 寄存器 + 三种状态（PWM 0% / PWM 100% / GPIO 高 / GPIO 低）下的
 * I 位一次打出来。I 位跟着变 → MCU 这一侧是好的，问题在 Q2/马达/焊点。 */
void motor_diag(void);

/* 慢速方波（给万用表/示波器看）：hz 默认 2，ms 总时长默认 10000 */
void motor_blink(uint32_t hz, uint32_t ms);

/* 换 PWM 通道（0..3）—— 手册说 IO30 = PWM2，但硅片映射要现场验证 */
int motor_set_channel(int ch);
int motor_channel(void);

/* "脚上挂东西了吗"：输出高 5 ms 后切输入+下拉，看节点电荷是否保持得住。
 * 保持 = 后面接着 Q2 栅极（有 ~500pF）；立刻掉 = 这根脚是悬空的。 */
int motor_padtest(void);

/* PWM 外设寄存器快照（证明外设跑没跑） */
void motor_dump_regs(void);

/* padtest 的进阶版：区分「MCU 没驱动」和「脚悬空」。
 * 返回 1 = 这条脚确实被驱动到了 3.3V（且后面没负载）。 */
int motor_padtest2(void);

/* 换马达脚（比如飞线到 IO21），通道按 脚号 % 4 自动换 */
int motor_set_pin(int n);
int motor_pin(void);

#endif /* _MOTOR_H_ */
