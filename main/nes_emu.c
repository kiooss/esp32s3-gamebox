/*
 * NES 模拟器适配层
 *
 * nofrendo 只要求宿主提供两件事：一块视频缓冲，和一个每帧调用一次的 blit 回调。
 * 这个文件干的就是把那块 8 位调色板索引的缓冲，转成 RGB565 画进 display.c 的帧缓冲。
 *
 * 画面怎么放：
 *   NES 输出 256x240，但上下各 8 行是 overscan —— 真电视上看不到，很多游戏
 *   那里就是垃圾数据，所以裁掉，剩 256x224。
 *   竖向 224 行 1:1 直接用，不再抽行（以前受内部 RAM 限制要每 7 行丢 1 行）。
 *   横向 256 -> 288，每 8 个源像素输出 9 个，修 NES 的 8:7 像素宽高比。
 *   为什么是 9:8、为什么不铺满 320，都记在 display.h 的画布那一节。
 *
 *   居中落点由 display.h 算：左右各 16 列、上下各 8 行黑边。
 *
 * ---- 双缓冲在这一层 ----
 *
 * display.c 没有常驻帧缓冲了，它按条带向上要数据。所以「核 0 算下一帧、
 * 核 1 推上一帧」的并行必须靠这里的两块 vidbuf 来保证：blit 回调把刚画完
 * 的那块交给 display，然后立刻 nes_setvidbuf() 换到另一块，nofrendo 下一帧
 * 就渲染进那块去，两边互不干扰。
 *
 * 8 位的 vidbuf 每块 64 KB，两块 128 KB —— 比两块 RGB565 整帧（252 KB）
 * 便宜一半，这正是能升到 288x224 的原因。
 *
 * 黑边只在开机时清一次，之后每帧只推 288x224 那块区域，不会被动到。
 */

#include <string.h>
#include <stdlib.h>
#include "nes_emu.h"
#include "display.h"
#include "input_serial.h"
#include "input_gamepad.h"
#include "input_usb.h"
#include "audio_output.h"
#include "rgb_led.h"
#include "nofrendo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "nes";

/* ---- 编译期嵌入的回退 ROM ----
 *
 * ⚠ 平时换游戏**不用改这里** —— 开机选单（rom_menu.c）会列出 roms 分区里
 * 全部游戏让你选。这个宏只决定「选单不可用时跑哪个」：roms 分区还没烧过
 * （`idf.py flash-roms`）、或者镜像坏了的时候，nes_emu_run(NULL,...) 会回退到它。
 *
 * 留着这条回退路径的理由：选单是新东西，它坏了不该让整块板子玩不了游戏。
 * 顺便也保住了「只烧固件、不烧 8 MB 的 ROM 分区」这个快速迭代路径。
 *
 * 游戏（都在 main/roms/，加新的要同时在 main/CMakeLists.txt 的 EMBED_FILES 里登记）：
 *
 *   0 = smb       超级马里奥兄弟   mapper 0 (NROM)   PRG 32K + CHR  8K
 *   1 = tetris    俄罗斯方块       mapper 1 (MMC1)   PRG 32K + CHR 16K
 *   2 = contra    魂斗罗           mapper 2 (UxROM)  PRG 128K
 *   3 = pacman    吃豆人           mapper 0 (NROM)   PRG 16K + CHR  8K
 *   4 = drmario   马里奥医生       mapper 1 (MMC1)   PRG 32K + CHR 32K
 *
 * 测试图形（不是游戏，调显示用的）：
 *
 *   5 = SimpleParallaxDemo  视差滚动。要按手柄才动，没接输入时画面静止。
 *   6 = full_palette        铺满全部 64 种 NES 颜色。静态，精确验证调色板转换。
 *   7 = flowing_palette     颜色自己循环流动，不需要输入 —— 验证画面在动用它。
 *
 * 5~7 是随 nofrendo 测试套件分发的公有领域 ROM，可以留在仓库里。
 * 0~4 是版权物，由使用者自备，不要提交进仓库（见 .gitignore）。
 */
#define ROM_CHOICE  2

/* 符号名是 EMBED_FILES 按文件名生成的：非字母数字全换成下划线，
 * 所以 `smb.nes` -> `_binary_smb_nes_start`。 */
