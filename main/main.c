/*
 * 在 ESP32-S3 + ST7789（240x320）上运行 NES / GB / GBC / SNES / Genesis
 *
 * 流程：启动等待页 -> 完成输入、游戏目录和收藏准备 -> GAME/WORDS/SETTINGS
 *
 * 接线见 display.h 顶部。换屏或显示不正常时改那里的宏，不用动这个文件。
 * 把 SHOW_DISPLAY_SELFTEST 改成 1 可以在启动模拟器前先跑一遍点屏诊断图。
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "display.h"
#include "audio_output.h"
#include "input_serial.h"
#include "input_usb.h"
#include "loading_screen.h"
#include "input_gamepad.h"
#include "nofrendo.h"
#include "nes_emu.h"
#include "pce_emu.h"
#include "gbc_emu.h"
#include "snes_emu.h"
#include "genesis_emu.h"
#include "rom_menu.h"
#include "rom_favorites.h"
#include "ui_sound.h"
#include "overclock.h"
#include "sd_card.h"
#include "word_study.h"
#include "word_audio.h"

static const char *TAG = "main";

#define SHOW_DISPLAY_SELFTEST  0

/* TF 卡自检：在挂载之后多跑一遍「列根目录 + 写读校验」，只往串口输出。
 * ROM 现在全部从卡上读，挂载本身是必做的（rom_store_init 会调）；这个开关
 * 只控制那些额外的诊断输出，在慢卡上要多花两秒。**换卡或改接线时打开**，
 * 平时关着。见 sd_card.c 文件头。 */
#define SD_SELFTEST 0

/* 超频实验开关，见 overclock.h。0 = 不动寄存器，维持 Kconfig 里配置的 240MHz；
 * 非零走 overclock_apply()，档位范围 [-8, 8]，实测主频打印在串口日志的
 * "overclock" tag 下。先从这个值开始测，稳定的话再往上加、不稳就往下退——
 * 一次只改这一个数，配合 nes_emu.c 每秒自报的 "CPU 余量" 那行看效果。 */
#define OVERCLOCK_LEVEL 4

static void print_board_info(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);

    printf("\n========= ESP32-S3 GAMEBOX =========\n");
    printf("版本      : v%s\n", esp_app_get_description()->version);
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

/* 首页字标：七个字母各占一个 gruvbox 色相。
 *
 * GAMEBOX 正好 7 个字母，gruvbox 正好 7 个色相，一人一个按光谱排下来——
 * 这是「用整套 gruvbox」最省事又最像回事的地方。
 *
 * 字形放大 4 倍（20x28），靠字号本身撑场面，不加任何底。试过两版加底的：
 * 七块彩色键帽——单看活泼，但下面 GAME/WORDS/SETTINGS 三张卡片本来就有
 * 三个色相，七块彩色摞上去整页就散了；一块深色牌匾——够聚焦，但那块黑
 * 在整屏暖底里太突兀，纯粹是为了给字找个衬底，字放大之后就不需要了。
 *
 * 阴影往右下偏 2px 用 LIGHT3：浅底上投影只能投比背景深一点点的暖灰，
 * 投黑（试过 DARK0 偏 3px）会和字本身一样重，整个字标发脏。 */
#define WORDMARK_SCALE  4
#define WORDMARK_H      28    /* 字形 7 行 x 4 倍 */
#define WORDMARK_SHADOW 2

/* 入场动画和模式选择页共用，两边都按这个底边排下面的元素。 */
static int wordmark_bottom(int y)
{
    return y + WORDMARK_H + WORDMARK_SHADOW;
}

/* dy 为每个字母的纵向偏移，用来做开机动画的逐字下落；传 NULL 就是静止。
 * 偏移等于 WORDMARK_HIDDEN 的字母整个不画。 */
#define WORDMARK_HIDDEN  (-9999)

