/*
 * 开机选单的实现
 *
 * ---- 两级结构 ----
 *
 * 开机页 <--B-- 分类页（选平台）--A--> 游戏列表（选游戏）--A--> 启动
 *                  ^                      |
 *                  +----------B-----------+
 *
 * 两页共用同一个轮询循环、同一份边沿检测和同一个条带回调。平台页用 2x3
 * 卡片网格，游戏页用带编号徽标的列表；draw_*() 各自填好 draw_args_t 快照，
 * 回调只负责按当前页面类型绘制。
 *
 * B 在两页都只有“返回上一级”一个含义，避免孩子记两套规则。音量、亮度和
 * 手柄测试统一收进开机主页的 SETTINGS。平台页 SELECT 打开跨平台收藏，
 * 游戏列表 X 收藏/取消；收藏复用列表页，六个平台仍保持原来的卡片布局。
 *
 * ---- 布局 ----
 *
 * 画布是 288x224（NES 画布尺寸，居中在 320x240 的屏上，见 display.h）。
 * 游戏列表每页 8 项 x 21px 行距 = 168px，给选中框和分隔线留出呼吸感；
 * 平台页最多 6 类，正好排成 2 列 x 3 行。
 *
 * 选中项用反白（填充块 + 浅色字），比单独一个箭头更醒目。中文是 16x16
 * 点阵，有限的 288x224 画布仍能同时容纳标题、8 行列表和操作提示。
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
#include "rom_favorites.h"
#include "display.h"
#include "input_serial.h"
#include "input_gamepad.h"
#include "input_usb.h"
#include "ui_sound.h"
#include "nofrendo.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "menu";

#define TITLE_Y        5
#define HEADER_LINE_Y  25
#define LIST_Y         32      /* 第一行文字基线 */
#define ROW_H          21      /* 16px 点阵 + 选中框和分隔线 */
#define PAGE_ROWS      8
#define FOOTER_LINE_Y  204
#define FOOTER_Y       207
#define TEXT_X         8       /* 文字左缩进 */

#define CARD_MARGIN_X  8
#define CARD_GAP_X     8
#define CARD_GAP_Y     6
#define CARD_W         132
#define CARD_H         48
#define CARD_Y         35

/* 六个平台，正好填满平台页的 2 列 x 3 行卡片网格。ZIP 不再单列一类
 * （见 rom_store.h 的 ROM_SYSTEM_LAST 注释）。 */
#define SYSTEM_COUNT   6

#define POLL_MS     16      /* 约 60 Hz，和游戏帧率一个量级 */

static uint16_t poll_input(void)
{
    return input_serial_poll() | input_gamepad_poll() | input_usb_poll();
}

typedef struct {
    rom_system_t system;
    int          count;
    bool         favorites;
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
    char  meta[32];          /* 标题右侧：总数或总数 + 页码 */
    bool  category_grid;
    int   page;             /* 0 基；page_count <= 1 时整个页码都不画 */
    int   page_count;
    int   sel_row;          /* 反白哪一行（页内行号） */
    int   row_count;
    int   first_number;     /* 游戏列表第一项的 1 基编号 */
    rom_system_t systems[SYSTEM_COUNT];
    int   counts[SYSTEM_COUNT];
    char  lines[PAGE_ROWS][64];
    bool  favorites[PAGE_ROWS];
    const char *empty_text;
} draw_args_t;

static const char *system_name(rom_system_t system)
{
    if (system == ROM_SYSTEM_SNES) return "SNES";
    if (system == ROM_SYSTEM_GENESIS) return "MD";
    if (system == ROM_SYSTEM_GBC) return "GBC";
    if (system == ROM_SYSTEM_GB) return "GB";
    if (system == ROM_SYSTEM_PCE) return "PCE";
    return "NES";
}

/* 平台色见 display.h 的 C_SYS_*。和 system_name() 分成两个函数而不是一张
 * 结构体表：这两处的 switch 顺序不一样（名字按长度、颜色按平台世代），
 * 合表反而要多记一层对应关系。 */
static uint16_t system_color(rom_system_t system)
{
    switch (system) {
    case ROM_SYSTEM_GB:      return C_SYS_GB;
    case ROM_SYSTEM_GBC:     return C_SYS_GBC;
    case ROM_SYSTEM_SNES:    return C_SYS_SNES;
    case ROM_SYSTEM_GENESIS: return C_SYS_GENESIS;
    case ROM_SYSTEM_PCE:     return C_SYS_PCE;
    default:                 return C_SYS_NES;
    }
}

