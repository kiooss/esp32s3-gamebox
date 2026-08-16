/*
 * Game Boy / Game Boy Color 模拟器适配层
 *
 * gnuboy 输出 160x144 的大端 RGB565。参照 retro-go 的 RG_DISPLAY_SCALING_FULL
 * （components/retro-go/rg_display.c 的 update_viewport_scaling()：直接拉伸到
 * screen_width x screen_height，不保长宽比）铺满整块 320x240 面板，不用公共的
 * 288x224 画布——和 Genesis 一样走 display_stream_sized() 提交自己的原生尺寸。
 * 横向 160->320 正好整数 2x；纵向 144->240 是 3:5，非整数，个别源行会被
 * 复制两次（同 retro-go 的最近邻映射，没有过滤）。
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
#include "input_usb.h"
#include "rgb_led.h"
#include "gnuboy.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gbc";

#define GBC_OUT_W           DISP_W
#define GBC_OUT_H           DISP_H
#define GBC_FRAME_PERIOD_US 16742  /* 4.194304 MHz / 70224 clocks = 59.7275 Hz */
#define GBC_AUDIO_S16_COUNT (AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET * 2)

/* 退出到 ROM 菜单：SELECT+START 长按 1 秒触发 esp_restart()。gnuboy 没有
 * 任何卡带电池 SRAM 落盘（cart.sram_dirty 只在内存里，从没写过 flash），
 * 软重启不丢数据。 */
#define EXIT_COMBO_BITS     (GAMEPAD_BIT_SELECT | GAMEPAD_BIT_START)
#define EXIT_HOLD_US        1000000

/* gnuboy 用整数 `round(2^21 / requested_rate)` 做采样分频。请求 24000 时
 * 分频值是 87，实际产出 2^21/87 = 24105.2 Hz；I2S 若仍消费 24000 Hz，
 * 队列每秒会净增长约 105 帧并周期性丢包。宿主按核心的真实速率消费。 */
#define GBC_AUDIO_CLOCK      (1U << 21)
#define GBC_AUDIO_DIV        ((GBC_AUDIO_CLOCK + AUDIO_OUTPUT_SAMPLE_RATE / 2) / \
                              AUDIO_OUTPUT_SAMPLE_RATE)
#define GBC_I2S_SAMPLE_RATE  (GBC_AUDIO_CLOCK / GBC_AUDIO_DIV)

_Static_assert(GBC_OUT_W == GB_WIDTH * 2,
               "GBC 横向铺满面板要求整数 2x");
_Static_assert(GBC_OUT_W <= DISP_W && GBC_OUT_H <= DISP_H,
               "GBC 输出尺寸不能超过面板");

static uint16_t *s_framebuf[2];
static int16_t  *s_soundbuf;
static int       s_draw_idx;

/* gnuboy 已经输出大端 RGB565。横向 160->320 是整数 2x，每个源像素原样写两遍。
 * 纵向 144->240 不是整数比，用最近邻按 retro-go 同样的 step 公式取源行
 * （见文件头注释）；每帧整块画布都会被写满，不用先清黑边。 */
static void gbc_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const uint16_t *frame = ctx;

    for (int r = 0; r < h; r++) {
        int out_y = y0 + r;
        int src_y = (out_y * GB_HEIGHT) / GBC_OUT_H;
        const uint16_t *src = frame + (size_t)src_y * GB_WIDTH;
        uint16_t *dst = strip + (size_t)r * GBC_OUT_W;

        for (int x = 0; x < GB_WIDTH; x++) {
            uint16_t c = src[x];
            dst[0] = c;
            dst[1] = c;
            dst += 2;
        }
    }
}

static void black_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    display_clear(C_BLACK);
}

static void video_callback(void *buffer)
{
    display_stream_sized(gbc_strip, buffer, GBC_OUT_W, GBC_OUT_H);
}

