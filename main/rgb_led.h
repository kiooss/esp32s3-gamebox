#pragma once

#include "esp_err.h"

/* ROM 成功装载后启动低亮度彩虹循环。任务只创建一次，之后不返回。 */
esp_err_t rgb_led_start_rainbow(void);
