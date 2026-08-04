/*
 * retro-go 框架垫片
 *
 * nofrendo 这份代码来自 retro-go（https://github.com/ducalex/retro-go），
 * 原本依赖它的框架。但看 nes/utils.h 就知道，真正用到的只有三样东西：
 *
 *   rg_system_log()  日志
 *   rg_crc32()       ROM 校验（用于查 database.h 里的游戏数据库）
 *   IRAM_ATTR        把 6502 解释器放进 IRAM
 *
 * 所以这里定义 RETRO_GO 并提供这三样，nofrendo 源码一行都不用改，
 * 将来上游更新可以直接覆盖。
 *
 * utils.h 里还有个 #else 分支能完全独立编译，但那条路会把 IRAM_ATTR 定义成空，
 * nes6502_execute 就只能跑在 flash 上吃 cache miss —— 所以不走那条。
 */
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_attr.h"       /* IRAM_ATTR */
#include "esp_rom_crc.h"    /* esp_rom_crc32_le */

#define RG_LOG_PRINTF 0

#define rg_system_log(level, ctx, ...)  printf(__VA_ARGS__)

/* retro-go 在 ESP 平台上就是直接调 ROM 里的 crc32_le，
 * 所以这里算出来的校验和与 database.h 的条目是一致的，数据库能真正命中。 */
#define rg_crc32(crc, buf, len) \
    esp_rom_crc32_le((uint32_t)(crc), (const uint8_t *)(buf), (uint32_t)(len))