static void draw_wordmark_ex(int y, const int *dy)
{
    static const char letters[] = "GAMEBOX";
    static const uint16_t hues[] = {
        C_GVB_FADED_RED,   C_GVB_FADED_ORANGE, C_GVB_FADED_YELLOW,
        C_GVB_FADED_GREEN, C_GVB_FADED_AQUA,   C_GVB_FADED_BLUE,
        C_GVB_FADED_PURPLE,
    };
    const int count = (int)(sizeof(letters) - 1);
    const int scale = WORDMARK_SCALE;

    /* 字形本体 5x7，第 6 列是字间距（见 display.c 的 FONT5X7）：整行宽度
     * 按步进算要减掉最后一列间距，居中才不会偏右。 */
    int text_w = count * 6 * scale - scale;
    int x0 = (DISP_FB_W - text_w) / 2;

    /* 阴影整排画完再画字面。当前步进 24px、字形 20px、偏移 2px，本来就
     * 不会串到下一个字上，但分两遍就不必依赖这个巧合。 */
    for (int i = 0; i < count; i++) {
        if (dy && dy[i] == WORDMARK_HIDDEN) continue;
        int oy = dy ? dy[i] : 0;
        char ch[2] = { letters[i], '\0' };
        display_text(x0 + i * 6 * scale + WORDMARK_SHADOW,
                     y + oy + WORDMARK_SHADOW, ch, C_GVB_LIGHT3, scale);
    }
    for (int i = 0; i < count; i++) {
        if (dy && dy[i] == WORDMARK_HIDDEN) continue;
        int oy = dy ? dy[i] : 0;
        char ch[2] = { letters[i], '\0' };
        display_text(x0 + i * 6 * scale, y + oy, ch, hues[i], scale);
    }
}

static void draw_wordmark(int y)
{
    draw_wordmark_ex(y, NULL);
}

/* 开机上电音。音符表和方波合成都在 ui_sound.c（菜单提示音共用同一套），
 * 这里只留按动画帧节奏喂数据的部分 —— 那段和动画循环绑在一起，挪不进通用模块。 */
#define CHIME_RATE   AUDIO_OUTPUT_SAMPLE_RATE
#define CHIME_CHUNK  256               /* 一次提交的立体声帧数 */

/* 字标在模式选择页的最终纵坐标。入场动画把字母落到这里，之后原地不动，
 * 所以这个值动画和菜单必须共用，改一处就够。 */
#define BOOT_WORDMARK_Y   9

/* ============ 模式选择页的入场动画 ============
 *
 * 以前这里是一张独立的开机画面（GAMEBOX + 平台列表 + loading...），停 1.5 秒
 * 之后才进模式选择页。去掉了，理由有三条：
 *
 *   - 紧接着的模式选择页会再画一遍同样的字标和副标题，等于连着看两次 logo；
 *   - 这机器靠重启换游戏（模拟器一进去就不返回），那段等待是**按次**付的，
 *     一晚上要付几十次；
 *   - 那屏上写着「5-IN-1」和五个平台标签，加了 PC Engine 之后就是错的。
 *
 * 现在改成：动画直接把模式选择页的字标送进场，字标落到的就是那页的最终位置
 * （y = BOOT_WORDMARK_Y），动画结束后它原地不动，菜单其余部分在它下面补齐 ——
 * 视觉上是一个连续的过程，不是两屏。
 *
 * 动画只在启动准备全部结束后播放，并立即接上模式选择页。输入初始化、
 * 游戏目录扫描和收藏读取期间统一显示等待页，不能先露出菜单再让它等读卡。
 *
 * ⚠ 整份绘制列表每帧会被逐条带调用 7 次（见 display.h），所以动画状态必须
 * 放在 ctx 里由调用方算好，不能在绘制函数里自增 —— 那样一帧之内七个条带
 * 会各自看到不同的状态，画面会横向撕成七段。 */
#define INTRO_CRT_END      9
#define INTRO_LETTER_STEP  3
#define INTRO_LETTER_DUR   9
#define INTRO_TOTAL        (INTRO_CRT_END + 6 * INTRO_LETTER_STEP + INTRO_LETTER_DUR + 2)