#if   ROM_CHOICE == 0
#define ROM_SYM   smb_nes
#define ROM_NAME  "Super Mario Bros."
#elif ROM_CHOICE == 1
#define ROM_SYM   tetris_nes
#define ROM_NAME  "Tetris"
#elif ROM_CHOICE == 2
#define ROM_SYM   contra_nes
#define ROM_NAME  "Contra"
#elif ROM_CHOICE == 3
#define ROM_SYM   pacman_nes
#define ROM_NAME  "Pac-Man"
#elif ROM_CHOICE == 4
#define ROM_SYM   drmario_nes
#define ROM_NAME  "Dr. Mario"
#elif ROM_CHOICE == 5
#define ROM_SYM   SimpleParallaxDemo_nes
#define ROM_NAME  "SimpleParallaxDemo"
#elif ROM_CHOICE == 6
#define ROM_SYM   full_palette_nes
#define ROM_NAME  "full_palette"
#elif ROM_CHOICE == 7
#define ROM_SYM   flowing_palette_nes
#define ROM_NAME  "flowing_palette"
#else
#error "ROM_CHOICE 超出范围（0~7）"
#endif

/* 两层展开：外层先把 ROM_SYM 换成真名，内层再拼进字符串。
 * 少一层的话拼出来的会是字面量 "_binary_ROM_SYM_start"。 */
#define ROM_ASM(sym, suffix)   ROM_ASM_(sym, suffix)
#define ROM_ASM_(sym, suffix)  "_binary_" #sym "_" #suffix

extern const uint8_t rom_start[] asm(ROM_ASM(ROM_SYM, start));
extern const uint8_t rom_end[]   asm(ROM_ASM(ROM_SYM, end));

/* ---- NES 调色板 ----
 *
 * nofrendo 内置 6 套。NES 的颜色是 NTSC 相位信号，没有唯一正确的 RGB 值，
 * 各家解码出来的色相差别不小 —— 尤其是天空色 $22 和马里奥标题字的粉色 $36。
 *
 * 拿《超级马里奥兄弟》标题画面的 4 个关键色（天空 $22 / 标题框 $17 /
 * 标题字 $36 / 草绿 $1A）跟参考画面比对，各套的总色差：
 *
 *   NESCLASSIC  57   <- 最接近，任天堂 NES Classic Edition 用的官方调色板
 *   COMPOSITE   68
 *   SMOOTH      88   <- 之前用的：$22 是唯一 G>R 的，天空偏青蓝而非紫蓝
 *   NTSC        92
 *   PVM         97
 *   NOFRENDO   214   <- 粉色 $36 拟合极准，但其余三色全偏，总分最差
 *
 * 觉得偏色可以换一套，这一行改掉即可，没有性能影响。
 */
#define NES_PALETTE  NES_PALETTE_NESCLASSIC

/* ---- 饱和度 ----
 *
 * 100 = 原样。大于 100 更鲜艳。
 *
 * 为什么需要这个：nofrendo 这 6 套调色板的红色 $16（马里奥的帽子和衣服）
 * 都偏暗 —— R 分量只有常见 FCEUX 调色板 (216,40,0) 的 67%~74%，
 * 所以在小屏上看着发褐。换调色板解决不了，6 套都这样。
 *
 * 这里围绕亮度拉开各通道，灰阶和白色不受影响（R=G=B 时算出来还是自己），
 * 只有带颜色的像素变鲜艳。对 $16 的效果：
 *
 *   100%  (146, 52,  4)  <- 原样，发褐
 *   150%  (182, 41,  0)  <- 当前
 *   180%  (203, 35,  0)  <- 接近 FCEUX 的观感
 *
 * 觉得过了就往回调，觉得还不够红就往上加。只在开机建表时算一次，运行时零开销。
 */
#define NES_SATURATION  150

/* NES 可见区域：裁掉上下各 8 行 overscan */
#define SRC_Y0      8
#define SRC_Y1      232                 /* 不含，共 224 行 */
#define SRC_W       256

/* 横向 9:8 扩展：每 GROUP_SRC 个源像素输出 GROUP_DST 个。256/8*9 = 288。
 * 复制点落在 NES 图块（8 像素宽且对齐）的边界上，视觉上最不突兀。 */
#define GROUP_SRC   8
#define GROUP_DST   9

/* 画布 = NES 可见区竖向 1:1、横向 9:8（display.h 里 DISP_FB_W/H = 288x224）。
 * 居中落在面板上由 display.c 负责，所以这里的绘图坐标从 (0,0) 起。 */
_Static_assert(DISP_FB_W == SRC_W / GROUP_SRC * GROUP_DST,
               "画布宽度要等于 NES 画面宽度做完 9:8 扩展的结果");
_Static_assert(DISP_FB_H == SRC_Y1 - SRC_Y0,
               "画布高度要和裁掉 overscan 后的行数一致");
_Static_assert(SRC_W % GROUP_SRC == 0, "源宽度要能被 8 整除，内循环才好展开");

