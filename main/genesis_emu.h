/* Sega Mega Drive / Genesis 模拟器宿主适配层。 */
#pragma once

#include "esp_err.h"
#include "rom_store.h"

/* 装载并运行指定卡带。正常情况下不返回。 */
esp_err_t genesis_emu_run(const rom_store_entry_t *entry);
