#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 目前只给 SMW 做一个“最近状态”。底层用两个交替文件防止保存中断电把
 * 唯一一份好档覆盖掉；rom_crc 用来拒绝给另一份 ROM 加载。 */
esp_err_t snes_save_init(uint32_t rom_crc);
bool snes_save_load_latest(uint32_t rom_crc);
/* 恢复双槽里序号较小的上一份；它不存在或损坏时仍退回最新一份，
 * 所以恢复操作不会把用户直接丢到新游戏。 */
bool snes_save_load_previous(uint32_t rom_crc);
bool snes_save_write(uint32_t rom_crc);