/* 第 i 个字母在第 f 帧的纵向偏移。二次缓出下落，末尾两帧压一下再弹回，
 * 比匀速落地有重量感。
 *
 * 落差取 30px 而不是更大：字标基线在 BOOT_WORDMARK_Y，落差再大起始点就跑到
 * 画布外，头两帧字母整个看不见 —— 渲染成序列图才发现 CRT 拉完和第一个字母
 * 露头之间空了一拍，而且字母入场时顶部被画布裁掉半截。 */
static int intro_letter_dy(int f, int i)
{
    int start = INTRO_CRT_END + i * INTRO_LETTER_STEP;
    if (f < start) return WORDMARK_HIDDEN;

    int t = f - start;
    if (t >= INTRO_LETTER_DUR) return 0;

    const int fall = INTRO_LETTER_DUR - 2;
    if (t < fall) {
        int r = fall - t;                 /* 剩余步数，二次缓出 */
        return -(30 * r * r) / (fall * fall);
    }
    return (t == fall) ? 3 : 1;           /* 触底压一下再回正 */
}

static void intro_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    int f = *(const int *)ctx;

    /* 黑底上一条亮带从中心纵向拉开，像 CRT 通电。上下缘用 bright_yellow ——
     * gruvbox 那组本来就是配深色底的，这是全工程唯一用得上它的地方。 */
    if (f < INTRO_CRT_END) {
        display_clear(C_BLACK);
        int half = (DISP_FB_H / 2) * (f + 1) / INTRO_CRT_END;
        int top = DISP_FB_H / 2 - half;
        display_fill_rect(0, top, DISP_FB_W, half * 2, C_UI_BG);
        display_hline(0, top, DISP_FB_W, C_GVB_BRIGHT_YELLOW);
        display_hline(0, top + half * 2 - 1, DISP_FB_W, C_GVB_BRIGHT_YELLOW);
        return;
    }

    /* 底色和外框就是模式选择页的，动画结束后无缝接上。 */
    display_clear(C_UI_BG);
    display_rect(0, 0, DISP_FB_W, DISP_FB_H, C_UI_EDGE);

    int dy[7];
    for (int i = 0; i < 7; i++) dy[i] = intro_letter_dy(f, i);
    draw_wordmark_ex(BOOT_WORDMARK_Y, dy);
}

/* 已经该产出多少个采样了（按真实经过时间算）。 */
static uint32_t chime_due(int64_t t0, uint32_t total)
{
    int64_t us = esp_timer_get_time() - t0;
    if (us < 0) return 0;
    uint32_t due = (uint32_t)(us * CHIME_RATE / 1000000);
    return due > total ? total : due;
}

