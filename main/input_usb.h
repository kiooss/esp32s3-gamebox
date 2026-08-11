/* ESP32-S3 原生 USB-OTG 上的通用 HID 游戏手柄。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 幂等初始化 USB Host 与 HID 驱动。失败不影响飞线手柄和串口键盘。 */
esp_err_t input_usb_init(void);

/* 返回 GAMEPAD_BIT_* 状态；未连接、未识别或拔出时返回 0。 */
uint8_t input_usb_poll(void);
bool input_usb_connected(void);
