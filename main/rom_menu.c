/*
 * 开机选单的实现
 *
 * ---- 两级结构 ----
 *
 * 分类页（选平台）--A--> 游戏列表（选游戏）--A--> 启动
 *      ^                      |
 *      +----------B-----------+
 *
 * 两页共用同一个轮询循环、同一份边沿检测和同一个条带回调，差别只有标题、
 * 页脚、要不要画页码和音量这几项 —— 所以不写两套 draw_strip，由 draw_*()
 * 各自填好一份 draw_args_t 快照，回调照着画就行。
 *
 * B 在两页含义不同：分类页是顶层，没有上一级可退，B 空着正好继续当音量键；
 * 游戏列表里 B 才是返回。两页的页脚各自写明自己的键，不会看错。音量因此
 * 只能在分类页调 —— 开机必然经过那一页，真要中途改也可以 B 退回去。
 *
 * ---- 布局 ----
 *
 * 画布是 288x224（NES 画布尺寸，居中在 320x240 的屏上，见 display.h）。
 * 每页 10 个游戏 x 18px 行距 = 180px，超出的自然翻页。分类页最多 5 行
 * （五个平台），不会翻页。
 *
 * 选中项用反白（填充块 + 浅色字），比箭头更醒目。中文是 16x16 点阵，
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
/* 声音/亮度两个指示器紧挨着排在标题行右侧，右边仍要给页码（"d/d"，18px）
 * 留空间。中文字形 17px/字，数字 6px/字，两个指示器最长都是 "X度:100"
 * 那种 4 字 58px，中间留 6px 间距，算下来 BRIGHT_X 结尾正好卡在页码前。 */
#define SOUND_X        136
#define BRIGHT_X       200
#define HL_PAD         2       /* 反白块比文字左右各多出这么多 */
#define LINE_CHARS_MAX ((DISP_FB_W - TEXT_X) / 6)

/* rom_system_t 一共五个取值，所以分类最多五种。 */
#define SYSTEM_COUNT   5

#define POLL_MS     16      /* 约 60 Hz，和游戏帧率一个量级 */

static uint16_t poll_input(void)
{
    return input_serial_poll() | input_gamepad_poll() | input_usb_poll();
}

typedef struct {
    rom_system_t system;
    int          count;
} category_t;

/* 条带回调的输入。整份绘制列表每帧会被逐条带调用 BAND_COUNT 次，每次只画到
 * 落在当前条带里的那几行（display.c 的绘图原语自己裁）。菜单只有按键时才
 * 重画，重复执行这段的开销可以忽略。
 *
 * ctx 指向调用方栈上的这份快照，所以画的时候必须用 display_stream_sync()
 * 等推完再返回。 */
typedef struct {
    const char *title;
    const char *footer;
    bool  show_volume;      /* 游戏列表把 B 让给了「返回」，那页不显示音量 */
    int   volume;
    int   backlight;
    int   page;             /* 0 基；page_count <= 1 时整个页码都不画 */
    int   page_count;
    int   sel_row;          /* 反白哪一行（页内行号） */
    int   row_count;
    char  lines[PAGE_ROWS][64];
} draw_args_t;

/* 一律补到 4 字符宽：加了 SNES 之后名字列才还能对齐成一竖条。 */
static const char *system_name(rom_system_t system)
{
    if (system == ROM_SYSTEM_SNES) return "SNES";
    if (system == ROM_SYSTEM_GENESIS) return "MD  ";
    if (system == ROM_SYSTEM_GBC) return "GBC ";
    if (system == ROM_SYSTEM_GB) return "GB  ";
    return "NES ";
}

/* 按 display_text 的步进量算像素宽度：汉字 17px、ASCII 6px（见 display.c
 * 末尾那两行 cx += ）。菜单文本只有这两类，够用。有了它页脚才能居中——
 * 两页的页脚长度不一样，像以前那样把 x 写死成 38 的话短的那条会偏左。 */
static int text_width(const char *s)
{
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        if ((*p & 0xF0) == 0xE0) { w += 17; p += 3; }    /* BMP 汉字 */
        else                     { w += 6;  p += 1; }
    }
    return w;
}

