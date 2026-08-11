/*
 * Game Boy / Game Boy Color 模拟器适配层
 *
 * gnuboy 输出 160x144 的大端 RGB565。本屏的公共画布是 288x224，采用 3:2
 * 等比最近邻放大到 240x216，四周留黑边。没有为 GBC 另开一套显示驱动：仍由
 * display_stream() 在核 1 上逐条带缩放和 DMA，核 0 同时模拟下一帧。
 *
 * 两块 160x144 RGB565 源缓冲放 PSRAM。它们不参与 DMA，只由核 0 写、核 1
 * 读；若强塞内部 RAM，会和 NES 的两块 64 KB 热 vidbuf 以及 DMA 条带争空间。
 */

#include <string.h>
#include "gbc_emu.h"
#include "audio_output.h"
#include "display.h"
#include "input_gamepad.h"
#include "input_serial.h"
#include "rgb_led.h"
#include "gnuboy.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gbc";

#define GBC_SCALE_NUM       3
#define GBC_SCALE_DEN       2
#define GBC_OUT_W           (GB_WIDTH * GBC_SCALE_NUM / GBC_SCALE_DEN)
#define GBC_OUT_H           (GB_HEIGHT * GBC_SCALE_NUM / GBC_SCALE_DEN)
#define GBC_OUT_X           ((DISP_FB_W - GBC_OUT_W) / 2)
#define GBC_OUT_Y           ((DISP_FB_H - GBC_OUT_H) / 2)
#define GBC_FRAME_PERIOD_US 16742  /* 4.194304 MHz / 70224 clocks = 59.7275 Hz */
#define GBC_AUDIO_S16_COUNT (AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET * 2)

/* gnuboy 用整数 `round(2^21 / requested_rate)` 做采样分频。请求 24000 时
 * 分频值是 87，实际产出 2^21/87 = 24105.2 Hz；I2S 若仍消费 24000 Hz，
 * 队列每秒会净增长约 105 帧并周期性丢包。宿主按核心的真实速率消费。 */
#define GBC_AUDIO_CLOCK      (1U << 21)
#define GBC_AUDIO_DIV        ((GBC_AUDIO_CLOCK + AUDIO_OUTPUT_SAMPLE_RATE / 2) / \
                              AUDIO_OUTPUT_SAMPLE_RATE)
#define GBC_I2S_SAMPLE_RATE  (GBC_AUDIO_CLOCK / GBC_AUDIO_DIV)

_Static_assert(GBC_OUT_W == 240 && GBC_OUT_H == 216,
               "GBC 画面应按 3:2 放大到 240x216");
_Static_assert(GBC_OUT_W <= DISP_FB_W && GBC_OUT_H <= DISP_FB_H,
               "GBC 放大结果必须能装进公共画布");

static uint16_t *s_framebuf[2];
static int16_t  *s_soundbuf;
static int       s_draw_idx;

/* gnuboy 已经输出大端 RGB565，这里只做 3:2 最近邻放大。每两个源像素写三个
 * 目标像素；竖向同理用整数映射，避免每像素除法和浮点数。 */
static void gbc_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const uint16_t *frame = ctx;
    memset(strip, 0, (size_t)DISP_FB_W * h * sizeof(uint16_t));

    for (int r = 0; r < h; r++) {
        int out_y = y0 + r;
        if (out_y < GBC_OUT_Y || out_y >= GBC_OUT_Y + GBC_OUT_H) continue;

        int src_y = (out_y - GBC_OUT_Y) * GBC_SCALE_DEN / GBC_SCALE_NUM;
        const uint16_t *src = frame + (size_t)src_y * GB_WIDTH;
        uint16_t *dst = strip + (size_t)r * DISP_FB_W + GBC_OUT_X;

        for (int x = 0; x < GB_WIDTH; x += 2) {
            uint16_t c0 = src[x];
            uint16_t c1 = src[x + 1];
            dst[0] = c0;
            dst[1] = c0;
            dst[2] = c1;
            dst += 3;
        }
    }
}

static void black_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    display_clear(C_BLACK);
}

static void video_callback(void *buffer)
{
    display_stream(gbc_strip, buffer);
}

/* gnuboy 的 length 是交错立体声 int16_t 的个数，不是帧数。 */
static void audio_callback(void *buffer, size_t length)
{
    audio_output_submit_stereo(buffer, length / 2);
}

static int map_pad(uint8_t state)
{
    int pad = 0;
    if (state & GAMEPAD_BIT_RIGHT)  pad |= GB_PAD_RIGHT;
    if (state & GAMEPAD_BIT_LEFT)   pad |= GB_PAD_LEFT;
    if (state & GAMEPAD_BIT_UP)     pad |= GB_PAD_UP;
    if (state & GAMEPAD_BIT_DOWN)   pad |= GB_PAD_DOWN;
    if (state & GAMEPAD_BIT_A)      pad |= GB_PAD_A;
    if (state & GAMEPAD_BIT_B)      pad |= GB_PAD_B;
    if (state & GAMEPAD_BIT_SELECT) pad |= GB_PAD_SELECT;
    if (state & GAMEPAD_BIT_START)  pad |= GB_PAD_START;
    return pad;
}