/* 平台卡片右侧的小图标：画的是**主机/手柄的外形**，不是厂商 logo。
 * 32x26 的格子，只用矩形和填充，和 main.c 的 boot_draw_icon() 一个路子。
 *
 * 卡片 132 宽，「999 GAMES」那行最宽到 x+85，右侧正好剩下这块地方。
 * 色相跟左边那条色条一致（选中时整卡已经是平台色，改用反白）。 */
#define ICON_W  32
#define ICON_H  26

static void draw_system_icon(rom_system_t system, int ix, int iy, uint16_t c)
{
    switch (system) {
    case ROM_SYSTEM_NES:        /* 横手柄：十字 + 两个方键 */
        display_rect(ix, iy + 5, ICON_W, 16, c);
        display_fill_rect(ix + 4, iy + 11, 9, 3, c);
        display_fill_rect(ix + 7, iy + 8, 3, 9, c);
        display_fill_rect(ix + 21, iy + 11, 4, 4, c);
        display_fill_rect(ix + 27, iy + 11, 4, 4, c);
        break;

    case ROM_SYSTEM_SNES:       /* 手柄：十字 + 菱形四键 */
        display_rect(ix, iy + 4, ICON_W, 18, c);
        display_fill_rect(ix + 4, iy + 12, 8, 3, c);
        display_fill_rect(ix + 7, iy + 9, 3, 9, c);
        display_fill_rect(ix + 24, iy + 8, 3, 3, c);
        display_fill_rect(ix + 20, iy + 12, 3, 3, c);
        display_fill_rect(ix + 28, iy + 12, 3, 3, c);
        display_fill_rect(ix + 24, iy + 16, 3, 3, c);
        break;

    case ROM_SYSTEM_GB:         /* 竖掌机：小屏 + 竖排按键 */
        display_rect(ix + 6, iy, 20, ICON_H, c);
        display_rect(ix + 9, iy + 3, 14, 9, c);
        display_fill_rect(ix + 9, iy + 18, 6, 2, c);
        display_fill_rect(ix + 11, iy + 16, 2, 6, c);
        display_fill_rect(ix + 18, iy + 19, 3, 3, c);
        display_fill_rect(ix + 22, iy + 16, 3, 3, c);
        break;

    case ROM_SYSTEM_GBC:        /* 掌机：机身更方、屏幕更大，和 GB 拉开区别 */
        display_rect(ix + 4, iy, 24, ICON_H, c);
        display_fill_rect(ix + 7, iy + 3, 18, 11, c);
        display_fill_rect(ix + 7, iy + 19, 6, 2, c);
        display_fill_rect(ix + 9, iy + 17, 2, 6, c);
        display_fill_rect(ix + 18, iy + 20, 3, 3, c);
        display_fill_rect(ix + 23, iy + 17, 3, 3, c);
        break;

    case ROM_SYSTEM_GENESIS:    /* 三键手柄：右侧三个键一排 */
        display_rect(ix, iy + 5, ICON_W, 16, c);
        display_fill_rect(ix + 4, iy + 11, 9, 3, c);
        display_fill_rect(ix + 7, iy + 8, 3, 9, c);
        display_fill_rect(ix + 18, iy + 12, 3, 3, c);
        display_fill_rect(ix + 23, iy + 12, 3, 3, c);
        display_fill_rect(ix + 28, iy + 12, 3, 3, c);
        break;

    case ROM_SYSTEM_PCE:
        /* 画 HuCard 而不是手柄。第一版画的是小手柄，渲染出来和 NES 那个
         * 几乎一样 —— 都是横机身 + 十字 + 两个键，只差一点大小，扫一眼分
         * 不开。PC Engine 最有辨识度的是那张信用卡大小的卡带，外形和其余
         * 五个完全不撞。 */
        display_rect(ix + 2, iy + 4, 28, 18, c);
        display_fill_rect(ix + 5, iy + 7, 3, 12, c);   /* 触点 */
        display_fill_rect(ix + 10, iy + 7, 3, 12, c);
        display_fill_rect(ix + 17, iy + 9, 10, 2, c);  /* 标签条 */
        display_fill_rect(ix + 17, iy + 14, 7, 2, c);
        break;
    }
}

