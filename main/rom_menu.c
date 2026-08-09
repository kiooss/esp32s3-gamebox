/*
 * 开机选单的实现
 *
 * ---- 布局 ----
 *
 * 画布是 288x224（NES 画布尺寸，居中在 320x240 的屏上，见 display.h）。
 * 21 个游戏 x 8px 行距 = 168px，加 14px 标题行 = 182px，剩 42px 边距。
 *
 * 8px 行距下字高 7px、行间只有 1px 缝，所以光标用**反白**（填充块 + 黑字）
 * 而不是箭头 —— 块的边界自己划出了行的界限，比在密排文本里找箭头清楚。
 * （画布从 192 涨到 224 行之后其实有余量把行距放宽到 9~10px 了，
 *   但反白光标在宽行距下同样好使，就没动。）
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
#include "input_serial.h"
#include "input_gamepad.h"
#include "nofrendo.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "menu";

#define TITLE_Y     2
#define LIST_Y      14      /* 第一行的 y */
#define ROW_H       8       /* 行距，字高 7px + 1px 缝 */
#define TEXT_X      8       /* 文字左缩进 */
#define HL_PAD      2       /* 反白块比文字左右各多出这么多 */

#define POLL_MS     16      /* 约 60 Hz，和游戏帧率一个量级 */

static uint8_t poll_input(void)
{
    return input_serial_poll() | input_gamepad_poll();
}

/* 条带回调：整份绘制列表每帧会被逐条带调用 BAND_COUNT 次，每次只画到落在
 * 当前条带里的那几行（display.c 的绘图原语自己裁）。菜单只有按键时才重画，
 * 重复执行这段的开销可以忽略。 */
typedef struct { int count, sel; } draw_args_t;

static void draw_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const draw_args_t *a = ctx;
    int count = a->count, sel = a->sel;

    display_clear(C_BLACK);

    char head[32];
    snprintf(head, sizeof(head), "SELECT A GAME            %2d", count);
    display_text(TEXT_X, TITLE_Y, head, C_CYAN, 1);

    for (int i = 0; i < count; i++) {
        const rom_store_entry_t *e = rom_store_entry(i);
        if (!e) break;
        int y = LIST_Y + i * ROW_H;

        if (i == sel) {
            /* 反白：先铺一条亮块，再在上面写黑字。
             * 块宽铺满画布，这样长短不一的名字看着也是整齐一条。 */
            display_fill_rect(TEXT_X - HL_PAD, y - 1,
                              DISP_FB_W - 2 * (TEXT_X - HL_PAD), ROW_H,
                              C_WHITE);
            display_text(TEXT_X, y, e->name, C_BLACK, 1);
        } else {
            display_text(TEXT_X, y, e->name, C_GRAY, 1);
        }
    }
}

/* ctx 指向栈上的 draw_args_t，所以必须用 sync 版本等推完再返回。 */
static void draw(int count, int sel)
{
    draw_args_t a = { .count = count, .sel = sel };
    display_stream_sync(draw_strip, &a);
}

bool rom_menu_pick(const uint8_t **data, size_t *size, const char **name)
{
    int count = rom_store_init();
    if (count <= 0) {
        ESP_LOGW(TAG, "roms 分区里没有游戏，用编译期嵌入的那个");
        return false;
    }

    /* 输入两路都要 —— 手柄是正路，串口是手柄不灵时的后路。
     * 两个 init 都是幂等的，nes_emu_run() 之后再调一次没问题。 */
    input_serial_init();
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

        if (edge & NES_PAD_A || edge & NES_PAD_START) {
            const rom_store_entry_t *e = rom_store_entry(sel);
            if (!e) continue;           /* 不该发生，稳妥起见 */
            *data = e->data;
            *size = e->size;
            *name = e->name;
            printf("选中：%s（%u 字节）\n\n", e->name, (unsigned)e->size);
            return true;
        }

        int moved = 0;
        if (edge & NES_PAD_UP)   moved = -1;
        if (edge & NES_PAD_DOWN) moved = +1;

        /* 上下环绕。21 项里从头翻到尾比一格一格挪快得多。 */
        if (moved) {
            sel = (sel + moved + count) % count;
            draw(count, sel);
        }
    }
}
