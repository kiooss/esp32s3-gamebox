/*
 * ESP32-S3-DevKitC-1 板载可寻址 RGB LED —— 开局帧率状态灯
 *
 * 最早的版本是彩虹呼吸 + 音效联动，纯装饰，颜色跟系统状态没有任何关联，
 * 实测下来「有点鸡肋」。改成读数用途：把每个模拟器每秒统计的「实际帧率 /
 * 目标帧率」分三档映射成三个原生色（绿/蓝/红），不用开串口 monitor 扫一眼
 * 灯就知道帧率有没有掉。一开始用的是「CPU 余量」（cpu 占用率），实测那
 * 个数字基本只跟 ROM 走、同一局游戏里几乎不变，灯跟摆设一样；换成帧率
 * 达成率以后才真的会随卡顿波动。
 *
 * 故意不用连续色相渐变——亮度压到 value=4 这么暗时，混色（比如红黄之间
 * 的过渡）会因为通道只剩 0~2 那几档而分不清，原生色因为全程只点亮一个
 * LED 芯片、不混色，同样的暗度下反而看得清。
 *
 * 数据本身就是每秒才更新一次，不需要常驻任务做动画，直接在调用点同步设
 * 一次颜色即可，比原来的 FreeRTOS 呼吸任务简单。
 *
 * 只在刚进游戏的头 10 秒亮，之后灭掉：常亮在实际玩的时候反而是干扰，
 * 开局这几秒看一眼「稳不稳」就够了。每次进新游戏（rgb_led_init() 被调用）
 * 都会重新打开这个 10 秒窗口。
 *
 * 这块实物用红/蓝对照测过：GPIO38 不亮，GPIO48 能正确显示蓝色，所以固定
 * 用 GPIO48。
 */

#include <stdbool.h>
#include "rgb_led.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "rgb_led";
#define RGB_LED_GPIO   48
/* 亮度封顶，实测标定出来的下限：value=4 时如果用红黄绿连续渐变色相，混色
 * 通道只有 0~2 那几档，肉眼分不清；改成三个"原生色"（单通道纯色：红/绿/蓝
 * 各自只点亮 WS2812 里的一个芯片，不混色）以后，value=4 一样能明确分辨，
 * 不受量化影响——这才是这颗灯能压到多暗的真正下限，不是色相算法的问题。 */
#define STATUS_VALUE   4

/* 帧率达成率分三档，映射到三个原生色（不再是连续渐变）：
 * 接近满帧 -> 绿，轻微掉帧 -> 蓝，明显卡顿 -> 红。
 * 阈值是先按经验定的，觉得档位切早/切晚了就调这两个常量。 */
#define PERF_GOOD_PCT   90
#define PERF_MID_PCT    70

#define WINDOW_US   (10 * 1000000LL)   /* 进游戏后亮的时长 */

static led_strip_handle_t s_strip;
static int64_t s_window_start;
static bool    s_window_open;   /* 窗口结束后置 false，避免每秒重复刷"灭" */

esp_err_t rgb_led_init(void)
{
    /* 每次进游戏都重新打开 10 秒窗口，不管硬件是不是已经初始化过。 */
    s_window_start = esp_timer_get_time();
    s_window_open  = true;

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

    ESP_LOGI(TAG, "GPIO%d 开局帧率状态灯已启动（亮度封顶 %d/255，亮 %lld 秒）",
             RGB_LED_GPIO, STATUS_VALUE, WINDOW_US / 1000000);
    return ESP_OK;
}

void rgb_led_report_perf(int percent)
{
    if (!s_strip) return;

    if (esp_timer_get_time() - s_window_start >= WINDOW_US) {
        if (!s_window_open) return;   /* 已经灭过了，不用每秒重复刷 */
        s_window_open = false;
        esp_err_t err = led_strip_clear(s_strip);
        if (err == ESP_OK) err = led_strip_refresh(s_strip);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "状态灯熄灭失败：%s", esp_err_to_name(err));
        }
        return;
    }

    uint16_t hue = percent >= PERF_GOOD_PCT ? 120   /* 绿：接近满帧 */
                 : percent >= PERF_MID_PCT  ? 240   /* 蓝：轻微掉帧 */
                 :                            0;    /* 红：明显卡顿 */
    esp_err_t err = led_strip_set_pixel_hsv(s_strip, 0, hue, 255, STATUS_VALUE);
    if (err == ESP_OK) err = led_strip_refresh(s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "状态灯刷新失败：%s", esp_err_to_name(err));
    }
}

void rgb_led_off(void)
{
    if (!s_strip) return;
    s_window_open = false;
    esp_err_t err = led_strip_clear(s_strip);
    if (err == ESP_OK) err = led_strip_refresh(s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "状态灯熄灭失败：%s", esp_err_to_name(err));
    }
}