static uint16_t s_palette[256]; /* 8 位索引 -> RGB565（大端，见 display.h） */
static uint8_t  *s_vidbuf[2];   /* 各 NES_SCREEN_PITCH * NES_SCREEN_HEIGHT */
static int       s_draw_idx;    /* nofrendo 当前正渲染进哪一块 */

static int64_t   s_next_frame;
static int       s_frames;
static int64_t   s_emu_us;      /* 累计模拟耗时，用来看 CPU 余量 */
static int64_t   s_stat_t0;
static int64_t   s_frame_t0;
static int       s_frame_period_us;

/* 开机时先跑一遍分阶段计时，把每帧的时间拆到 CPU / PPU 渲染 / 调色板转换 / 推屏。
 * 调性能或者换硬件之后打开它，能一眼看出瓶颈在哪。 */
#define DIAG_TIMING  0

/* 诊断用：0=正常, 1=blit 什么都不做（只测 6502+PPU） */
static int s_diag_mode;

/* 建调色板：向 nofrendo 要 24 位版本，调完饱和度再转 RGB565。
 *
 * 特意不用它的 16 位版本 —— 那个已经量化到 5/6/5 了，在量化后的值上调饱和度
 * 会放大色阶断层。从 8 位原值算完再量化一次，只损失一道。
 *
 * RGB565() 宏内部含大端字节交换（见 display.h），所以这张表可以直接查了就写帧缓冲。 */
static esp_err_t build_palette(void)
{
    uint8_t *p24 = nofrendo_buildpalette(NES_PALETTE, 24);   /* 256 * 3 字节 */
    if (!p24) return ESP_FAIL;

    for (int i = 0; i < 256; i++) {
        int r = p24[i * 3], g = p24[i * 3 + 1], b = p24[i * 3 + 2];

#if NES_SATURATION != 100
        /* 绕着亮度拉开各通道。R=G=B 时 lum 等于它们自己，所以灰阶和白色不动。
         * 77/150/29 是 0.299/0.587/0.114 的 8 位定点近似。 */
        int lum = (r * 77 + g * 150 + b * 29) >> 8;
        r = lum + (r - lum) * NES_SATURATION / 100;
        g = lum + (g - lum) * NES_SATURATION / 100;
        b = lum + (b - lum) * NES_SATURATION / 100;
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;
#endif
        s_palette[i] = RGB565(r, g, b);
    }

    free(p24);
    return ESP_OK;
}

/* 条带回调 —— 跑在**核 1** 上。把 vidbuf 的第 y0..y0+h-1 行（画布坐标）
 * 查调色板转成 RGB565，同时做横向 9:8 扩展，写进这块条带缓冲。
 *
 * 内循环按 8 个源像素一组展开：查 8 次表、写 9 个像素（最后一个重复），
 * 没有分支也没有取模。 */
static void nes_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const uint8_t  *vidbuf = ctx;
    const uint16_t *pal    = s_palette;

    for (int r = 0; r < h; r++) {
        const uint8_t *src = vidbuf + (size_t)(SRC_Y0 + y0 + r) * NES_SCREEN_PITCH
                                    + NES_SCREEN_OVERDRAW;
        uint16_t      *dst = strip + (size_t)r * DISP_FB_W;

        for (int g = SRC_W / GROUP_SRC; g > 0; g--) {
            uint16_t c0 = pal[src[0]], c1 = pal[src[1]];
            uint16_t c2 = pal[src[2]], c3 = pal[src[3]];
            uint16_t c4 = pal[src[4]], c5 = pal[src[5]];
            uint16_t c6 = pal[src[6]], c7 = pal[src[7]];

            dst[0] = c0; dst[1] = c1; dst[2] = c2; dst[3] = c3;
            dst[4] = c4; dst[5] = c5; dst[6] = c6; dst[7] = c7;
            dst[8] = c7;                    /* 复制点落在图块边界上 */

            src += GROUP_SRC;
            dst += GROUP_DST;
        }
    }
}

/* 把画布整片刷黑（进游戏前擦掉菜单）。绘图原语只在条带回调里有效，
 * 所以这种「一次性全屏操作」也得包成回调。 */
static void black_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    display_clear(C_BLACK);
}