/* gnuboy 的 length 是交错立体声 int16_t 的个数，不是帧数。 */
static void audio_callback(void *buffer, size_t length)
{
    audio_output_submit_stereo(buffer, length / 2);
}

/* 配色切换：只对原版单色 GB 卡带有意义——lcd.c 的 sync_palette() 只在
 * !IS_CGB 时读 GB.video.colorize，真彩色 GBC 卡带走自己的调色板，切了
 * 也没有视觉效果，所以主循环只在非 CGB 卡带上响应按键。
 *
 * 触发键选 X：map_pad() 根本不读 GAMEPAD_BIT_X，GB 模式下这颗键天生空闲，
 * 单点即切，不用像 SNES 存档组合键那样加长按防误触。 */
typedef struct {
    gb_palette_t id;
    const char  *name;
} gbc_palette_preset_t;

static const gbc_palette_preset_t PALETTE_PRESETS[] = {
    { GB_PALETTE_DMG,  "DMG GREEN" },
    { GB_PALETTE_MGB0, "POCKET GRAY 1" },
    { GB_PALETTE_MGB1, "POCKET GRAY 2" },
    { GB_PALETTE_CGB,  "AUTO COLOR" },
    { GB_PALETTE_SGB,  "SUPER GB" },
};
#define PALETTE_PRESET_COUNT \
    (sizeof(PALETTE_PRESETS) / sizeof(PALETTE_PRESETS[0]))

/* 开机默认档位：POCKET GRAY 2（GB_PALETTE_MGB1）。 */
#define PALETTE_DEFAULT_IDX 2

static int s_palette_idx = PALETTE_DEFAULT_IDX;

typedef struct {
    const uint16_t *frame;
    const char     *text;
} gbc_notice_t;

/* 复用 gbc_strip 把最后一帧完整帧缓冲缩放上屏，再盖一条文字提示——和
 * snes_emu.c 的 save_notice_strip 同一个套路。配色切换和退出提示共用。 */
static void gbc_notice_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const gbc_notice_t *notice = ctx;
    gbc_strip(strip, y0, h, (void *)notice->frame);

    int text_w = (int)strlen(notice->text) * 6;
    int x = (GBC_OUT_W - text_w) / 2;
    display_fill_rect(x - 8, 108, text_w + 16, 24, C_BLACK);
    display_text(x, 116, notice->text, C_YELLOW, 1);
}

static void show_gbc_notice(const uint16_t *frame, const char *text)
{
    gbc_notice_t notice = { .frame = frame, .text = text };
    display_stream_sized(gbc_notice_strip, &notice, GBC_OUT_W, GBC_OUT_H);
    display_wait_idle();
}

static int map_pad(uint16_t state)
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

