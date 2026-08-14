#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "rom_store.h"

/* 运行一个 SNES ROM（.sfc/.smc），正常情况下不返回。
 * entry 的 ROM 会直接解压/复制进最终 PSRAM 缓冲，因为 snes9x 的内存映射
 * 会就地改写 ROM 头部区域。launch_keys 是菜单确认瞬间捕获的 X/Y 状态。
 *
 * ⚠ 这一层目前是可行性验证版：单帧缓冲 + 同步推屏，L/R 暂无实体键。
 * SMW 的最近一份即时状态由宿主层额外持久化，见 snes_save.c。
 *   先看帧率能不能到可玩区间，再决定要不要按 NES/GB 那样做完整集成。 */
esp_err_t snes_emu_run(const rom_store_entry_t *entry, uint16_t launch_keys);