static void boot_intro(bool sound)
{
    /* 声音设备已在准备阶段初始化；提示音和入场动画一起表示可以开始操作。
     *
     * ⚠ 必须按真实经过时间分批喂，不能一次灌完：audio_output 的队列只有
     * AUDIO_QUEUE_FRAMES(4) 个包、约 88 ms，一次性提交剩下的会被直接丢掉
     * （那条接口队列满时丢包并计数，不阻塞）。 */
    uint32_t total = ui_sound_ms(UI_SOUND_BOOT, UI_SOUND_BOOT_COUNT)
                   * CHIME_RATE / 1000;
    uint32_t sent = 0;
    int64_t t0 = esp_timer_get_time();
    static int16_t chunk[CHIME_CHUNK * 2];

    for (int f = 0; f < INTRO_TOTAL; f++) {
        /* display_stream_sync() 阻塞到这一帧推完才返回，默认 288x224 画布实测
         * 约 13 ms/帧，所以循环本身就是节流器，整段约 0.49 秒。 */
        display_stream_sync(intro_strip, &f);

        if (!sound) continue;
        uint32_t due = chime_due(t0, total);
        while (sent < due) {
            size_t n = due - sent;
            if (n > CHIME_CHUNK) n = CHIME_CHUNK;
            ui_sound_render(chunk, n, sent, UI_SOUND_BOOT,
                            UI_SOUND_BOOT_COUNT, CHIME_RATE,
                            UI_SOUND_BOOT_PEAK, UI_DUTY_50);
            audio_output_submit_stereo(chunk, n);
            sent += (uint32_t)n;
        }
    }

    /* 音效比动画长几十毫秒，把尾巴喂完再走，否则收尾音会被切断。 */
    while (sound && sent < total) {
        uint32_t due = chime_due(t0, total);
        while (sent < due) {
            size_t n = due - sent;
            if (n > CHIME_CHUNK) n = CHIME_CHUNK;
            ui_sound_render(chunk, n, sent, UI_SOUND_BOOT,
                            UI_SOUND_BOOT_COUNT, CHIME_RATE,
                            UI_SOUND_BOOT_PEAK, UI_DUTY_50);
            audio_output_submit_stereo(chunk, n);
            sent += (uint32_t)n;
        }
        if (sent < total) vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* 等待页没有可操作控件，也不靠固定延时猜测什么时候完成。只有当前阶段的
 * 同步初始化返回后才进入下一步，慢卡完整扫描期间仍明确显示正在准备。 */
static void boot_prepare_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    (void)strip;
    (void)y0;
    (void)h;
    const char *stage = *(const char *const *)ctx;
    const char *title = "正在启动";
    const char *hint = "准备完成后自动进入";

    display_clear(C_UI_BG);
    display_rect(0, 0, DISP_FB_W, DISP_FB_H, C_UI_EDGE);
    display_text_16((DISP_FB_W - display_text_width_16(title)) / 2, 75,
                    title, C_UI_FG);
    display_text_16((DISP_FB_W - display_text_width_16(stage)) / 2, 105,
                    stage, C_UI_FG_DIM);
    display_text_16((DISP_FB_W - display_text_width_16(hint)) / 2, 167,
                    hint, C_UI_FG_FAINT);
}

static void boot_show_preparing(const char *stage)
{
    ESP_LOGI(TAG, "启动准备：%s", stage);
    display_stream_sync(boot_prepare_strip, &stage);
}

/* 准备结束后才进入可操作的模式选择页。GAME 的目录已经就绪，SETTINGS 的
 * Controller Test 同样复用这份目录，不再在用户确认之后阻塞扫描。 */
#define BOOT_MENU_POLL_MS 16   /* 和 rom_menu.c 的 POLL_MS 同一个量级 */

typedef enum {
    BOOT_MODE_GAME,
    BOOT_MODE_WORDS,
    BOOT_MODE_SETTINGS,
    BOOT_MODE_COUNT,
} boot_mode_t;

static void boot_text_center_ascii(int y, const char *text, uint16_t color, int scale)
{
    int width = (int)strlen(text) * 6 * scale;
    display_text((DISP_FB_W - width) / 2, y, text, color, scale);
}

static void boot_draw_icon(boot_mode_t mode, int cx, int y, uint16_t color)
{
    if (mode == BOOT_MODE_GAME) {
        /* 小手柄：十字键和两颗面键比文字更快让孩子认出“游戏”。 */
        display_rect(cx - 21, y + 3, 42, 23, color);
        display_fill_rect(cx - 14, y + 12, 13, 3, color);
        display_fill_rect(cx - 9, y + 7, 3, 13, color);
        display_fill_rect(cx + 7, y + 9, 4, 4, color);
        display_fill_rect(cx + 13, y + 15, 4, 4, color);
    } else if (mode == BOOT_MODE_WORDS) {
        /* 打开的书，中缝留一列底色，缩到 86px 卡片里仍然看得清。 */
        display_rect(cx - 21, y + 2, 20, 25, color);
        display_rect(cx + 1, y + 2, 20, 25, color);
        display_fill_rect(cx - 16, y + 8, 11, 2, color);
        display_fill_rect(cx + 5, y + 8, 11, 2, color);
        display_fill_rect(cx - 16, y + 14, 11, 2, color);
        display_fill_rect(cx + 5, y + 14, 11, 2, color);
    } else {
        /* 三条滑杆比齿轮在 42x29 的小区域里更清楚，也直接对应设置页内容。 */
        display_hline(cx - 18, y + 7, 36, color);
        display_hline(cx - 18, y + 15, 36, color);
        display_hline(cx - 18, y + 23, 36, color);
        display_fill_rect(cx - 8, y + 4, 4, 7, color);
        display_fill_rect(cx + 7, y + 12, 4, 7, color);
        display_fill_rect(cx - 2, y + 20, 4, 7, color);
    }
}

static void boot_menu_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    (void)strip;
    (void)y0;
    (void)h;
    const int *selected = ctx;
    static const char *labels[BOOT_MODE_COUNT] = { "GAME", "WORDS", "SETTINGS" };
    static const char *descriptions[BOOT_MODE_COUNT] = {
        "选择并启动游戏", "按教材学习单词", "声音 亮度 手柄测试"
    };

    /* 三个模式各一个色相，取的是 display.h 里 gruvbox 的 faded 强调色：
     * GAME 橙、WORDS 蓝、SETTINGS 青。只在本页出现，就近定义，但不自己
     * 调新色。 */
    static const uint16_t accents[BOOT_MODE_COUNT] = {
        C_GVB_FADED_ORANGE, C_GVB_FADED_BLUE, C_GVB_FADED_AQUA,
    };

    display_clear(C_UI_BG);
    display_rect(0, 0, DISP_FB_W, DISP_FB_H, C_UI_EDGE);
    /* 牌匾本身已经是一条足够重的分隔，原来标题下面那条 hline 再画就多余了。 */
    draw_wordmark(BOOT_WORDMARK_Y);
    const char *version = esp_app_get_description()->version;
    int version_w = ((int)strlen(version) + 1) * 6;
    display_text(DISP_FB_W - 8 - version_w, 5, "v", C_UI_FG_FAINT, 1);
    display_text(DISP_FB_W - 8 - version_w + 6, 5,
                 version, C_UI_FG_FAINT, 1);
    boot_text_center_ascii(wordmark_bottom(BOOT_WORDMARK_Y) + 5, "PLAY  LEARN  EXPLORE",
                           C_UI_FG_DIM, 1);
    int choose_w = display_text_width_16("选择模式");
    display_text_16((DISP_FB_W - choose_w) / 2, 64, "选择模式", C_UI_FG);

    for (int i = 0; i < BOOT_MODE_COUNT; i++) {
        const int card_w = 86;
        const int card_h = 68;
        int x = 8 + i * 93;
        int cx = x + card_w / 2;
        bool active = i == *selected;
        uint16_t color = active ? C_UI_FG_INV : accents[i];

        display_fill_rect(x, 87, card_w, card_h,
                          active ? accents[i] : C_UI_PANEL);
        display_rect(x, 87, card_w, card_h,
                     active ? C_UI_SEL_EDGE : C_UI_EDGE);
        boot_draw_icon((boot_mode_t)i, cx, 94, color);

        if (i == BOOT_MODE_SETTINGS) {
            int label_w = display_text_width_16(labels[i]);
            display_text_16(cx - label_w / 2, 130, labels[i], color);
        } else {
            int label_w = (int)strlen(labels[i]) * 6 * 2;
            display_text(cx - label_w / 2, 132, labels[i], color, 2);
        }
    }

    int desc_w = display_text_width_16(descriptions[*selected]);
    display_text_16((DISP_FB_W - desc_w) / 2, 166,
                    descriptions[*selected], C_UI_FG);
    for (int i = 0; i < BOOT_MODE_COUNT; i++) {
        display_fill_rect(124 + i * 16, 188, 8, 4,
                          i == *selected ? accents[i] : C_UI_LINE);
    }
    int footer_w = display_text_width_16("左右选择  A确认");
    display_text_16((DISP_FB_W - footer_w) / 2, 204,
                    "左右选择  A确认", C_UI_FG_FAINT);
}

