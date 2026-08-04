/*
 * 串口键盘当手柄 —— 真手柄到货前的临时方案
 *
 * idf.py monitor 会把你敲的字符经串口发给板子，这里读出来映射成 NES 手柄按键。
 */
#pragma once

#include <stdint.h>

/* 装 UART0 驱动。必须在开始模拟之前调用一次。 */
void input_serial_init(void);

/* 每帧调一次，返回 NES 手柄位掩码（NES_PAD_* 的组合），直接喂给 input_update()。 */
uint8_t input_serial_poll(void);