static void draw_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const draw_args_t *a = ctx;

    /* 经典 GAMEBOY DMG 绿色 4 阶（C_GB0..C_GB3，见 display.h），不是
     * 中性灰阶。背景 C_GB0（浅黄绿），标题/正文用最深的 C_GB3 压对比度，
     * 次要信息用 C_GB2。C_GB0 只留给深色块上的反白字——直接铺在浅色
     * 背景上对比度太弱，会糊。 */
    display_clear(C_GB0);

    display_text(TEXT_X, TITLE_Y, a->title, C_GB3, 1);

    if (a->show_volume) {
        char vol_text[16];
        snprintf(vol_text, sizeof(vol_text), "声音:%d", a->volume);
        display_text(SOUND_X, PAGE_Y, vol_text, C_GB2, 1);
    }
    char bl_text[16];
    snprintf(bl_text, sizeof(bl_text), "亮度:%d", a->backlight);
    display_text(BRIGHT_X, PAGE_Y, bl_text, C_GB2, 1);

    /* 只有一页就不画页码：分类页永远是 "1/1"，写出来只是噪声。 */
    if (a->page_count > 1) {
        char page_text[32];
        snprintf(page_text, sizeof(page_text), "%d/%d",
                 a->page + 1, a->page_count);
        int page_x = DISP_FB_W - TEXT_X - (int)strlen(page_text) * 6;
        display_text(page_x, PAGE_Y, page_text, C_GB3, 1);
    }
    display_fill_rect(TEXT_X, HEADER_LINE_Y, DISP_FB_W - 2 * TEXT_X, 1,
                      C_GB2);

    for (int row = 0; row < a->row_count; row++) {
        int y = LIST_Y + row * ROW_H;
        const char *line = a->lines[row];

        if (row == a->sel_row) {
            /* 反白：铺一条 C_GB2 块，再在上面写 C_GB0 字——不用最深的
             * C_GB3 是嫌太重，C_GB2 对比度也够。块宽铺满画布，这样
             * 长短不一的名字看着也是整齐一条。 */
            display_fill_rect(TEXT_X - HL_PAD, y - 1,
                              DISP_FB_W - 2 * (TEXT_X - HL_PAD), ROW_H - 1,
                              C_GB2);
            display_text(TEXT_X, y, line, C_GB0, 1);
        } else {
            display_text(TEXT_X, y, line, C_GB2, 1);
        }
    }

    display_fill_rect(TEXT_X, FOOTER_LINE_Y, DISP_FB_W - 2 * TEXT_X, 1,
                      C_GB2);
    display_text((DISP_FB_W - text_width(a->footer)) / 2, FOOTER_Y,
                 a->footer, C_GB2, 1);
}

/* 统计每个平台各有多少游戏，返回分类数。
 *
 * 故意不记录「起始下标 + 长度」：那等于把「同平台条目在 rom_store 里连续」
 * 变成硬约束。目前 pack_roms.py 确实按 system 排过序，但没必要让菜单依赖它
 * ——下面 nth_of_system() 每次线性扫描，条目只有几十个，省下的复杂度更值。 */
static int build_categories(int count, category_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        const rom_store_entry_t *e = rom_store_entry(i);
        if (!e) break;

        int k = 0;
        while (k < n && out[k].system != e->system) k++;
        if (k == n) {
            if (n >= max) continue;     /* 不该发生：枚举就五个取值 */
            out[n].system = e->system;
            out[n].count  = 0;
            n++;
        }
        out[k].count++;
    }
    return n;
}

/* 平台 system 的第 j 个游戏在 rom_store 里的下标；没有就返回 -1。 */
static int nth_of_system(int count, rom_system_t system, int j)
{
    for (int i = 0; i < count; i++) {
        const rom_store_entry_t *e = rom_store_entry(i);
        if (!e) break;
        if (e->system == system && j-- == 0) return i;
    }
    return -1;
}