esp_err_t gbc_emu_run(const rom_store_entry_t *entry)
{
    if (!entry || entry->size < 0x150) return ESP_ERR_INVALID_ARG;

    rom_store_image_t image = {0};
    esp_err_t err = rom_store_load(entry, 0, &image);
    if (err != ESP_OK) return err;
    const uint8_t *rom = image.data;
    size_t rom_size = image.size;
    const char *name = entry->name;

    printf("\nROM: %s  (%u 字节，GB/GBC)\n", name ? name : "(unknown)",
           (unsigned)rom_size);
    display_stream_sync(black_strip, NULL);

    err = alloc_buffers();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GB/GBC 缓冲分配失败：需要 2x%d KB PSRAM + %d 字节内部 RAM",
                 (GB_WIDTH * GB_HEIGHT * 2) / 1024,
                 GBC_AUDIO_S16_COUNT * (int)sizeof(int16_t));
        rom_store_image_release(&image);
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
        rom_store_image_release(&image);
        return ESP_FAIL;
    }
    if (gnuboy_load_rom(rom, rom_size) != 0) {
        ESP_LOGE(TAG, "ROM 解析失败（不是受支持的 GB/GBC 卡带？）");
        rom_store_image_release(&image);
        return ESP_FAIL;
    }

    /* gnuboy 默认 .video.colorize = GB_PALETTE_CGB：原版单色 GB 卡带没有
     * 自己的调色板时，会按真实 GBC 开机 BIOS 的算法（查卡名校验和）套一套
     * 彩色配色，效果是本该单色的游戏也花花绿绿。这里强制成
     * PALETTE_PRESETS[PALETTE_DEFAULT_IDX]（POCKET GRAY 2）。对真正的
     * GBC 卡带无影响——lcd.c 的 sync_palette() 只在 !IS_CGB 时读这个设置，
     * 彩色卡带走自己的调色板。和 s_palette_idx 的初始值取自同一个宏，
     * 玩家可以用 X 键循环切换到其它预设，见下方主循环。 */
    gnuboy_set_palette(PALETTE_PRESETS[PALETTE_DEFAULT_IDX].id);

    s_draw_idx = 0;
    gnuboy_set_framebuffer(s_framebuf[s_draw_idx]);
    gnuboy_set_soundbuffer(s_soundbuf, GBC_AUDIO_S16_COUNT);
    gnuboy_reset(true);

    esp_err_t rgb_err = rgb_led_start_rainbow();
    if (rgb_err != ESP_OK) {
        ESP_LOGW(TAG, "板载 RGB 彩虹效果未启动：%s", esp_err_to_name(rgb_err));
    }

    input_serial_init();
    input_usb_init();
    input_gamepad_init();

    printf("硬件模式：%s，内部 RAM 剩余 %u KB，PSRAM 剩余 %u KB\n",
           gnuboy_get_hwtype() == GB_HW_CGB ? "GBC" : "GB",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    printf("开始模拟，目标 59.7 fps。\n\n");

    /* 只有非 CGB 卡带才响应配色切换键——理由见 gbc_notice_strip 之前
     * 的注释。hwtype 装 ROM 时就定了，整个运行期间不变，出循环外算一次。 */
    bool palette_switchable = (gnuboy_get_hwtype() != GB_HW_CGB);

    int last_pad = -1;
    uint16_t last_raw = 0;
    int64_t exit_hold_t0 = 0;
    int frames = 0;
    int64_t emu_us = 0;
    int64_t stat_t0 = esp_timer_get_time();
    int64_t next_frame = stat_t0;

    while (1) {
        uint16_t raw = input_serial_poll() | input_gamepad_poll() |
                       input_usb_poll();

        bool exit_combo = (raw & EXIT_COMBO_BITS) == EXIT_COMBO_BITS;
        if (exit_combo) {
            /* 组合键属于系统，不让游戏同时收到 SELECT/START。 */
            raw &= ~EXIT_COMBO_BITS;
            int64_t now = esp_timer_get_time();
            if (exit_hold_t0 == 0) exit_hold_t0 = now;
            if (now - exit_hold_t0 >= EXIT_HOLD_US) {
                show_gbc_notice(s_framebuf[s_draw_idx ^ 1], "EXITING...");
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            exit_hold_t0 = 0;
        }

        int pad = map_pad(raw);
        if (pad != last_pad) {
            gnuboy_set_pad(pad);
            last_pad = pad;
        }

        /* X 键边沿触发：map_pad() 不读它，raw 里的 X 位对 gnuboy 完全透明，
         * 不需要像退出组合键那样把这一位从 raw/pad 里摘掉。 */
        if (palette_switchable &&
            (raw & GAMEPAD_BIT_X) && !(last_raw & GAMEPAD_BIT_X)) {
            s_palette_idx = (s_palette_idx + 1) % PALETTE_PRESET_COUNT;
            gnuboy_set_palette(PALETTE_PRESETS[s_palette_idx].id);
            show_gbc_notice(s_framebuf[s_draw_idx ^ 1],
                            PALETTE_PRESETS[s_palette_idx].name);
        }
        last_raw = raw;

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