static void draw_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const draw_args_t *a = ctx;

    /* 配色全部走 display.h 的语义层，别在这里直接写 C_GVB_* 或新色。 */
    display_clear(C_UI_BG);

    display_rect(0, 0, DISP_FB_W, DISP_FB_H, C_UI_EDGE);
    display_text_16(TEXT_X, TITLE_Y, a->title, C_UI_FG);
    int meta_x = DISP_FB_W - TEXT_X - display_text_width_16(a->meta);
    display_text_16(meta_x, TITLE_Y, a->meta, C_UI_FG_DIM);
    display_fill_rect(TEXT_X, HEADER_LINE_Y, DISP_FB_W - 2 * TEXT_X, 1,
                      C_UI_LINE);

    if (a->category_grid) {
        for (int i = 0; i < a->row_count; i++) {
            int col = i % 2;
            int row = i / 2;
            int x = CARD_MARGIN_X + col * (CARD_W + CARD_GAP_X);
            int y = CARD_Y + row * (CARD_H + CARD_GAP_Y);
            bool active = i == a->sel_row;
            uint16_t accent = system_color(a->systems[i]);

            /* 选中的卡片整块铺平台色，没选中的只在左边留一条 5px 色条：
               六张卡同时铺满色会花，色条已经够认平台。 */
            display_fill_rect(x, y, CARD_W, CARD_H, active ? accent : C_UI_PANEL);
            display_rect(x, y, CARD_W, CARD_H,
                         active ? C_UI_SEL_EDGE : C_UI_EDGE);
            if (!active) display_fill_rect(x + 1, y + 1, 5, CARD_H - 2, accent);

            display_text_16(x + 13, y + 6, system_name(a->systems[i]),
                            active ? C_UI_FG_INV : C_UI_FG);

            char count_text[24];
            snprintf(count_text, sizeof(count_text), "%d GAMES", a->counts[i]);
            display_text_16(x + 13, y + 27, count_text,
                            active ? C_UI_FG_INV_DIM : C_UI_FG_DIM);

            draw_system_icon(a->systems[i], x + CARD_W - ICON_W - 6,
                             y + (CARD_H - ICON_H) / 2,
                             active ? C_UI_FG_INV : accent);
        }
    } else {
        if (a->empty_text) {
            display_text_16((DISP_FB_W - display_text_width_16(a->empty_text)) / 2,
                            91, a->empty_text, C_UI_FG_DIM);
            const char *hint = "在游戏列表按 X 收藏";
            display_text_16((DISP_FB_W - display_text_width_16(hint)) / 2,
                            119, hint, C_UI_FG_FAINT);
        }
        for (int row = 0; row < a->row_count; row++) {
            int y = LIST_Y + row * ROW_H;
            bool active = row == a->sel_row;

            if (active) {
                display_fill_rect(6, y - 3, DISP_FB_W - 12, ROW_H - 1, C_UI_SEL);
            } else if (row + 1 < a->row_count) {
                display_fill_rect(TEXT_X, y + 17, DISP_FB_W - 2 * TEXT_X, 1,
                                  C_UI_LINE);
            }

            /* 编号徽标在选中行里要比行底更深才分得出来，所以用 SEL_EDGE。 */
            display_fill_rect(TEXT_X, y - 1, 31, 17,
                              active ? C_UI_SEL_EDGE : C_UI_PANEL_ALT);
            char number[8];
            snprintf(number, sizeof(number), "%02d", a->first_number + row);
            display_text_16(TEXT_X + 6, y, number,
                            active ? C_UI_FG_INV : C_UI_FG_DIM);
            display_text_16(46, y, a->lines[row],
                            active ? C_UI_FG_INV : C_UI_FG);
            if (a->favorites[row]) {
                display_text_16(DISP_FB_W - 25, y, "★",
                                active ? C_UI_FG_INV : C_UI_GOLD);
            } else if (active) {
                display_text_16(DISP_FB_W - 17, y, ">", C_UI_FG_INV);
            }
        }
    }

    display_fill_rect(TEXT_X, FOOTER_LINE_Y, DISP_FB_W - 2 * TEXT_X, 1,
                      C_UI_LINE);
    display_text_16((DISP_FB_W - display_text_width_16(a->footer)) / 2, FOOTER_Y,
                    a->footer, C_UI_FG_FAINT);
}

/* 统计每个平台各有多少游戏，返回分类数。
 *
 * 故意不记录「起始下标 + 长度」：那等于把「同平台条目在 rom_store 里连续」
 * 变成硬约束。目前 pack_roms.py 确实按 system 排过序，但没必要让菜单依赖它
 * ——下面 nth_of_category() 只读内存目录，收藏同样不碰 ROM 内容。 */
