/*
 * ESP32-S3-DevKitC-1 板载可寻址 RGB LED
 *
 * 这块实物用红/蓝对照测过：GPIO38 不亮，GPIO48 能正确显示蓝色，所以固定
 * 用 GPIO48。只驱动一颗像素，RMT 每次发送约 30 us；40 ms 更新一次，对
 * 60 fps 模拟器的影响远小于一帧预算。
 */

#include "rgb_led.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rgb_led";
#define RGB_LED_GPIO       48
#define EFFECT_TICK_MS     40
#define BREATH_STEPS       128
#define BREATH_MIN         2
#define BREATH_MAX         36      /* 峰值约 14%，避免板载灯刺眼 */

static led_strip_handle_t s_strip;

static void rainbow_task(void *arg)
{
    uint16_t hue = 0;
    uint8_t breath_phase = 0;
    while (1) {
        /* 先生成 0..255..0 的三角相位，再平方做近似 gamma 校正。
         * 肉眼对暗部更敏感，平方后会缓慢亮起、自然熄灭，不会像线性三角波
         * 那样在最亮点看出明显折返。完整一次呼吸约 5.1 秒。 */
        uint16_t half_phase = breath_phase < BREATH_STEPS / 2
                            ? breath_phase
                            : BREATH_STEPS - 1 - breath_phase;
        uint16_t ramp = (half_phase * 255 + 31) / 63;
        uint32_t curved = (uint32_t)ramp * ramp;
        uint8_t value = BREATH_MIN +
            (curved * (BREATH_MAX - BREATH_MIN) + 32512) / 65025;

        esp_err_t err = led_strip_set_pixel_hsv(s_strip, 0, hue, 255, value);
        if (err == ESP_OK) err = led_strip_refresh(s_strip);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "彩虹刷新失败：%s", esp_err_to_name(err));
            break;
        }

        hue = (hue + 2) % 360;       /* 色相一圈约 7.2 秒，不和呼吸周期重合 */
        breath_phase = (breath_phase + 1) % BREATH_STEPS;
        vTaskDelay(pdMS_TO_TICKS(EFFECT_TICK_MS));
    }

    led_strip_clear(s_strip);
    led_strip_del(s_strip);
    s_strip = NULL;
    vTaskDelete(NULL);
}

esp_err_t rgb_led_start_rainbow(void)
{
    if (s_strip) return ESP_OK;

    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config,
                                             &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d 初始化失败：%s", RGB_LED_GPIO,
                 esp_err_to_name(err));
        return err;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        rainbow_task, "rgb_rainbow", 2048, NULL, tskIDLE_PRIORITY + 1,
        NULL, 0);
    if (created != pdPASS) {
        led_strip_del(s_strip);
        s_strip = NULL;
        ESP_LOGE(TAG, "彩虹任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "GPIO%d 幻彩呼吸已启动（亮度 %d~%d/255，呼吸约 5.1 秒）",
             RGB_LED_GPIO, BREATH_MIN, BREATH_MAX);
    return ESP_OK;
}
