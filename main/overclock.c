#include "overclock.h"

#include "esp_log.h"
#include "esp_rom_regi2c.h"
#include "esp_rom_sys.h"
#include "soc/regi2c_bbpll.h"
#include "soc/esp32s3/rtc.h"
#include "xtensa/hal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "overclock";

#define OC_MIN_LEVEL (-8)
#define OC_MAX_LEVEL 8

int overclock_apply(int level)
{
    if (level < OC_MIN_LEVEL || level > OC_MAX_LEVEL) {
        ESP_LOGW(TAG, "档位 %d 超出 [%d, %d]，跳过", level, OC_MIN_LEVEL, OC_MAX_LEVEL);
        return 0;
    }

    /* 原始值只读一次并缓存：多次调用本函数时都相对同一个基准加档位，
     * 不会把上一次已经写进寄存器的偏移当成新的基准继续累加。 */
    static int original_div7_0 = -1;
    if (original_div7_0 < 0) {
        original_div7_0 = esp_rom_regi2c_read(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_OC_DIV_7_0);
    }
    uint8_t div7_0 = (uint8_t)(original_div7_0 + level);
    esp_rom_regi2c_write(I2C_BBPLL, I2C_BBPLL_HOSTID, I2C_BBPLL_OC_DIV_7_0, div7_0);
    vTaskDelay(pdMS_TO_TICKS(20)); /* 给模拟锁相环留稳定时间 */

    /* RTC 定时器走独立时钟源，不受这个寄存器影响，超频后仍是唯一能信的计时基准。
     * 用它量一段 100ms 窗口内 CPU 周期计数的增量，反推实测主频。 */
    uint64_t t0 = esp_rtc_get_time_us();
    uint32_t cc0 = xthal_get_ccount();
    esp_rom_delay_us(100000);
    uint32_t cc1 = xthal_get_ccount();
    uint64_t t1 = esp_rtc_get_time_us();
    int real_mhz = (int)((double)(cc1 - cc0) / (double)(t1 - t0));

    /* esp_rom_delay_us 内部按「ticks per us」换算延时，不重新校准的话，
     * 超频之后板载库函数（含它自己下一次调用）算出来的延时会系统性地偏短/偏长。 */
    esp_rom_set_cpu_ticks_per_us((uint32_t)real_mhz);

    ESP_LOGW(TAG, "超频档位 %+d：寄存器 0x%02X -> 0x%02X，实测 CPU 主频 %d MHz",
             level, original_div7_0, div7_0, real_mhz);
    return real_mhz;
}