/* nofrendo 每模拟完一帧调一次 */
static void blit_frame(uint8_t *vidbuf)
{
    if (s_diag_mode == 1) return;

    /* 只统计核 0 这一侧（6502 + PPU）的耗时。转换和推屏都在核 1，
     * 它们的时间由 display.c 的 DISP_PROFILE 单独报。 */
    s_emu_us += esp_timer_get_time() - s_frame_t0;

    /* 把刚渲染完的这块交给核 1 流式推屏，然后立刻换到另一块。
     *
     * nes_setvidbuf() 只是个指针交换（nes.c），而 nofrendo 在调完 blit 之后
     * 本帧就不再碰 vidbuf 了（nes.c 里 blit_func 之后只剩 apu_emulate），
     * 所以在这里换是安全的 —— 下一帧的 ppu_renderline 会写进新的那块。 */
    display_stream(nes_strip, vidbuf);

    s_draw_idx ^= 1;
    nes_setvidbuf(s_vidbuf[s_draw_idx]);

    /* ---- 帧率对齐到 ROM 制式 ----
     * display_stream() 只保证不超过屏幕能吃下的速度，不等于 NES 的 50/60fps，
     * 所以这里自己配速。 */
    s_next_frame += s_frame_period_us;
    int64_t now = esp_timer_get_time();
    if (s_next_frame > now) {
        int64_t wait_us = s_next_frame - now;
        if (wait_us > 1500) {
            vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));   /* tick = 1ms */
        }
        while (esp_timer_get_time() < s_next_frame) { }  /* 补齐零头 */
    } else {
        s_next_frame = now;     /* 已经落后了，别越积越多 */
        /* 就算跑不满 60fps 也必须让出 CPU 一次，
         * 否则核 0 的空闲任务永远得不到调度，5 秒后触发任务看门狗。 */
        vTaskDelay(1);
    }

    /* ---- 每秒报一次 ---- */
    s_frames++;
    if (now - s_stat_t0 >= 1000000) {
        int   fps  = (int)(s_frames * 1000000LL / (now - s_stat_t0));
        float per  = (float)s_emu_us / 1000.0f / s_frames;
        printf("NES %d fps  (模拟+转换 %.1f ms/帧，CPU 余量 %d%%)\n",
               fps, per, 100 - (int)(s_emu_us * 100 / (now - s_stat_t0)));
        s_frames  = 0;
        s_emu_us  = 0;
        s_stat_t0 = now;
    }

    s_frame_t0 = esp_timer_get_time();
}