static int build_categories(int count, category_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        const rom_store_entry_t *e = rom_store_entry(i);
        if (!e) break;

        int k = 0;
        while (k < n && out[k].system != e->system) k++;
        if (k == n) {
            if (n >= max) continue;     /* 不该发生：枚举就六个取值 */
            out[n].system = e->system;
            out[n].count  = 0;
            out[n].favorites = false;
            n++;
        }
        out[k].count++;
    }
    return n;
}

/* 收藏只匹配当前目录中的游戏；拔掉的文件不会显示，但收藏记录仍保留，
 * 下次同路径放回即可恢复。普通平台与收藏共用定位，启动不会串到别的游戏。 */
static int nth_of_category(int count, const category_t *cat, int j)
{
    for (int i = 0; i < count; i++) {
        const rom_store_entry_t *e = rom_store_entry(i);
        if (!e) break;
        bool matches = cat->favorites ? rom_favorites_contains(e)
                                      : e->system == cat->system;
        if (matches && j-- == 0) return i;
    }
    return -1;
}

static int count_favorites(int count)
{
    int favorites = 0;
    for (int i = 0; i < count; i++) {
        if (rom_favorites_contains(rom_store_entry(i))) favorites++;
    }
    return favorites;
}

static void draw_categories(const category_t *cats, int cat_count, int sel,
                            int favorite_count, const char *status)
{
    draw_args_t a = {
        .title       = "选择游戏平台",
        .footer      = status ? status : "A进入 B返回 SELECT收藏",
        .category_grid = true,
        .page        = 0,
        .page_count  = 1,
        .sel_row     = sel,
        .row_count   = cat_count,
    };

    int total = 0;
    for (int i = 0; i < a.row_count; i++) {
        a.systems[i] = cats[i].system;
        a.counts[i] = cats[i].count;
        total += cats[i].count;
    }
    snprintf(a.meta, sizeof(a.meta), "%d款 收藏%d", total, favorite_count);
    display_stream_sync(draw_strip, &a);
}

/* 游戏名要在星标前结束；按完整 UTF-8 字符收短，避免中文半个字变成问号。
 * 必须在菜单任务里处理，条带回调只接收已经裁好的文本。 */
static void fit_game_line(char *line)
{
    size_t len = strlen(line);
    while (len && display_text_width_16(line) > DISP_FB_W - 32 - 46) {
        do { len--; } while (len && ((unsigned char)line[len] & 0xc0) == 0x80);
        line[len] = '\0';
    }
}

static void draw_games(int count, const category_t *cat, int sel, const char *status)
{
    int page = sel / PAGE_ROWS;
    int first = page * PAGE_ROWS;
    int last = first + PAGE_ROWS;
    if (last > cat->count) last = cat->count;

    draw_args_t a = {
        /* 标题就是平台名，所以行里不再重复画平台徽标，省下的宽度给名字。 */
        .title       = cat->favorites ? "我的收藏" : system_name(cat->system),
        .footer      = "A开始 B返回 X收藏 左右翻页",
        .category_grid = false,
        .page        = page,
        .page_count  = (cat->count + PAGE_ROWS - 1) / PAGE_ROWS,
        .sel_row     = sel - first,
        .row_count   = last - first,
        .first_number = first + 1,
        .empty_text = cat->count == 0 ? "暂无可用收藏" : NULL,
    };
    if (cat->count) {
        snprintf(a.meta, sizeof(a.meta), "%d GAMES %d/%d", cat->count,
                 page + 1, a.page_count);
    } else {
        snprintf(a.meta, sizeof(a.meta), "0 GAMES");
        a.footer = "B返回平台";
    }

    for (int j = first; j < last; j++) {
        int i = nth_of_category(count, cat, j);
        const rom_store_entry_t *e = i >= 0 ? rom_store_entry(i) : NULL;
        if (!e) break;

        /* ROM 目录位于 PSRAM，格式化也会使用较深的 libc 调用栈。都在
         * 菜单任务里先完成，核 1 的推屏回调只读取这份栈上快照，避免长列表
         * 页面令 3 KB 推屏任务栈承受目录访问和 snprintf。
         *
         * 编号是平台内序号，每个平台都从 01 起；翻页后继续递增，不能接着
         * 全局目录编号一路数，否则看不出这是该平台里的第几个。 */
        char *line = a.lines[j - first];
        if (cat->favorites) {
            snprintf(line, sizeof(a.lines[0]), "[%s] %s", system_name(e->system),
                     e->name);
        } else {
            snprintf(line, sizeof(a.lines[0]), "%s", e->name);
        }
        fit_game_line(line);
        a.favorites[j - first] = rom_favorites_contains(e);
    }
    if (cat->count && a.favorites[a.sel_row]) {
        a.footer = "A开始 B返回 X取消 左右翻页";
    }
    if (status) a.footer = status;
    display_stream_sync(draw_strip, &a);
}

