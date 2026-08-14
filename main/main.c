/*
 * 在 ESP32-S3 + ST7789（240x320）上运行 NES / GB / GBC
 *
 * 流程：打印板级信息 -> 初始化屏 -> 开机选游戏 -> 启动模拟器（不返回）
 *
 * 接线见 display.h 顶部。换屏或显示不正常时改那里的宏，不用动这个文件。
 * 把 SHOW_DISPLAY_SELFTEST 改成 1 可以在启动模拟器前先跑一遍点屏诊断图。
 */

#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "display.h"
#include "audio_output.h"
#include "nes_emu.h"
#include "gbc_emu.h"
#include "snes_emu.h"
#include "rom_menu.h"

static const char *TAG = "main";

#define SHOW_DISPLAY_SELFTEST  0

static void print_board_info(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);

    printf("\n========= ESP32-S3 GAMEBOX =========\n");
    printf("芯片      : ESP32-S3, %d core(s), rev %d.%d\n",
           info.cores, info.revision / 100, info.revision % 100);
    printf("Flash     : %" PRIu32 " MB\n", flash / (1024 * 1024));
    printf("PSRAM     : %u KB\n",
           (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
    printf("内部空闲  : %u KB\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    printf("====================================\n\n");
}

#if SHOW_DISPLAY_SELFTEST
/* 点屏诊断图：换屏之后用它确认旋转 / 颜色顺序 / 反色是否设对。
 *   左上红右上绿左下蓝右下黄 -> 旋转和镜像对
 *   色条 红绿蓝黄青品白灰    -> RGB 顺序对
 *   灰阶左黑右白      -> invert 对
 *   白框四边贴满**画布**（不是面板）-> 画布落点对
 *
 * ⚠ 画的是 288x224 的画布，不是 320x240 的面板 —— 白框贴的是画布边界，
 * 外面还有 16/8 像素的黑边。要验 gap 得先把 DISP_FB_W/H 临时改成 DISP_W/H。
 * （以前这里用的是 DISP_W/DISP_H，画布比它小，右边和下边其实被裁掉了。）
 */
static void diag_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    display_clear(C_BLACK);

    static const uint16_t bars[8] = {
        C_RED, C_GREEN, C_BLUE, C_YELLOW, C_CYAN, C_MAGENTA, C_WHITE, C_GRAY,
    };
    int bar_w = DISP_FB_W / 8;
    for (int i = 0; i < 8; i++) {
        display_fill_rect(i * bar_w, 24, bar_w, 46, bars[i]);
    }

    for (int x = 0; x < DISP_FB_W; x++) {
        int v = (x * 255) / (DISP_FB_W - 1);
        display_fill_rect(x, 78, 1, 30, RGB565(v, v, v));
    }

    display_fill_rect(4, 4, 22, 14, C_RED);
    display_fill_rect(DISP_FB_W - 26, 4, 22, 14, C_GREEN);
    display_fill_rect(4, DISP_FB_H - 18, 22, 14, C_BLUE);
    display_fill_rect(DISP_FB_W - 26, DISP_FB_H - 18, 22, 14, C_YELLOW);

    display_text(8, 118, "border must touch all 4 edges", C_GRAY, 1);

    display_rect(0, 0, DISP_FB_W, DISP_FB_H, C_WHITE);
    display_rect(1, 1, DISP_FB_W - 2, DISP_FB_H - 2, C_WHITE);
    display_rect(2, 2, DISP_FB_W - 4, DISP_FB_H - 4, C_RED);
}

static void screen_diagnostic(void)
{
    display_stream_sync(diag_strip, NULL);
    vTaskDelay(pdMS_TO_TICKS(6000));
}
#endif

static void splash_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    display_clear(C_BLACK);
    display_text(58, 58,  "ESP32-S3  NES", C_WHITE, 2);
    display_text(58, 84,  "nofrendo", C_GRAY, 1);
    display_text(58, 98,  "loading...", C_GRAY, 1);
}

static void splash(void)
{
    display_stream_sync(splash_strip, NULL);
    vTaskDelay(pdMS_TO_TICKS(1200));
}

void app_main(void)
{
    print_board_info();

    /* 必须先于 display_init()：NES 视频缓冲要 65 KB 连续内部内存，
     * 等两块帧缓冲分配完就凑不出来了。 */
    if (nes_emu_prealloc() != ESP_OK) {
        ESP_LOGE(TAG, "NES 视频缓冲分配失败，内部 RAM 不够");
        return;
    }

    /* 声音开关只在当前运行中有效；每次启动都先恢复默认开启。 */
    audio_output_settings_init();

    if (display_init() != ESP_OK) {
        ESP_LOGE(TAG, "屏幕初始化失败，检查接线和 display.h 里的引脚定义");
        return;
    }

#if SHOW_DISPLAY_SELFTEST
    screen_diagnostic();
#endif
    splash();

    /* 开机选单只返回目录项；各模拟器在自己的大块内存准备妥当后再解压，SNES
     * 尤其不能先解出 4 MiB 再复制一份，否则 8 MiB PSRAM 会在峰值时耗尽。
     * 分区不可用时 entry 留 NULL，NES 继续走编译期嵌入 ROM 的回退路径。 */
    const rom_store_entry_t *entry = NULL;
    uint16_t launch_keys = 0;
    rom_menu_pick(&entry, &launch_keys);

    rom_system_t system = entry ? entry->system : ROM_SYSTEM_NES;
    esp_err_t run_err = system == ROM_SYSTEM_NES  ? nes_emu_run(entry)
                      : system == ROM_SYSTEM_SNES ? snes_emu_run(entry, launch_keys)
                                                  : gbc_emu_run(entry);
    if (run_err != ESP_OK) {
        ESP_LOGE(TAG, "模拟器启动失败");
    }
}