static void draw_categories(const category_t *cats, int cat_count, int sel)
{
    draw_args_t a = {
        .title       = "平台选择",
        .footer      = "A进入  B声音  Y亮度",
        .show_volume = true,
        .volume      = audio_output_get_volume(),
        .backlight   = display_get_backlight(),
        .page        = 0,
        .page_count  = 1,
        .sel_row     = sel,
        .row_count   = cat_count > PAGE_ROWS ? PAGE_ROWS : cat_count,
    };

    for (int i = 0; i < a.row_count; i++) {
        snprintf(a.lines[i], sizeof(a.lines[0]), "%s    %d",
                 system_name(cats[i].system), cats[i].count);
    }
    display_stream_sync(draw_strip, &a);
}

static void draw_games(int count, const category_t *cat, int sel)
{
    int page = sel / PAGE_ROWS;
    int first = page * PAGE_ROWS;
    int last = first + PAGE_ROWS;
    if (last > cat->count) last = cat->count;

    draw_args_t a = {
        /* 标题就是平台名，所以行里不再重复画平台徽标，省下的宽度给名字。 */
        .title       = system_name(cat->system),
        .footer      = "A开始  B返回  Y亮度  左右翻页",
        .show_volume = false,
        .backlight   = display_get_backlight(),
        .page        = page,
        .page_count  = (cat->count + PAGE_ROWS - 1) / PAGE_ROWS,
        .sel_row     = sel - first,
        .row_count   = last - first,
    };

    for (int j = first; j < last; j++) {
        int i = nth_of_system(count, cat->system, j);
        const rom_store_entry_t *e = i >= 0 ? rom_store_entry(i) : NULL;
        if (!e) break;

        /* ROM 目录位于 flash mmap，格式化也会使用较深的 libc 调用栈。都在
         * 菜单任务里先完成，核 1 的推屏回调只读取这份栈上快照，避免长列表
         * 页面令 3 KB 推屏任务栈承受目录访问和 snprintf。
         *
         * 编号是页内序号，每个平台都从 01 起——它只是行号，接着全局编号
         * 一路数下去反而看不出这是第几个。 */
        char *line = a.lines[j - first];
        snprintf(line, sizeof(a.lines[0]), "%02d %s", j + 1, e->name);
        line[LINE_CHARS_MAX] = '\0';
    }
    display_stream_sync(draw_strip, &a);
}