/* 独立设置页沿用 retro-go 的 Options 结构：纯色面板、居中标题、反选行；
 * 颜色统一复用 display.h 的语义层。设置仍只对本次开机
 * 有效，避免孩子不小心静音后每次上电都以为机器坏了。 */
#define SETTINGS_COUNT       3
#define SETTINGS_BRIGHTNESS  0
#define SETTINGS_VOLUME      1
#define SETTINGS_TEST        2

static void settings_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    (void)strip;
    (void)y0;
    (void)h;
    int selected = *(const int *)ctx;

    display_clear(C_UI_BG);
    display_rect(0, 0, DISP_FB_W, DISP_FB_H, C_UI_EDGE);

    const int box_x = 27;
    const int box_y = 34;
    const int box_w = 234;
    const int box_h = 130;
    const int row_x = box_x + 9;
    const int row_w = box_w - 18;
    const int row_y = box_y + 39;
    const int row_h = 24;

    display_fill_rect(box_x, box_y, box_w, box_h, C_UI_PANEL);
    display_rect(box_x, box_y, box_w, box_h, C_UI_EDGE);

    const char *title = "Options";
    display_text_16(box_x + (box_w - display_text_width_16(title)) / 2,
                    box_y + 9, title, C_UI_FG);
    display_fill_rect(box_x + 9, box_y + 30, box_w - 18, 1, C_UI_LINE);

    char rows[SETTINGS_COUNT][28];
    snprintf(rows[SETTINGS_BRIGHTNESS], sizeof(rows[0]),
             "Brightness: %3d%%", display_get_backlight());
    snprintf(rows[SETTINGS_VOLUME], sizeof(rows[0]),
             "Volume    : %3d%%", audio_output_get_volume());
    snprintf(rows[SETTINGS_TEST], sizeof(rows[0]), "Controller Test");

    for (int i = 0; i < SETTINGS_COUNT; i++) {
        int y = row_y + i * row_h;
        uint16_t fg = C_UI_FG_DIM;
        if (i == selected) {
            display_fill_rect(row_x, y - 2, row_w, row_h - 2, C_UI_SEL);
            fg = C_UI_FG_INV;
        }
        display_text_16(row_x + 6, y, rows[i], fg);
    }

    int hint1_w = display_text_width_16("上下选择  左右调整");
    display_text_16((DISP_FB_W - hint1_w) / 2, 178,
                    "上下选择  左右调整", C_UI_FG);
    int hint2_w = display_text_width_16("A进入测试  B返回");
    display_text_16((DISP_FB_W - hint2_w) / 2, 202,
                    "A进入测试  B返回", C_UI_FG_FAINT);
}

