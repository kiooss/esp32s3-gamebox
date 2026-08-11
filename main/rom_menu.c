/*
 * 开机选单的实现
 *
 * ---- 布局 ----
 *
 * 画布是 288x224（NES 画布尺寸，居中在 320x240 的屏上，见 display.h）。
 * 每页 10 个游戏 x 18px 行距 = 180px。30 个游戏正好三页；以后继续加 ROM
 * 也只会自然增加页数，不会把列表画出屏幕。当前页由 sel / PAGE_ROWS 推导，
 * 不单独保存状态，避免选择项和页码不同步。
 *
 * 选中项用青色反白（填充块 + 黑字），比箭头更醒目。中文是 16x16 点阵，
 * 行距留 2px，有限的 288x224 画布仍能同时容纳标题、10 行列表和操作提示。
 *
 * ---- 为什么要边沿检测 ----
 *
 * 摇杆报的是**状态**不是事件：推着不动，每次 poll 都返回 UP。直接拿它移动
 * 光标的话，一次推杆会在几毫秒里把光标扫到底。所以只在「上一帧没按、这一帧
 * 按了」的瞬间移动一格。
 *
 * 串口那路同理，而且它本来就没有「松手」事件（靠终端的按键重复维持按下），
 * 边沿检测对两路都是必须的。
 */

#include <string.h>
#include "rom_menu.h"
#include "rom_store.h"
#include "display.h"
#include "audio_output.h"
#include "input_serial.h"
#include "input_gamepad.h"
#include "input_usb.h"
#include "nofrendo.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "menu";

#define TITLE_Y        2
#define PAGE_Y         7
#define HEADER_LINE_Y  20
#define LIST_Y         25      /* 第一行的 y */
#define ROW_H          18      /* 16px 中文点阵 + 2px 行距 */
#define PAGE_ROWS      10
#define FOOTER_LINE_Y  204
#define FOOTER_Y       207
#define TEXT_X         8       /* 文字左缩进 */
#define SOUND_X        184     /* `声音:开/关` 右边仍给页码留足空间 */
#define HL_PAD         2       /* 反白块比文字左右各多出这么多 */
#define C_DIVIDER      RGB565(48, 48, 48)

#define POLL_MS     16      /* 约 60 Hz，和游戏帧率一个量级 */

static uint8_t poll_input(void)
{
    return input_serial_poll() | input_gamepad_poll() | input_usb_poll();
}

/* 条带回调：整份绘制列表每帧会被逐条带调用 BAND_COUNT 次，每次只画到落在
 * 当前条带里的那几行（display.c 的绘图原语自己裁）。菜单只有按键时才重画，
 * 重复执行这段的开销可以忽略。 */
typedef struct { int count, sel; bool muted; } draw_args_t;

static const char *system_name(rom_system_t system)
{
    if (system == ROM_SYSTEM_GBC) return "GBC";
    if (system == ROM_SYSTEM_GB) return "GB ";
    return "NES";
}

static void draw_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const draw_args_t *a = ctx;
    int count = a->count, sel = a->sel;
    int page = sel / PAGE_ROWS;
    int page_count = (count + PAGE_ROWS - 1) / PAGE_ROWS;
    int first = page * PAGE_ROWS;
    int last = first + PAGE_ROWS;
    if (last > count) last = count;

    display_clear(C_BLACK);

    display_text(TEXT_X, TITLE_Y, "游戏选择", C_CYAN, 1);
    display_text(SOUND_X, PAGE_Y, a->muted ? "声音:关" : "声音:开",
                 a->muted ? C_GRAY : C_GREEN, 1);

    char page_text[32];
    snprintf(page_text, sizeof(page_text), "%d/%d", page + 1, page_count);
    int page_x = DISP_FB_W - TEXT_X - (int)strlen(page_text) * 6;
    display_text(page_x, PAGE_Y, page_text, C_WHITE, 1);
    display_fill_rect(TEXT_X, HEADER_LINE_Y, DISP_FB_W - 2 * TEXT_X, 1,
                      C_DIVIDER);

    for (int i = first; i < last; i++) {
        const rom_store_entry_t *e = rom_store_entry(i);
        if (!e) break;
        int y = LIST_Y + (i - first) * ROW_H;

        char line[48];
        snprintf(line, sizeof(line), "%02d %s %s", i + 1,
                 system_name(e->system), e->name);

        if (i == sel) {
            /* 反白：先铺一条青色块，再在上面写黑字。
             * 块宽铺满画布，这样长短不一的名字看着也是整齐一条。 */
            display_fill_rect(TEXT_X - HL_PAD, y - 1,
                              DISP_FB_W - 2 * (TEXT_X - HL_PAD), ROW_H - 1,
                              C_CYAN);
            display_text(TEXT_X, y, line, C_BLACK, 1);
        } else {
            display_text(TEXT_X, y, line, C_GRAY, 1);
        }
    }

    display_fill_rect(TEXT_X, FOOTER_LINE_Y, DISP_FB_W - 2 * TEXT_X, 1,
                      C_DIVIDER);
    display_text(68, FOOTER_Y, "A开始  B声音  左右翻页", C_GRAY, 1);
}