bool rom_menu_pick(const rom_store_entry_t **entry, uint16_t *launch_keys)
{
    int count = rom_store_init();
    if (count <= 0) {
        ESP_LOGW(TAG, "roms 分区里没有游戏，用编译期嵌入的那个");
        return false;
    }

    category_t cats[SYSTEM_COUNT];
    int cat_count = build_categories(count, cats, SYSTEM_COUNT);
    if (cat_count <= 0) {       /* count > 0 就不该发生，稳妥起见 */
        ESP_LOGW(TAG, "目录里一个平台都认不出来，用编译期嵌入的那个");
        return false;
    }

    /* 三路输入并存：飞线手柄、USB HID、串口调试键盘。init 都是幂等的，
     * 模拟器启动后再调一次没有副作用。 */
    input_serial_init();
    input_usb_init();
    input_gamepad_init();

    printf("\n开机选单：%d 个游戏，%d 个平台。\n", count, cat_count);
    printf("摇杆上下选，A 进入/确认，B 返回上一级。\n");
    printf("（想换游戏按板子上的 RST 重启）\n\n");

    int cat = 0;
    int sel[SYSTEM_COUNT] = { 0 };  /* 每个平台各记各的，退出去再进来回原位 */
    bool in_games = false;

    uint16_t prev = poll_input();   /* 先读一次当基线：上电时可能有键按着 */
    draw_categories(cats, cat_count, cat);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        uint16_t now = poll_input();
        uint16_t edge = now & ~prev;    /* 这一帧新按下的位 */
        prev = now;

        bool dirty = false;

        /* Y 调背光，10% 一档循环，最暗 5%——不设到 0 是不想让屏幕全黑
         * （那样看不见菜单，没法确认调到哪一档了）。100% 那档单独钳位，
         * 不然 95+10=105 会直接跳过 100 冲到下一轮的 5%。不用 SELECT 是想
         * 把它留给以后可能加的系统级组合键（比如 SELECT+START 长按待机）。
         * 两页行为一致，所以放在分页之前。 */
        if (edge & GAMEPAD_BIT_Y) {
            int backlight = display_get_backlight();
            backlight = (backlight >= 100) ? 5 : backlight + 10;
            if (backlight > 100) backlight = 100;
            display_backlight(backlight);
            dirty = true;

        } else if (!in_games) {
            /* ---- 分类页 ---- */

            /* 这一页没有上一级可退，B 就继续当音量键：每按一次加 10%，
             * 到 100% 再按一次绕回 0%（和背光那档同一套循环手感）。进游戏
             * 后仍完整保留原来的 B（跑/发射），重启则总是恢复默认档位。 */
            if (edge & NES_PAD_B) {
                int volume = audio_output_get_volume() + 10;
                if (volume > 100) volume = 0;
                audio_output_set_volume(volume);
                dirty = true;

            } else if (edge & (NES_PAD_A | NES_PAD_START)) {
                in_games = true;
                dirty = true;

            } else {
                int moved = 0;
                if (edge & NES_PAD_UP)   moved = -1;
                if (edge & NES_PAD_DOWN) moved = +1;
                if (moved) {
                    cat = (cat + moved + cat_count) % cat_count;
                    dirty = true;
                }
            }

        } else {
            /* ---- 游戏列表 ---- */
            const category_t *c = &cats[cat];
            int page_count = (c->count + PAGE_ROWS - 1) / PAGE_ROWS;

            if (edge & NES_PAD_B) {
                in_games = false;
                dirty = true;

            } else if (edge & (NES_PAD_A | NES_PAD_START)) {
                int i = nth_of_system(count, c->system, sel[cat]);
                const rom_store_entry_t *e = i >= 0 ? rom_store_entry(i) : NULL;
                if (e) {
                    *entry = e;
                    *launch_keys = now;
                    printf("选中：[%s] %s（ROM %u KB，分区 %u KB）\n\n",
                           system_name(e->system), e->name,
                           (unsigned)(e->size / 1024),
                           (unsigned)(e->stored_size / 1024));
                    return true;
                }

            } else if (page_count > 1 && (edge & (NES_PAD_LEFT | NES_PAD_RIGHT))) {
                /* 左右直接翻一整页，并尽量保留当前行。最后一页不足 10 项时，
                 * 同一行不存在就落到最后一项；页首和页尾之间同样可以环绕。
                 * 这一支排在上下之前单独走，是为了让斜推摇杆时以翻页为准，
                 * 避免同时又上下移动一格。
                 *
                 * ⚠ `page_count > 1` 这个前置条件不能去掉。摇杆是模拟量，往上
                 * 推很容易同时越过左/右阈值，这一帧的 edge 就是 UP|RIGHT。只有
                 * 一页时翻页是空操作（(0±1+1)%1 == 0），但它排在前面会把上下
                 * 吃掉——整个列表看着就像按键失灵。分平台之前列表是 31 个游戏
                 * 铺 4 页，翻页总有事可做所以没暴露；拆成平台之后 GB/GBC/SNES
                 * 都只有一页，全都中招。 */
                int page = sel[cat] / PAGE_ROWS;
                int row = sel[cat] % PAGE_ROWS;
                int page_delta = (edge & NES_PAD_LEFT) ? -1 : +1;

                page = (page + page_delta + page_count) % page_count;
                sel[cat] = page * PAGE_ROWS + row;
                if (sel[cat] >= c->count) sel[cat] = c->count - 1;
                dirty = true;

            } else {
                /* 上下环绕。跨过页边界时 draw_games() 会按新的 sel 自动切页。 */
                int moved = 0;
                if (edge & NES_PAD_UP)   moved = -1;
                if (edge & NES_PAD_DOWN) moved = +1;
                if (moved) {
                    sel[cat] = (sel[cat] + moved + c->count) % c->count;
                    dirty = true;
                }
            }
        }

        /* 统一在这里重画：省得每个分支各写一遍，还要各自挑对是哪一页。 */
        if (dirty) {
            if (in_games) draw_games(count, &cats[cat], sel[cat]);
            else          draw_categories(cats, cat_count, cat);
        }
    }
}
