/*
 * NES 模拟器适配层 —— 把 nofrendo 接到 display.c 上
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* 抢在 display_init() 之前把 NES 视频缓冲（65 KB）从内部 SRAM 里挖出来。
 *
 * 顺序很要紧：两块帧缓冲一旦先分配，剩下的连续内部内存就凑不出 65 KB 了。
 * 而这块缓冲是整个模拟里最热的内存，落到 PSRAM 上 PPU 渲染会慢好几倍。 */
esp_err_t nes_emu_prealloc(void);

/* 初始化模拟器并开始跑指定的 ROM。正常情况下不返回。
 * 调用前必须先 nes_emu_prealloc() 和 display_init()。
 *
 * rom/rom_size 指向 iNES 数据，通常来自 rom_menu_pick()（flash mmap，
 * 零拷贝）。传 NULL 则用编译期嵌进固件的那个 —— roms 分区还没烧过时的回退。
 *
 * ROM 数据必须在整个运行期间保持有效：nofrendo 的 rom_loadmem 只存指针，
 * 不拷贝。mmap 出来的和 EMBED_FILES 嵌入的都满足这点。 */
esp_err_t nes_emu_run(const uint8_t *rom, size_t rom_size, const char *name);
