/*
 * Game Boy / Game Boy Color 模拟器宿主适配层。
 *
 * 核心来自 components/gnuboy；这里只负责接本项目的条带推屏、手柄和 I2S。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* ROM 必须是完整的 .gb/.gbc 数据，通常直接指向 roms 分区的 mmap 地址。 */
esp_err_t gbc_emu_run(const uint8_t *rom, size_t rom_size, const char *name);
