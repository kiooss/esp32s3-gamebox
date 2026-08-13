#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* 运行一个 SNES ROM（.sfc/.smc），正常情况下不返回。
 * rom 指向 mmap 的 flash 或任意只读内存；内部会拷进 PSRAM，
 * 因为 snes9x 的内存映射会就地改写 ROM 头部区域。
 *
 * ⚠ 这一层目前是可行性验证版：单帧缓冲 + 同步推屏，L/R 暂无实体键。
 * SMW 的最近一份即时状态由宿主层额外持久化，见 snes_save.c。
 *   先看帧率能不能到可玩区间，再决定要不要按 NES/GB 那样做完整集成。 */
esp_err_t snes_emu_run(const uint8_t *rom, size_t rom_size, const char *name);