static void settings_menu(void)
{
    int selected = SETTINGS_VOLUME;
    bool volume_preview_active = false;
    display_stream_sync(settings_strip, &selected);

    uint16_t prev = input_serial_poll() | input_gamepad_poll() | input_usb_poll();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_MENU_POLL_MS));
        uint16_t now = input_serial_poll() | input_gamepad_poll() | input_usb_poll();
        uint16_t edge = now & ~prev;
        prev = now;
        bool dirty = false;

        if (edge & NES_PAD_B) {
            if (volume_preview_active) word_audio_shutdown();
            ui_sound_back();
            return;
        }

        if (edge & (NES_PAD_UP | NES_PAD_DOWN)) {
            int delta = (edge & NES_PAD_UP) ? -1 : 1;
            selected = (selected + delta + SETTINGS_COUNT) % SETTINGS_COUNT;
            dirty = true;
        } else if (edge & (NES_PAD_LEFT | NES_PAD_RIGHT)) {
            int delta = (edge & NES_PAD_LEFT) ? -1 : 1;
            if (selected == SETTINGS_VOLUME) {
                /* 每档 10%，并立即用真实教材语音试听。以前 5% 线性振幅既没有
                 * 试听，档间又只有约 0.8 dB，听起来就像设置没传给 WORDS。 */
                int volume = audio_output_get_volume() + delta * 10;
                if (volume < 0) volume = 0;
                if (volume > 100) volume = 100;
                audio_output_set_volume(volume);
                if (volume_preview_active || (volume > 0 && word_audio_init())) {
                    volume_preview_active = true;
                    word_audio_play("hello");
                }
                dirty = true;
            } else if (selected == SETTINGS_BRIGHTNESS) {
                int backlight = display_get_backlight() + delta * 10;
                if (backlight < 5) backlight = 5;
                if (backlight > 100) backlight = 100;
                display_backlight(backlight);
                dirty = true;
            }
        } else if (selected == SETTINGS_TEST &&
                   (edge & (NES_PAD_A | NES_PAD_START))) {
            ui_sound_enter();
            if (volume_preview_active) {
                word_audio_shutdown();
                volume_preview_active = false;
            }
            input_gamepad_show();
            display_stream_sync(settings_strip, &selected);
            prev = input_serial_poll() | input_gamepad_poll() | input_usb_poll();
        }

        if (dirty) display_stream_sync(settings_strip, &selected);
    }
}