esp_err_t nes_emu_prealloc(void)
{
    /* 两块：核 0 渲染一块的同时核 1 在读另一块。都必须在内部 RAM ——
     * vidbuf 是 PPU 逐扫描线读写的最热内存，放 PSRAM 会让渲染从 2ms 涨到 8.5ms。 */
    for (int i = 0; i < 2; i++) {
        s_vidbuf[i] = heap_caps_calloc(1, NES_SCREEN_PITCH * NES_SCREEN_HEIGHT,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_vidbuf[i]) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* 选中的不是 NES 时把这 128 KB 内部 SRAM 还回去。
 *
 * prealloc 是在开机选单**之前**跑的（顺序被 display_init 逼死了），
 * 那时还不知道用户会选哪个平台。NES 之外的核自己也要大块内部内存
 * （SNES 的 RGB565 帧缓冲就要 119 KB），不还就只能退 PSRAM。 */
void nes_emu_release_prealloc(void)
{
    for (int i = 0; i < 2; i++) {
        heap_caps_free(s_vidbuf[i]);
        s_vidbuf[i] = NULL;
    }
}

esp_err_t nes_emu_run(const uint8_t *rom, size_t rom_size, const char *name)
{
    /* 没给 ROM 就用编译期嵌进来的那个 —— roms 分区还没烧过时的回退路径。 */
    if (!rom) {
        rom      = rom_start;
        rom_size = rom_end - rom_start;
        name     = ROM_NAME;
    }
    printf("\nROM: %s  (%u 字节)\n", name, (unsigned)rom_size);

    /* 把菜单擦掉。只要一次 —— 没有常驻缓冲了，之后每帧都是现算的。 */
    display_stream_sync(black_strip, NULL);

    if (nofrendo_init(SYS_DETECT, NES_AUDIO_SAMPLE_RATE, false, blit_frame,
                      NULL, NULL) != 0) {
        ESP_LOGE(TAG, "nofrendo 初始化失败");
        return ESP_FAIL;
    }

    esp_err_t audio_err = audio_output_init(NES_AUDIO_SAMPLE_RATE);
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "MAX98357 音频未启动：%s，继续静音运行",
                 esp_err_to_name(audio_err));
    }

    if (build_palette() != ESP_OK) {
        ESP_LOGE(TAG, "调色板构建失败");
        return ESP_FAIL;
    }

    /* PPU 逐扫描线写这两块（各 64 KB），是整个模拟里最热的内存。
     * 放 PSRAM 时实测 PPU 渲染要 8.5 ms/帧，放内部 SRAM 快得多。
     * 去掉两块 96 KB 的 RGB565 帧缓冲之后，内部 RAM 够放两块了。 */
    if (!s_vidbuf[0] || !s_vidbuf[1]) {
        ESP_LOGE(TAG, "视频缓冲分配失败（需要 2x%d 字节内部 RAM）",
                 NES_SCREEN_PITCH * NES_SCREEN_HEIGHT);
        return ESP_ERR_NO_MEM;
    }
    /* ROM 数据留在 flash 里（走 cache 映射），rom_loadmem 只存指针不拷贝 */
    rom_t *cart = rom_loadmem((uint8_t *)rom, rom_size);
    if (!cart) {
        ESP_LOGE(TAG, "ROM 解析失败（不是合法的 iNES 文件？）");
        return ESP_FAIL;
    }
    if (nes_insertcart(cart) != 0) {
        ESP_LOGE(TAG, "装卡失败（mapper %d 不支持？）", cart->mapper_number);
        return ESP_FAIL;
    }

    /* PAL 每帧 480 个音频样本、NTSC 每帧 400 个。模拟帧率必须跟 ROM 制式一致，
     * 否则 PAL 游戏仍以 60fps 生产音频，会超过 24kHz I2S 的消费速度并持续丢帧。 */
    int refresh_rate = nes_getptr()->refresh_rate;
    s_frame_period_us = 1000000 / refresh_rate;

    /* ⚠ 必须在 nes_insertcart 之后 ——
     * insertcart 内部会调 nes_reset()，而 nes_reset() 里有一句 nes.vidbuf = NULL。
     * 先 setvidbuf 再 insertcart 的话缓冲会被清掉，nes_emulate 开头的
     * `draw = draw && nes.vidbuf != NULL` 就恒为 false：画面永远不渲染，
     * blit 回调永远不触发，而且因为不再配速会把看门狗饿死。 */
    s_draw_idx = 0;
    nes_setvidbuf(s_vidbuf[s_draw_idx]);

    /* 到这里 ROM、mapper 和视频缓冲都已就绪，游戏确定能启动后再亮灯。
     * RGB 失败不影响模拟器主路径，只记日志继续运行。 */
    esp_err_t rgb_err = rgb_led_start_rainbow();
    if (rgb_err != ESP_OK) {
        ESP_LOGW(TAG, "板载 RGB 彩虹效果未启动：%s",
                 esp_err_to_name(rgb_err));
    }

    printf("内部 RAM 剩余 %u KB\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    printf("开始模拟，目标 %d fps。\n\n", refresh_rate);

    /* nes_insertcart 里已经做过 hard reset，这里不用再来一次
     * —— 再调一次又会把 vidbuf 清成 NULL。 */

#if DIAG_TIMING
    /* ---- 分段计时诊断：一刀一刀切，定位时间到底花在哪 ---- */
    /* 注意：调色板转换和推屏现在都在核 1 上，这个循环只量得到核 0 那一侧。
     * 核 1 的耗时看 display.c 的 DISP_PROFILE 每秒一行的输出。 */
    static const struct { bool draw; int mode; const char *name; } stages[] = {
        { false, 1, "A 只跑 CPU（不渲染不 blit）" },
        { true,  1, "B + PPU 渲染到 vidbuf" },
        { true,  0, "C + 提交给核 1（完整）" },
    };
    for (unsigned st = 0; st < sizeof(stages) / sizeof(stages[0]); st++) {
        s_diag_mode = stages[st].mode;
        int64_t best = 0;
        for (int i = 0; i < 3; i++) {
            int64_t t = esp_timer_get_time();
            nes_emulate(stages[st].draw);
            int64_t d = esp_timer_get_time() - t;
            if (i == 0 || d < best) best = d;   /* 取最快的一次，避开首帧冷 cache */
            vTaskDelay(1);
        }
        printf("%-34s %8lld us\n", stages[st].name, best);
    }
    s_diag_mode = 0;
    printf("\n");
#endif

    /* 飞线手柄、USB HID、串口键盘三路并存。串口留作硬件不灵时的后路。 */
    input_serial_init();
    input_usb_init();
    input_gamepad_init();

    s_next_frame = s_stat_t0 = s_frame_t0 = esp_timer_get_time();
    while (1) {
        /* 端口 0 的手柄在 input_init() 里已经接好了，这里只管更新状态。
         * 按位或：两路谁按下都算数。 */
        input_update(0, (uint8_t)(input_serial_poll() | input_gamepad_poll() |
                                 input_usb_poll()));
        nes_emulate(true);
    }

    return ESP_OK;
}
