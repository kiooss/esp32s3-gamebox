/*
 * retro-go 框架垫片（snes9x 版）
 *
 * 和 components/nofrendo/rg_system.h 同样的用意：让上游源码一行不改就能编。
 * 区别是 snes9x 用得更少 —— 整个 src/ 里只有 port.h 那一句
 * `#include <rg_system.h>`，没有任何 rg_* 调用，也没用 IRAM_ATTR。
 * 所以这里只需要保证被 include 时不报错、并把常用的标准头带进去。
 *
 * 故意不去 include nofrendo 那份：两个组件各自独立，上游更新时可以单独覆盖，
 * 也不会因为 REQUIRES 互相牵连。
 */
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "esp_attr.h"
#include "esp_rom_crc.h"

#define RG_LOG_PRINTF 0

#define rg_system_log(level, ctx, ...)  printf(__VA_ARGS__)

#define rg_crc32(crc, buf, len) \
    esp_rom_crc32_le((uint32_t)(crc), (const uint8_t *)(buf), (uint32_t)(len))