static boot_mode_t boot_menu(void)
{
    int selected = 0;
    display_stream_sync(boot_menu_strip, &selected);
    ESP_LOGI(TAG, "模式选择页已显示");

    uint16_t prev = input_serial_poll() | input_gamepad_poll() | input_usb_poll();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_MENU_POLL_MS));
        uint16_t now = input_serial_poll() | input_gamepad_poll() | input_usb_poll();
        uint16_t edge = now & ~prev;
        prev = now;

        if (edge & (NES_PAD_LEFT | NES_PAD_RIGHT)) {
            int delta = (edge & NES_PAD_LEFT) ? -1 : 1;
            selected = (selected + delta + BOOT_MODE_COUNT) % BOOT_MODE_COUNT;
            display_stream_sync(boot_menu_strip, &selected);
        }
        if (edge & (NES_PAD_A | NES_PAD_START)) {
            ui_sound_enter();
            return (boot_mode_t)selected;
        }
    }
}

void app_main(void)
{
    int64_t startup_started = esp_timer_get_time();
#if OVERCLOCK_LEVEL != 0
    overclock_apply(OVERCLOCK_LEVEL);
#endif

    print_board_info();

    /* 必须先于 display_init()：NES 视频缓冲要 65 KB 连续内部内存，
     * 等两块帧缓冲分配完就凑不出来了。 */
    if (nes_emu_prealloc() != ESP_OK) {
        ESP_LOGE(TAG, "NES 视频缓冲分配失败，内部 RAM 不够");
        return;
    }

    /* 挂卡放在 prealloc 之后：那两块 64 KB 连续内部内存先占住，再让 FATFS
     * 去要它的工作缓冲，免得把内存碎片化连累到 NES 视频缓冲。
     * 挂不上不是致命错误——没卡时下面回退到编译期嵌入的 ROM。 */
#if SD_SELFTEST
    sd_card_selftest();
#else
    sd_card_mount();
#endif

    /* 声音开关只在当前运行中有效；每次启动都先恢复默认开启。 */
    audio_output_settings_init();

    if (display_init() != ESP_OK) {
        ESP_LOGE(TAG, "屏幕初始化失败，检查接线和 display.h 里的引脚定义");
        return;
    }

    /* 保留声音早于 USB/摇杆的内存分配顺序，避免把它的内部 DMA 空间碎片化。
     * 入场音稍后才播放；准备阶段只能看到没有控件的等待提示。 */
    boot_show_preparing("正在准备声音");
    bool sound = audio_output_init(CHIME_RATE) == ESP_OK;

    boot_show_preparing("正在准备手柄");
    input_serial_init();
    input_usb_init();
    input_gamepad_init();
    uint16_t boot_keys = input_serial_poll() | input_gamepad_poll() | input_usb_poll();
    bool refresh_rom_index = (boot_keys & NES_PAD_SELECT) != 0;
    if (refresh_rom_index) {
        ESP_LOGI(TAG, "检测到 SELECT，忽略 ROM 目录缓存并完整重扫");
    }

    /* SELECT 在耗时读卡前采样，玩家看见重扫日志即可松手。目录和收藏都在
     * 首页出现之前准备好，进入 GAME 后的首帧就能显示完整数量和收藏状态。 */
    boot_show_preparing(refresh_rom_index ? "正在刷新游戏列表" : "正在读取游戏列表");
    int rom_count = rom_store_init(refresh_rom_index);
    if (rom_count > 0) {
        boot_show_preparing("正在读取收藏");
        esp_err_t err = rom_favorites_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "收藏暂不可用，保留原文件并允许进入其他功能：%s",
                     esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "启动准备完成：%d 个游戏，耗时 %lld ms，即将显示模式选择页",
             rom_count, (long long)((esp_timer_get_time() - startup_started) / 1000));
#if SHOW_DISPLAY_SELFTEST
    screen_diagnostic();
#endif
    boot_intro(sound);
    /* 慢卡扫描期间串口可能积压了按键，清掉后再显示首页，避免刚出现就被
     * 之前的确认键带走。实体手柄的长按由 boot_menu() 的输入基线抑制。 */
    input_serial_discard();
    /* 开机选单只返回目录项；各模拟器在自己的大块内存准备妥当后再从卡上读，
     * SNES 尤其不能先读出 4 MiB 再复制一份，否则 8 MiB PSRAM 会在峰值时耗尽。
     * 卡不可用时 entry 留 NULL，NES 继续走编译期嵌入 ROM 的回退路径。 */
    const rom_store_entry_t *entry = NULL;
    while (1) {
        boot_mode_t boot_mode = boot_menu();
        if (boot_mode == BOOT_MODE_WORDS) {
            word_study_run();
            continue;
        }
        if (boot_mode == BOOT_MODE_SETTINGS) {
            settings_menu();
            continue;
        }

        rom_menu_result_t menu_result = rom_menu_pick(&entry);
        if (menu_result == ROM_MENU_BACK) continue;
        break;  /* 已选游戏，或目录不可用而回退到内置 NES */
    }

    /* ZIP 为了开机快只登记外层文件名，到玩家真正选择时才读一次内部目录。 */
    rom_store_entry_t resolved;
    const rom_store_entry_t *run_entry = entry;
    if (entry) {
        loading_screen_begin(entry->name);
        rom_store_set_progress_callback(loading_screen_progress);
    }
    if (entry && rom_store_resolve(entry, &resolved) != ESP_OK) {
        ESP_LOGE(TAG, "%s 不是可用的 ROM ZIP，1.5 秒后返回菜单", entry->name);
        loading_screen_error("ZIP 不可用");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    if (entry) run_entry = &resolved;

    rom_system_t system = run_entry ? run_entry->system : ROM_SYSTEM_NES;
    esp_err_t run_err = system == ROM_SYSTEM_NES     ? nes_emu_run(run_entry)
                      : system == ROM_SYSTEM_SNES    ? snes_emu_run(run_entry)
                      : system == ROM_SYSTEM_GENESIS ? genesis_emu_run(run_entry)
                      : system == ROM_SYSTEM_PCE     ? pce_emu_run(run_entry)
                                                     : gbc_emu_run(run_entry);
    if (run_err != ESP_OK) {
        ESP_LOGE(TAG, "模拟器启动失败");
        loading_screen_error("游戏启动失败");
    }
}
