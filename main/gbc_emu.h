/*
 * Game Boy / Game Boy Color 模拟器宿主适配层。
 *
 * 核心来自 components/gnuboy；这里只负责接本项目的条带推屏、手柄和 I2S。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "rom_store.h"

/* 选中后才把这一份 .gb/.gbc 解到 PSRAM，gnuboy 的 bank 指针会长期引用它。 */
esp_err_t gbc_emu_run(const rom_store_entry_t *entry);
