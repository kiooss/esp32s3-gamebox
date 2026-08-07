/*
 * NES 模拟器适配层
 *
 * nofrendo 只要求宿主提供两件事：一块视频缓冲，和一个每帧调用一次的 blit 回调。
 * 这个文件干的就是把那块 8 位调色板索引的缓冲，转成 RGB565 画进 display.c 的帧缓冲。
 *
 * 画面怎么放：
 *   NES 输出 256x240，但上下各 8 行是 overscan —— 真电视上看不到，很多游戏
 *   那里就是垃圾数据，所以裁掉，剩 256x224。
 *   屏幕横屏是 320x240，论面积 224 行放得下，但两块帧缓冲塞不进内部 RAM
 *   （详细的账在 display.h 里），所以竖向抽行：每 7 行丢 1 行，224 -> 192。
 *   换屏前那块 320x170 的屏是每 4 行丢 1 行（224 -> 168），现在损失小了不少，
 *   画面也高了 14%。整数运算、不插值，几乎不花 CPU。
 *   横向 256 一律不缩，像素 1:1 最锐利。
 *
 *   居中落点由 display.h 算：左右各 32 列、上下各 24 行黑边。
 *
 * 黑边只在开机时清一次。之后每帧只画 256x192 那块区域，
 * 边框区域的内容在两块缓冲里都是黑的，不会被动到。
 */

#include <string.h>
#include <stdlib.h>
#include "nes_emu.h"
#include "display.h"
#include "input_serial.h"
#include "nofrendo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "nes";

/* ---- 选择跑哪个 ROM ----
 *
 *   0 = smb                 超级马里奥兄弟。mapper 0 (NROM)，PRG 32K + CHR 8K。
 *   1 = SimpleParallaxDemo  视差滚动。要按手柄才动，没接输入时画面静止。
 *   2 = full_palette        铺满全部 64 种 NES 颜色。静态，精确验证调色板转换。
 *   3 = flowing_palette     颜色自己循环流动，不需要输入 —— 验证画面在动用它。
 *
 * 1~3 是随 nofrendo 测试套件分发的公有领域 ROM，可以留在仓库里。
 * 0 是版权物，由使用者自备，不要提交进仓库。
 */
#define ROM_CHOICE  0

#if ROM_CHOICE == 0
extern const uint8_t rom_start[] asm("_binary_smb_nes_start");
extern const uint8_t rom_end[]   asm("_binary_smb_nes_end");
#define ROM_NAME "Super Mario Bros."
#elif ROM_CHOICE == 1
extern const uint8_t rom_start[] asm("_binary_SimpleParallaxDemo_nes_start");
extern const uint8_t rom_end[]   asm("_binary_SimpleParallaxDemo_nes_end");
#define ROM_NAME "SimpleParallaxDemo"
#elif ROM_CHOICE == 2
extern const uint8_t rom_start[] asm("_binary_full_palette_nes_start");
extern const uint8_t rom_end[]   asm("_binary_full_palette_nes_end");
#define ROM_NAME "full_palette"
#else
extern const uint8_t rom_start[] asm("_binary_flowing_palette_nes_start");
extern const uint8_t rom_end[]   asm("_binary_flowing_palette_nes_end");
#define ROM_NAME "flowing_palette"
#endif

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

/* 音频还没接喇叭，但采样率不能填 0 —— apu_build_luts(0) 会除零。
 * 给个正常值让 APU 正常跑，输出直接丢掉，将来接 I2S 时这里就是现成的。 */
#define AUDIO_SAMPLE_RATE  16000

/* NES 可见区域：裁掉上下各 8 行 overscan */
#define SRC_Y0      8
#define SRC_Y1      232                 /* 不含，共 224 行 */
#define SRC_W       256

/* 竖向抽行：每 KEEP_EVERY 行丢最后 1 行。224 * 6/7 = 192。 */
#define KEEP_EVERY  7

/* 画布就是 NES 画面区本身（display.h 里 DISP_FB_W/H = 256x192），
 * 居中落在面板上由 display.c 负责，所以这里的绘图坐标从 (0,0) 起。 */
_Static_assert(DISP_FB_W == SRC_W, "画布宽度要和 NES 画面宽度一致");
_Static_assert(DISP_FB_H == (SRC_Y1 - SRC_Y0) * (KEEP_EVERY - 1) / KEEP_EVERY,
               "画布高度要和抽行后的行数一致");

#define FRAME_PERIOD_US  (1000000 / 60)

static uint16_t s_palette[256]; /* 8 位索引 -> RGB565（大端，见 display.h） */
static uint8_t  *s_vidbuf;      /* NES_SCREEN_PITCH * NES_SCREEN_HEIGHT */

static int64_t   s_next_frame;
static int       s_frames;
static int64_t   s_emu_us;      /* 累计模拟耗时，用来看 CPU 余量 */
static int64_t   s_stat_t0;
static int64_t   s_frame_t0;

/* 开机时先跑一遍分阶段计时，把每帧的时间拆到 CPU / PPU 渲染 / 调色板转换 / 推屏。
 * 调性能或者换硬件之后打开它，能一眼看出瓶颈在哪。 */