/* ctx 指向栈上的 draw_args_t，所以必须用 sync 版本等推完再返回。 */
static void draw(int count, int sel)
{
    draw_args_t a = {
        .count = count,
        .sel = sel,
        .muted = audio_output_is_muted(),
    };
    display_stream_sync(draw_strip, &a);
}

bool rom_menu_pick(const uint8_t **data, size_t *size, const char **name,
                   rom_system_t *system)
{
    int count = rom_store_init();
    if (count <= 0) {
        ESP_LOGW(TAG, "roms 分区里没有游戏，用编译期嵌入的那个");
        return false;
    }

    /* 三路输入并存：飞线手柄、USB HID、串口调试键盘。init 都是幂等的，
     * 模拟器启动后再调一次没有副作用。 */
    input_serial_init();
    input_usb_init();
    input_gamepad_init();

    printf("\n开机选单：%d 个游戏。摇杆上下选，A 或 START 确认。\n", count);
    printf("（想换游戏按板子上的 RST 重启）\n\n");

    int sel = 0;
    uint8_t prev = poll_input();    /* 先读一次当基线：上电时可能有键按着 */
    draw(count, sel);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        uint8_t now = poll_input();
        uint8_t edge = now & ~prev;     /* 这一帧新按下的位 */
        prev = now;

        /* B 在开机选单里没有游戏语义，正好做声音开关；进游戏后仍完整保留
         * 原来的 B（跑/发射），不会占掉任何模拟器按键组合。 */
        if (edge & NES_PAD_B) {
            bool muted = !audio_output_is_muted();
            esp_err_t err = audio_output_set_muted(muted);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "声音已%s，但保存失败：%s",
                         muted ? "关闭" : "开启", esp_err_to_name(err));
            }
            draw(count, sel);
            continue;
        }

        if (edge & NES_PAD_A || edge & NES_PAD_START) {
            const rom_store_entry_t *e = rom_store_entry(sel);
            if (!e) continue;           /* 不该发生，稳妥起见 */
            *data = e->data;
            *size = e->size;
            *name = e->name;
            *system = e->system;
            printf("选中：[%s] %s（%u 字节）\n\n", system_name(e->system),
                   e->name, (unsigned)e->size);
            return true;
        }

        /* 左右直接翻一整页，并尽量保留当前行。最后一页不足 10 项时，
         * 同一行不存在就落到最后一项；页首和页尾之间同样可以环绕。 */
        if (edge & (NES_PAD_LEFT | NES_PAD_RIGHT)) {
            int page_count = (count + PAGE_ROWS - 1) / PAGE_ROWS;
            int page = sel / PAGE_ROWS;
            int row = sel % PAGE_ROWS;
            int page_delta = (edge & NES_PAD_LEFT) ? -1 : +1;

            page = (page + page_delta + page_count) % page_count;
            sel = page * PAGE_ROWS + row;
            if (sel >= count) sel = count - 1;
            draw(count, sel);
            continue;       /* 斜推摇杆时以翻页为准，避免再上下移动一格 */
        }

        int moved = 0;
        if (edge & NES_PAD_UP)   moved = -1;
        if (edge & NES_PAD_DOWN) moved = +1;

        /* 上下环绕。跨过页边界时 draw() 会按新的 sel 自动切页。 */
        if (moved) {
            sel = (sel + moved + count) % count;
            draw(count, sel);
        }
    }
}