static esp_err_t alloc_buffers(void)
{
    const size_t frame_bytes = GB_WIDTH * GB_HEIGHT * sizeof(uint16_t);
    for (int i = 0; i < 2; i++) {
        s_framebuf[i] = heap_caps_calloc(1, frame_bytes,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_framebuf[i]) return ESP_ERR_NO_MEM;
    }

    s_soundbuf = heap_caps_calloc(GBC_AUDIO_S16_COUNT, sizeof(int16_t),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return s_soundbuf ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t gbc_emu_run(const uint8_t *rom, size_t rom_size, const char *name)
{
    if (!rom || rom_size < 0x150) return ESP_ERR_INVALID_ARG;

    printf("\nROM: %s  (%u 字节，GB/GBC)\n", name ? name : "(unknown)",
           (unsigned)rom_size);
    display_stream_sync(black_strip, NULL);

    esp_err_t err = alloc_buffers();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GB/GBC 缓冲分配失败：需要 2x%d KB PSRAM + %d 字节内部 RAM",
                 (GB_WIDTH * GB_HEIGHT * 2) / 1024,
                 GBC_AUDIO_S16_COUNT * (int)sizeof(int16_t));
        return err;
    }

    esp_err_t audio_err = audio_output_init(GBC_I2S_SAMPLE_RATE);
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "MAX98357 音频未启动：%s，继续静音运行",
                 esp_err_to_name(audio_err));
    }

    if (gnuboy_init(AUDIO_OUTPUT_SAMPLE_RATE, GB_AUDIO_STEREO_S16,
                    GB_PIXEL_565_BE, video_callback, audio_callback) != 0) {
        ESP_LOGE(TAG, "gnuboy 初始化失败");
        return ESP_FAIL;
    }
    if (gnuboy_load_rom(rom, rom_size) != 0) {
        ESP_LOGE(TAG, "ROM 解析失败（不是受支持的 GB/GBC 卡带？）");
        return ESP_FAIL;
    }

    s_draw_idx = 0;
    gnuboy_set_framebuffer(s_framebuf[s_draw_idx]);
    gnuboy_set_soundbuffer(s_soundbuf, GBC_AUDIO_S16_COUNT);
    gnuboy_reset(true);

    esp_err_t rgb_err = rgb_led_start_rainbow();
    if (rgb_err != ESP_OK) {
        ESP_LOGW(TAG, "板载 RGB 彩虹效果未启动：%s", esp_err_to_name(rgb_err));
    }

    input_serial_init();
    input_gamepad_init();

    printf("硬件模式：%s，内部 RAM 剩余 %u KB，PSRAM 剩余 %u KB\n",
           gnuboy_get_hwtype() == GB_HW_CGB ? "GBC" : "GB",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    printf("开始模拟，目标 59.7 fps。\n\n");

    int last_pad = -1;
    int frames = 0;
    int64_t emu_us = 0;
    int64_t stat_t0 = esp_timer_get_time();
    int64_t next_frame = stat_t0;

    while (1) {
        int pad = map_pad(input_serial_poll() | input_gamepad_poll());
        if (pad != last_pad) {
            gnuboy_set_pad(pad);
            last_pad = pad;
        }

        int64_t frame_t0 = esp_timer_get_time();
        gnuboy_run(true);
        emu_us += esp_timer_get_time() - frame_t0;

        s_draw_idx ^= 1;
        gnuboy_set_framebuffer(s_framebuf[s_draw_idx]);

        next_frame += GBC_FRAME_PERIOD_US;
        int64_t now = esp_timer_get_time();
        if (next_frame > now) {
            int64_t wait_us = next_frame - now;
            if (wait_us > 1500) vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
            while (esp_timer_get_time() < next_frame) { }
        } else {
            next_frame = now;
            vTaskDelay(1);  /* 跑不满时也要喂核 0 的 idle task/watchdog */
        }

        frames++;
        now = esp_timer_get_time();
        if (now - stat_t0 >= 1000000) {
            int fps10 = (int)(frames * 10000000LL / (now - stat_t0));
            float per = (float)emu_us / 1000.0f / frames;
            printf("%s %d.%d fps  (模拟+提交 %.1f ms/帧，CPU 余量 %d%%)\n",
                   gnuboy_get_hwtype() == GB_HW_CGB ? "GBC" : "GB",
                   fps10 / 10, fps10 % 10, per,
                   100 - (int)(emu_us * 100 / (now - stat_t0)));
            frames = 0;
            emu_us = 0;
            stat_t0 = now;
        }
    }

    return ESP_OK;
}