#define DIAG_TIMING  0

/* 诊断用：0=正常, 1=blit 什么都不做, 2=只做调色板转换不推屏 */
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

/* nofrendo 每模拟完一帧调一次 */
static void blit_frame(uint8_t *vidbuf)
{
    if (s_diag_mode == 1) return;

    uint16_t       *fb  = display_fb();
    const uint16_t *pal = s_palette;

    int oy = 0, run = 0;
    for (int sy = SRC_Y0; sy < SRC_Y1; sy++) {
        /* 每 7 行丢 1 行 —— 224 行变 192 行。用计数器而不是取模，省掉除法。 */
        if (++run == KEEP_EVERY) { run = 0; continue; }

        const uint8_t *src = vidbuf + (size_t)sy * NES_SCREEN_PITCH
                                    + NES_SCREEN_OVERDRAW;
        uint16_t      *dst = fb + (size_t)oy * DISP_FB_W;

        for (int x = 0; x < SRC_W; x++) {
            dst[x] = pal[src[x]];
        }
        oy++;
    }

    if (s_diag_mode == 2) return;

    s_emu_us += esp_timer_get_time() - s_frame_t0;
    display_flush();

    /* ---- 帧率对齐到 60fps ----
     * display_flush() 只保证不超过屏幕能吃下的速度（满屏约 86fps），
     * 不等于 NES 的 60fps，所以这里自己配速。 */
    s_next_frame += FRAME_PERIOD_US;
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
    s_vidbuf = heap_caps_calloc(1, NES_SCREEN_PITCH * NES_SCREEN_HEIGHT,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return s_vidbuf ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t nes_emu_run(void)
{
    size_t rom_size = rom_end - rom_start;
    printf("\nROM: %s  (%u 字节)\n", ROM_NAME, (unsigned)rom_size);

    /* 黑边只清这一次。两块缓冲都要清，之后每帧只动中间 256x168。 */
    for (int i = 0; i < 2; i++) {
        display_clear(C_BLACK);
        display_flush();
    }
    display_wait_idle();

    if (nofrendo_init(SYS_DETECT, AUDIO_SAMPLE_RATE, false, blit_frame,
                      NULL, NULL) != 0) {
        ESP_LOGE(TAG, "nofrendo 初始化失败");
        return ESP_FAIL;
    }

    if (build_palette() != ESP_OK) {
        ESP_LOGE(TAG, "调色板构建失败");
        return ESP_FAIL;
    }

    /* PPU 逐扫描线写这块（65 KB），是整个模拟里最热的内存。
     * 放 PSRAM 时实测 PPU 渲染要 8.5 ms/帧，放内部 SRAM 快得多。
     * 画布缩到 256x168 之后省下的内存正好够它落回内部。 */
    if (!s_vidbuf) {
        ESP_LOGE(TAG, "视频缓冲分配失败（需要 %d 字节内部 RAM）",
                 NES_SCREEN_PITCH * NES_SCREEN_HEIGHT);
        return ESP_ERR_NO_MEM;
    }
    /* ROM 数据留在 flash 里（走 cache 映射），rom_loadmem 只存指针不拷贝 */
    rom_t *cart = rom_loadmem((uint8_t *)rom_start, rom_size);
    if (!cart) {
        ESP_LOGE(TAG, "ROM 解析失败（不是合法的 iNES 文件？）");
        return ESP_FAIL;
    }
    if (nes_insertcart(cart) != 0) {
        ESP_LOGE(TAG, "装卡失败（mapper %d 不支持？）", cart->mapper_number);
        return ESP_FAIL;
    }

    /* ⚠ 必须在 nes_insertcart 之后 ——
     * insertcart 内部会调 nes_reset()，而 nes_reset() 里有一句 nes.vidbuf = NULL。
     * 先 setvidbuf 再 insertcart 的话缓冲会被清掉，nes_emulate 开头的
     * `draw = draw && nes.vidbuf != NULL` 就恒为 false：画面永远不渲染，
     * blit 回调永远不触发，而且因为不再配速会把看门狗饿死。 */
    nes_setvidbuf(s_vidbuf);

    printf("内部 RAM 剩余 %u KB\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    printf("开始模拟，目标 60 fps。\n\n");

    /* nes_insertcart 里已经做过 hard reset，这里不用再来一次
     * —— 再调一次又会把 vidbuf 清成 NULL。 */

#if DIAG_TIMING
    /* ---- 分段计时诊断：一刀一刀切，定位时间到底花在哪 ---- */
    static const struct { bool draw; int mode; const char *name; } stages[] = {
        { false, 1, "A 只跑 CPU（不渲染不 blit）" },
        { true,  1, "B + PPU 渲染到 vidbuf" },
        { true,  2, "C + 调色板转换到帧缓冲" },
        { true,  0, "D + 推屏（完整）" },
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

    input_serial_init();

    s_next_frame = s_stat_t0 = s_frame_t0 = esp_timer_get_time();
    while (1) {
        /* 端口 0 的手柄在 input_init() 里已经接好了，这里只管更新状态 */
        input_update(0, input_serial_poll());
        nes_emulate(true);
    }

    return ESP_OK;
}