rom_menu_result_t rom_menu_pick(const rom_store_entry_t **entry)
{
    int count = rom_store_init(false);
    if (count <= 0) {
        ESP_LOGW(TAG, "TF 卡上没有游戏，用编译期嵌入的那个");
        return ROM_MENU_FALLBACK;
    }

    category_t cats[SYSTEM_COUNT];
    int cat_count = build_categories(count, cats, SYSTEM_COUNT);
    if (cat_count <= 0) {       /* count > 0 就不该发生，稳妥起见 */
        ESP_LOGW(TAG, "目录里一个平台都认不出来，用编译期嵌入的那个");
        return ROM_MENU_FALLBACK;
    }

    esp_err_t favorite_error = rom_favorites_init();
    category_t favorites = { .favorites = true, .count = count_favorites(count) };
    const char *status = favorite_error == ESP_OK ? NULL : "收藏读取失败，请重试";

    /* 三路输入并存：飞线手柄、USB HID、串口调试键盘。init 都是幂等的，
     * 模拟器启动后再调一次没有副作用。 */
    input_serial_init();
    input_usb_init();
    input_gamepad_init();

    printf("\n开机选单：%d 个游戏，%d 个平台。\n", count, cat_count);
    printf("方向键选择，A 进入/确认，B 返回上一级。\n");
    printf("平台页 SELECT（Tab）打开收藏；游戏列表 X（U）收藏/取消。\n");
    printf("（想换游戏按板子上的 RST 重启）\n\n");

    int cat = 0;
    int sel[SYSTEM_COUNT] = { 0 };  /* 每个平台各记各的，退出去再进来回原位 */
    int favorite_sel = 0;
    bool in_games = false;
    bool in_favorites = false;

    uint16_t prev = poll_input();   /* 先读一次当基线：上电时可能有键按着 */
    draw_categories(cats, cat_count, cat, favorites.count, status);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        uint16_t now = poll_input();
        uint16_t edge = now & ~prev;    /* 这一帧新按下的位 */
        prev = now;

        bool dirty = false;
        if (edge) status = NULL;

        if (!in_games) {
            /* ---- 分类页 ---- */

            /* 分类页的上一级就是 GAME/WORDS/SETTINGS 开机页。用独立返回值告诉
             * app_main，不能冒充“没有 ROM”，否则会误启动编译期内置游戏。 */
            if (edge & NES_PAD_B) {
                ui_sound_back();
                /* 返回首页仍保留已读收藏，下一次进入 GAME 不应再等一次读卡。
                 * 真正启动模拟器时才释放，给游戏的大块 PSRAM 分配让路。 */
                return ROM_MENU_BACK;

            } else if (edge & NES_PAD_SELECT) {
                ui_sound_enter();
                in_games = true;
                in_favorites = true;
                /* 短暂读卡/内存错误后，重新进入收藏就能重试，不必先重启机器。 */
                if (favorite_error != ESP_OK) {
                    favorite_error = rom_favorites_init();
                    favorites.count = count_favorites(count);
                }
                if (favorite_error != ESP_OK) status = "收藏读取失败，请重试";
                dirty = true;

            } else if (edge & (NES_PAD_A | NES_PAD_START)) {
                ui_sound_enter();
                in_games = true;
                in_favorites = false;
                dirty = true;

            } else if (edge & (NES_PAD_LEFT | NES_PAD_RIGHT)) {
                /* 卡片按行排列：左右切同一行。最后一行只有左卡时，向右不动，
                 * 不突然跳去别的行，孩子看到的移动方向和光标完全一致。 */
                int target = cat + ((edge & NES_PAD_LEFT) ? -1 : 1);
                if (target >= 0 && target < cat_count && target / 2 == cat / 2) {
                    cat = target;
                    dirty = true;
                }
            } else if (edge & (NES_PAD_UP | NES_PAD_DOWN)) {
                /* 上下切同一列，到边缘后在该列内环绕。 */
                int col = cat % 2;
                int target = cat + ((edge & NES_PAD_UP) ? -2 : 2);
                if (target < 0) {
                    target = cat_count - 1;
                    while (target >= 0 && target % 2 != col) target--;
                } else if (target >= cat_count) {
                    target = col;
                }
                if (target >= 0 && target < cat_count) {
                    cat = target;
                    dirty = true;
                }
            }

        } else {
            /* ---- 游戏列表 ---- */
            const category_t *c = in_favorites ? &favorites : &cats[cat];
            int *selected = in_favorites ? &favorite_sel : &sel[cat];
            int page_count = (c->count + PAGE_ROWS - 1) / PAGE_ROWS;

            if (edge & NES_PAD_B) {
                ui_sound_back();
                in_games = false;
                dirty = true;

            } else if (c->count > 0 && (edge & GAMEPAD_BIT_X)) {
                int i = nth_of_category(count, c, *selected);
                const rom_store_entry_t *e = i >= 0 ? rom_store_entry(i) : NULL;
                if (e) {
                    /* 慢卡的写入可能超过一帧，先给出反馈；落盘成功才让星标改变。 */
                    draw_games(count, c, *selected, "正在保存收藏");
                    if (favorite_error != ESP_OK) {
                        favorite_error = rom_favorites_init();
                        favorites.count = count_favorites(count);
                    }
                    esp_err_t err = favorite_error == ESP_OK
                                  ? rom_favorites_toggle(e) : favorite_error;
                    if (err == ESP_OK) {
                        favorite_error = ESP_OK;
                        bool added = rom_favorites_contains(e);
                        status = added ? "已收藏" : "已取消收藏";
                        if (added) ui_sound_enter();
                        else       ui_sound_back();
                        favorites.count = count_favorites(count);
                        /* 删除当前行后跟随下一项；删掉末项则退一格。空列表保留在
                         * 收藏页，不能取模 0，也不能意外启动内置游戏。 */
                        if (favorite_sel >= favorites.count) {
                            favorite_sel = favorites.count > 0 ? favorites.count - 1 : 0;
                        }
                        ESP_LOGI(TAG, "%s：[%s] %s（当前目录收藏 %d）", status,
                                 system_name(e->system), e->name, favorites.count);
                    } else {
                        status = "收藏保存失败，请重试";
                        ESP_LOGW(TAG, "收藏保存失败：%s", esp_err_to_name(err));
                    }
                    /* 把写卡期间仍按住的键纳入基线，避免返回后立即重复收藏。 */
                    prev = poll_input();
                    dirty = true;
                }

            } else if (c->count > 0 && (edge & (NES_PAD_A | NES_PAD_START))) {
                ui_sound_enter();
                int i = nth_of_category(count, c, *selected);
                const rom_store_entry_t *e = i >= 0 ? rom_store_entry(i) : NULL;
                if (e) {
                    *entry = e;
                    if (e->storage == ROM_STORAGE_ZIP_PENDING) {
                        printf("选中：[%s] %s（ZIP，按需识别）\n  %s\n\n",
                               system_name(e->system), e->name, e->path);
                    } else {
                        printf("选中：[%s] %s（%u KB）\n  %s\n\n",
                               system_name(e->system), e->name,
                               (unsigned)(e->size / 1024), e->path);
                    }
                    rom_favorites_deinit();
                    return ROM_MENU_SELECTED;
                }

            } else if (page_count > 1 && (edge & (NES_PAD_LEFT | NES_PAD_RIGHT))) {
                /* 左右直接翻一整页，并尽量保留当前行。最后一页不足 8 项时，
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
                int page = *selected / PAGE_ROWS;
                int row = *selected % PAGE_ROWS;
                int page_delta = (edge & NES_PAD_LEFT) ? -1 : +1;

                page = (page + page_delta + page_count) % page_count;
                *selected = page * PAGE_ROWS + row;
                if (*selected >= c->count) *selected = c->count - 1;
                dirty = true;

            } else {
                /* 上下环绕。跨过页边界时 draw_games() 会按新的 sel 自动切页。 */
                int moved = 0;
                if (edge & NES_PAD_UP)   moved = -1;
                if (edge & NES_PAD_DOWN) moved = +1;
                if (moved && c->count > 0) {
                    *selected = (*selected + moved + c->count) % c->count;
                    dirty = true;
                }
            }
        }

        /* 统一在这里重画：省得每个分支各写一遍，还要各自挑对是哪一页。 */
        if (dirty) {
            if (in_games) {
                draw_games(count, in_favorites ? &favorites : &cats[cat],
                           in_favorites ? favorite_sel : sel[cat], status);
            } else {
                draw_categories(cats, cat_count, cat, favorites.count, status);
            }
        }
    }
}
