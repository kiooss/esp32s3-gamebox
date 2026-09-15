/*
 * 串口键盘当手柄 —— 真手柄到货前的临时方案
 *
 * idf.py monitor 会把你敲的字符经串口发给板子，这里读出来映射成 NES 手柄按键。
 */
#pragma once

#include <stdint.h>

/* 装 UART0 驱动。必须在开始模拟之前调用一次。 */
void input_serial_init(void);

/* 初始化后调用，丢弃启动等待期间的字符、转义序列和按键保持状态。 */
void input_serial_discard(void);

/* 每帧调一次，返回宿主按键位掩码。低 8 位与 NES_PAD_* 一致，可直接喂给
 * input_update()；高两位是 GAMEPAD_BIT_X/Y，只有 SNES 会用。 */
uint16_t input_serial_poll(void);
