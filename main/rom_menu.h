/*
 * 开机选游戏
 *
 * 从 roms 分区（见 rom_store.h）读列表，画在屏上，摇杆上下选，A/START 确认。
 * 选单只在开机时出现 —— 想换游戏按板子上的 RST 重启。
 *
 * 为什么不做游戏里热切换：nofrendo 没有「卸载卡再装另一张」的干净路径，
 * 硬来容易留下 PPU 状态残留或者内存泄漏。重启一次才两秒，不值得为它冒险。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rom_store.h"

/* 显示选单并阻塞，直到用户选定一个游戏。
 *
 * 选中的目录项通过 `*entry` 返回；模拟器再按条目决定是直接 mmap 还是解压。
 * `*launch_keys` 保存确认瞬间的完整 SNES 面键状态，避免大 ROM 解压期间用户
 * 已松开 X/Y，导致“恢复上一份/从头开始”修饰键丢失。
 *
 * 返回 false 表示选单没法用（roms 分区没烧过、镜像坏了、或者只有一个游戏
 * 没必要选）—— 这时出参不变，调用方应当用编译期嵌入的那个 ROM。
 *
 * 前置条件：display_init() 已经成功。输入初始化在函数内部做（幂等，
 * nes_emu_run() 之后再调一次没关系）。 */
bool rom_menu_pick(const rom_store_entry_t **entry, uint16_t *launch_keys);
