#pragma once

/*
 * ESP32-S3 CPU 超频实验：直接下发 BBPLL 模拟微调寄存器（I2C_BBPLL_OC_DIV_7_0），
 * 绕过 Kconfig CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240 的 240MHz 上限。
 *
 * 这寄存器没有官方文档，Espressif 只在 rtc_clk.c / clk_tree_ll.h 的注释里提过，
 * 效果因芯片个体而异，所以不给档位标定固定 MHz —— 每次下发后现场用 RTC 时钟
 * （不受此寄存器影响）反推实测主频。参考实现：retro-go 的
 * components/retro-go/rg_system.c: rg_system_set_overclock()（S3 分支）。
 *
 * 已知不确定的地方（板子实测会给出答案，别先臆测）：
 *   - level 的正负方向哪边是升频，retro-go 自己也没写死，靠返回值判断。
 *   - CPU 之外，APB / SPI（显示）/ I2S（音频）是否共享同一路 BBPLL、是否跟着
 *     一起偏移，S3 上未经验证 —— 如果偏移，画面或声音的时序可能先于 CPU 死机
 *     暴露出来，这也是要观察的现象之一。
 */

// level 范围 [-8, 8]（retro-go 在 S3 上用的经验区间）。0 表示不改寄存器，仅测量
// 当前实际主频做基线核对。越界直接跳过、返回 0。
// 返回值：现场实测的 CPU 主频（MHz）；越界或本函数在其他目标上被误调用时返回 0。
int overclock_apply(int level);
