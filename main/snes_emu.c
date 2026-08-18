/*
 * SNES（Snes9x 2005）适配层 —— 可行性验证版
 *
 * SNES 可见区是 256x224，和 NES 裁掉 overscan 之后**一模一样**。但和 NES
 * 不同，SNES 不挂公共 288x224 画布——横向 256->320 正好是整数 5:4 扩展，
 * 纵向 224->240（15:14，非整数比，最近邻取源行）走 display_stream_sized()
 * 铺满整块 320x240 面板，和 GB/GBC、Genesis 同一套做法（见 snes_strip）。
 *
 * 和 NES/GB 那两层的差别（都是刻意的，为了先拿到帧率数据）：
 *
 *  1. **渲染缓冲在内部 SRAM，推屏走 PSRAM 影子缓冲**。不是标准的乒乓 ——
 *     一块 119 KB，内部 SRAM 腾不出第二块。理由见 s_framebuf/s_present 处。
 *
 *  2. **仅 SMW 有即时存档，L/R 暂无实体键**。Shield 四个大键已按
 *     物理方位映射完整 SNES ABXY，F/E 小键对应 SELECT/START。SMW 用
 *     SELECT+A（A 键实体丝印是 B）长按 1 秒保存，实现在宿主 snes_save.c，
 *     不改 Snes9x 核心。SELECT+START 长按 1 秒是全局退出键，触发
 *     esp_restart() 软重启回到 ROM 菜单——没有任何模拟器落盘卡带电池
 *     SRAM，重启不丢真实存档。
 *
 *  3. **跳帧默认为 3**（画 1 帧跳 3 帧）。这不是保守估计，是上游 retro-go
 *     给 SNES 写死的初值（main_snes.c: `app->frameskip = 3;`），
 *     而它的 README 直接把 SNES 标成 "(slow)"。先按它的设定跑，再看余量。
 *
 * ⚠ 授权：snes9x 不是 GPL，src/LICENSE 禁止商业分发。整机固件本来就因为
 *   nofrendo 受 GPL v2 约束，加上这一条之后分发限制更严。
 */

#include <string.h>
#include "snes_emu.h"
#include "audio_output.h"
#include "display.h"
#include "input_gamepad.h"
#include "input_serial.h"
#include "input_usb.h"
#include "nes_emu.h"
#include "rgb_led.h"
#include "snes_save.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <snes9x.h>

static const char *TAG = "snes";

/* 横向 256->320 是整数 5:4 扩展（4 个源像素一组变 5 个）。不用公共
 * 288x224 画布——和 GB/GBC、Genesis 一样走 display_stream_sized() 铺满
 * 整块面板；纵向 224->240 非整数比，运行时在 snes_strip 里最近邻取源行。 */
#define GROUP_SRC   4
#define GROUP_DST   5

_Static_assert(DISP_W == SNES_WIDTH / GROUP_SRC * GROUP_DST,
               "面板宽度要等于 SNES 画面宽度做完 5:4 扩展的结果");

/* snes9x 的帧缓冲按 SNES_HEIGHT_EXTENDED(239) 行分配：少数游戏会切到
 * 239 行模式。画布只有 224 行，多出来的 15 行直接不画（先不处理，
 * SMW 全程是 224 行模式）。 */
#define SNES_FB_BYTES   (SNES_WIDTH * 2 * SNES_HEIGHT_EXTENDED)

/* 缓冲按 50 fps（PAL）的最大需求分配，每帧实际提交多少按卡带真实帧率算 ——
 * 见 s_audio_frames。之前固定按 480 提交是个 bug：NTSC 60 fps 只需要 400，
 * 多产的 20% 既白烧 CPU（snes9x 的 dsp.c 混音不便宜），又和 I2S 的消费速率对不上。 */
#define SNES_AUDIO_MAX_FRAMES  AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET

/* memmap.c 分配 ROM 时会多要 64 KB「for mapping purposes」——
 * 映射表以 32/64 KB 为粒度指进 ROM，末页可能越过实际文件尾。照抄这个余量。 */
#define SNES_ROM_SLACK  (0x10000 + 0x200)

/* 出厂跳帧数。0 = 每帧都画。改这个值重编就能对比帧率。 */
#define SNES_FRAMESKIP  3

/* 1 = 模拟照跑但完全不推屏，用来单独量 CPU+PPU 的耗时（对照 nes_emu.c 的 DIAG_TIMING）。 */
#define SNES_DIAG_NO_BLIT  0

/* SMW 的即时存档组合键。两键从按下那一帧起就不传给游戏，持续 1 秒才写盘，
 * 避免正常按 START 暂停时误存；松开以后才能再触发下一次。 */
#define SAVE_COMBO_BITS    (GAMEPAD_BIT_SELECT | GAMEPAD_BIT_A)
#define SAVE_HOLD_US       1000000

/* 退出到 ROM 菜单：SELECT+START 长按 1 秒触发 esp_restart()。全局键，不像
 * 存档那样区分卡带——没有任何模拟器实现卡带电池 SRAM 落盘（SNES 唯一的
 * 存档功能是上面这条独立的全量状态存档，重启不影响它），所以直接软重启
 * 回 app_main -> rom_menu_pick 不会丢数据。 */
#define EXIT_COMBO_BITS    (GAMEPAD_BIT_SELECT | GAMEPAD_BIT_START)
#define EXIT_HOLD_US       1000000

/* 内部 SRAM 只腾得出约 179 KB（NES 那 128 KB 还回来之后），而想放进去的有：
 * 帧缓冲 119 KB、WRAM 128 KB、VRAM 64 KB —— 任意两个都放不下。
 * 这个开关用来实测哪一个进内部 SRAM 收益最大：
 *   0 = 帧缓冲进内部（WRAM/VRAM 留 PSRAM）
 *   1 = WRAM 进内部（帧缓冲退 PSRAM）
 * 结论见 README 的排障记录。 */
#define SNES_MEM_PROFILE   0

/* 渲染目标（内部 SRAM）和推屏源（PSRAM）分开两块。
 *
 * 为什么不是两块内部 SRAM 的乒乓：一块 119 KB，内部总共只腾得出 ~179 KB
 * （已经把 NES 那 128 KB 还回来之后），装不下第二块。
 *
 * 为什么也不是「单块 + 异步推屏」：display_stream 只在**提交下一帧**时阻塞，
 * 不保护缓冲本身。核 1 还在推最后一条带时核 0 已经开始渲染下一帧，最后一条
 * 会撕。
 *
 * 所以折中成：渲染始终写内部那块（最热，PPU 逐扫描线读写），画完 memcpy
 * 到 PSRAM 影子缓冲再异步推。memcpy 约 2~3 ms，换掉同步推屏的 14 ms 阻塞。
 * 反过来（渲染进 PSRAM）实测每渲染帧要多花 ~23 ms，差得远。 */
static uint16_t *s_framebuf;    /* GFX.Screen，内部 SRAM，SNES_WIDTH x 239 */
static uint16_t *s_present;     /* 推屏源，PSRAM */
static int16_t  *s_soundbuf;
static bool      s_audio_ok;
static int       s_audio_frames; /* 每帧提交多少立体声帧，由卡带帧率决定 */
static uint16_t  s_pad_state;    /* 每帧只轮询一次，核心回调读取这份快照 */

/* 条带回调 —— 跑在核 1 上。做三件事：纵向最近邻选源行、横向 4:5 扩展、
 * 小端转大端。
 *
 * snes9x 直接产出主机字节序（小端）的 RGB565，而本项目的帧缓冲存的是
 * 字节交换后的大端值（见 display.h 的 RGB565 宏）。所以这里每个像素都要
 * __builtin_bswap16 —— 在已经要逐像素搬运的循环里，这一条指令基本白送。
 *
 * 纵向 224->240 不是整数比，每行按 retro-go 同样的 step 公式取源行
 * （和 gbc_strip 同一套算法，没有插值）。横向内循环按 4 个源像素一组
 * 展开，写 5 个（最后一个重复），和之前 8:9 版本同构，只是组更小。 */
static void snes_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const uint16_t *frame = ctx;

    for (int r = 0; r < h; r++) {
        int src_y = (y0 + r) * SNES_HEIGHT / DISP_H;
        const uint16_t *src = frame + (size_t)src_y * SNES_WIDTH;
        uint16_t       *dst = strip + (size_t)r * DISP_W;

        for (int g = SNES_WIDTH / GROUP_SRC; g > 0; g--) {
            uint16_t c0 = __builtin_bswap16(src[0]);
            uint16_t c1 = __builtin_bswap16(src[1]);
            uint16_t c2 = __builtin_bswap16(src[2]);
            uint16_t c3 = __builtin_bswap16(src[3]);

            dst[0] = c0; dst[1] = c1; dst[2] = c2; dst[3] = c3;
            dst[4] = c3;                /* 复制点落在 4 像素图块边界上 */

            src += GROUP_SRC;
            dst += GROUP_DST;
        }
    }
}

typedef struct {
    const char *text;
    uint16_t color;
} save_notice_t;

/* 保存时冻结当前画面并盖一条提示。display 的绘图原语会自己裁到当前条带，
 * 所以和其他条带回调一样，每条都执行同一份整屏坐标绘制列表。 */
static void save_notice_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const save_notice_t *notice = ctx;
    snes_strip(strip, y0, h, s_present);

    int text_w = (int)strlen(notice->text) * 6;
    int x = (DISP_W - text_w) / 2;
    display_fill_rect(x - 8, 108, text_w + 16, 24, C_BLACK);
    display_text(x, 116, notice->text, notice->color, 1);
}

static void show_save_notice(const char *text, uint16_t color)
{
    save_notice_t notice = { .text = text, .color = color };
    /* snes_strip 现在按 DISP_W 铺画布——display_stream_sync 走的默认
     * DISP_FB_W(288) 会和条带 stride 对不上，改用 sized + 手动等待。 */
    display_stream_sized(save_notice_strip, &notice, DISP_W, DISP_H);
    display_wait_idle();
}

static void black_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    display_clear(C_BLACK);
}

/* ---- snes9x 要求宿主实现的回调（src/display.h 里那一组） ---- */

bool S9xInitDisplay(void)
{
    GFX.Pitch  = SNES_WIDTH * 2;
    GFX.ZPitch = SNES_WIDTH;
    GFX.Screen = (uint8_t *)s_framebuf;   /* snes9x 里 Screen 是字节指针 */

    /* 这三块是渲染中间结果，只被 gfx.c/tile.c 读写，不参与 DMA，放 PSRAM。
     * 合计约 245 KB —— 内部 SRAM 放不下（NES 那两块 64 KB vidbuf 已经占了）。 */
    GFX.SubScreen  = heap_caps_malloc(GFX.Pitch * SNES_HEIGHT_EXTENDED,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    GFX.ZBuffer    = heap_caps_malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    GFX.SubZBuffer = heap_caps_malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    return GFX.Screen && GFX.SubScreen && GFX.ZBuffer && GFX.SubZBuffer;
}

void S9xDeinitDisplay(void)
{
}

/* Shield 的四个大键按方位完整映射 SNES ABXY；L/R 仍无实体键。 */
static uint32_t map_pad(uint16_t state)
{
    uint32_t pad = 0;
    if (state & GAMEPAD_BIT_RIGHT)  pad |= SNES_RIGHT_MASK;
    if (state & GAMEPAD_BIT_LEFT)   pad |= SNES_LEFT_MASK;
    if (state & GAMEPAD_BIT_UP)     pad |= SNES_UP_MASK;
    if (state & GAMEPAD_BIT_DOWN)   pad |= SNES_DOWN_MASK;
    if (state & GAMEPAD_BIT_A)      pad |= SNES_A_MASK;
    if (state & GAMEPAD_BIT_B)      pad |= SNES_B_MASK;
    if (state & GAMEPAD_BIT_X)      pad |= SNES_X_MASK;
    if (state & GAMEPAD_BIT_Y)      pad |= SNES_Y_MASK;
    if (state & GAMEPAD_BIT_SELECT) pad |= SNES_SELECT_MASK;
    if (state & GAMEPAD_BIT_START)  pad |= SNES_START_MASK;
    return pad;
}

uint32_t S9xReadJoypad(int32_t port)
{
    if (port != 0) return 0;
    return map_pad(s_pad_state);
}

bool S9xReadMousePosition(int32_t which1, int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

void S9xToggleSoundChannel(int32_t channel)
{
}

/* S9xNextController() 故意不在这里实现 —— ppu.c:1824 已经有一份，
 * 重复定义会在链接期打架。display.h 把它列进「宿主要实现」是上游的笔误。 */

bool JustifierOffscreen(void)
{
    return true;
}

void JustifierButtons(uint32_t *justifiers)
{
    (void)justifiers;
}

/* ---- 启动 ---- */

static esp_err_t alloc_buffers(void)
{
    /* 帧缓冲优先要内部 SRAM：这是 PPU 逐扫描线写、核 1 逐像素读的最热内存，
     * NES 那边把 vidbuf 放 PSRAM 会让渲染从 2ms 涨到 8.5ms（见 AGENTS.md）。
     * 但 SNES 是 RGB565 不是 8 位调色板，一块就要 122 KB，多半抢不到 ——
     * 抢不到就退 PSRAM，并把实际落点打出来，因为它直接决定帧率数据怎么读。 */
    s_framebuf = SNES_MEM_PROFILE == 0
               ? heap_caps_calloc(1, SNES_FB_BYTES,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
               : NULL;   /* profile 1 把内部 SRAM 留给 WRAM */
    if (s_framebuf) {
        ESP_LOGI(TAG, "帧缓冲 %d KB 拿到了内部 SRAM", SNES_FB_BYTES / 1024);
    } else {
        s_framebuf = heap_caps_calloc(1, SNES_FB_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGW(TAG, "帧缓冲 %d KB 只能放 PSRAM —— 渲染会明显变慢",
                 SNES_FB_BYTES / 1024);
    }
    if (!s_framebuf) return ESP_ERR_NO_MEM;

    /* 推屏源只被核 1 顺序读一遍，PSRAM 的延迟藏在 SPI DMA 后面，不心疼。 */
    s_present = heap_caps_calloc(1, SNES_FB_BYTES,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_present) return ESP_ERR_NO_MEM;

    s_soundbuf = heap_caps_calloc(SNES_AUDIO_MAX_FRAMES * 2, sizeof(int16_t),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return s_soundbuf ? ESP_OK : ESP_ERR_NO_MEM;
}

/* S9xInitMemory() 会自己 malloc 一块 ROM 缓冲，且贪心地从 6 MB 往下试
 * （memmap.c 的 AllocSizes 表）—— 在 8 MB PSRAM 上第一档就成功，白占 6 MB。
 * 而 LoadROM(NULL) 是拿 ROM_AllocSize 当文件长度用的，所以那块必须换成
 * 「正好等于 ROM 大小」的缓冲，否则 snes9x 会以为这是个 6 MB 的卡带。
 *
 * 顺便也解决了 mmap 只读的问题：LoadROM 会就地改写（跳 512 字节头、
 * 补映射），不能直接喂 flash 指针。 */
/* 把 WRAM 从 PSRAM 挪进内部 SRAM。必须在 LoadROM() 之前 —— 它会走到
 * S9xReset()，那里 memset(Memory.RAM, 0x55, RAM_SIZE) 负责初始化内容。 */
static void relocate_wram_internal(void)
{
    uint8_t *ram = heap_caps_malloc(RAM_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ram) {
        ESP_LOGW(TAG, "WRAM %d KB 挪不进内部 SRAM，留在 PSRAM", RAM_SIZE / 1024);
        return;
    }
    free(Memory.RAM);
    Memory.RAM = ram;
    ESP_LOGI(TAG, "WRAM %d KB 已挪进内部 SRAM", RAM_SIZE / 1024);
}

static esp_err_t install_rom(const rom_store_entry_t *entry, uint32_t *rom_crc)
{
    if (Memory.ROM) {
        free(Memory.ROM - Memory.ROM_Offset);
        Memory.ROM = NULL;
        Memory.ROM_Offset = 0;
    }

    /* S9xInitMemory() 刚释放出的 6 MiB 先还给 PSRAM，再把压缩数据直接展开到
     * 最终缓冲。若先在外面解压再 memcpy，4 MiB DKC 会同时占两份而失败。 */
    rom_store_image_t image = {0};
    esp_err_t err = rom_store_load(entry, SNES_ROM_SLACK, &image);
    if (err != ESP_OK) return err;
    if (!image.owned) {
        rom_store_image_release(&image);
        return ESP_ERR_INVALID_STATE; /* extra_bytes 保证一定得到可写缓冲 */
    }

    Memory.ROM           = image.data;
    Memory.ROM_AllocSize = image.size; /* LoadROM(NULL) 把它当文件长度 */
    *rom_crc = image.crc32;
    return ESP_OK;
}

esp_err_t snes_emu_run(const rom_store_entry_t *entry, uint16_t launch_keys)
{
    if (!entry || entry->size < 1024) return ESP_ERR_INVALID_ARG;

    /* 菜单已经初始化过输入，这几次调用仍保持幂等。launch_keys 是确认瞬间
     * 捕获的状态，不会因为大 ROM 解压耗时而丢掉 X/Y 修饰键。 */
    input_serial_init();
    input_usb_init();
    input_gamepad_init();

    printf("\nROM: %s  (%u 字节，SNES)\n", entry->name,
           (unsigned)entry->size);
    display_stream_sync(black_strip, NULL);

    /* 先把 NES 预留的 128 KB 内部 SRAM 拿回来 —— 我们的帧缓冲要 119 KB，
     * 不这么做就只能落 PSRAM（实测那样渲染慢一倍多）。 */
    nes_emu_release_prealloc();

    esp_err_t err = alloc_buffers();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNES 缓冲分配失败：需要 2x%d KB 帧缓冲 + %d 字节音频缓冲",
                 SNES_FB_BYTES / 1024, SNES_AUDIO_MAX_FRAMES * 4);
        return err;
    }

    esp_err_t audio_err = audio_output_init(AUDIO_OUTPUT_SAMPLE_RATE);
    s_audio_ok = (audio_err == ESP_OK);
    if (!s_audio_ok) {
        ESP_LOGW(TAG, "MAX98357 音频未启动：%s，继续静音运行",
                 esp_err_to_name(audio_err));
    }

    /* 这一组取值照抄 retro-go main_snes.c，都是 SNES 时序常量，不是可调参数。 */
    Settings.CyclesPercentage   = 100;
    Settings.H_Max              = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL       = 20000;
    Settings.FrameTimeNTSC      = 16667;
    Settings.ControllerOption   = SNES_JOYPAD;
    Settings.HBlankStart        = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate  = AUDIO_OUTPUT_SAMPLE_RATE;
    Settings.SoundInputRate     = AUDIO_OUTPUT_SAMPLE_RATE;
    Settings.DisableSoundEcho   = false;
    Settings.InterpolatedSound  = true;

    if (!S9xInitDisplay())  { ESP_LOGE(TAG, "显示初始化失败");   return ESP_ERR_NO_MEM; }
    if (!S9xInitMemory())   { ESP_LOGE(TAG, "内存初始化失败");   return ESP_ERR_NO_MEM; }
    if (!S9xInitAPU())      { ESP_LOGE(TAG, "APU 初始化失败");   return ESP_FAIL; }
    if (!S9xInitSound(0, 0)){ ESP_LOGE(TAG, "声音初始化失败");   return ESP_FAIL; }
    if (!S9xInitGFX())      { ESP_LOGE(TAG, "图形初始化失败");   return ESP_FAIL; }

#if SNES_MEM_PROFILE == 1
    relocate_wram_internal();
#endif

    uint32_t rom_crc = 0;
    err = install_rom(entry, &rom_crc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ROM 拷入 PSRAM 失败（需要 %u KB）",
                 (unsigned)((entry->size + SNES_ROM_SLACK) / 1024));
        return err;
    }
    if (!LoadROM(NULL)) {
        ESP_LOGE(TAG, "ROM 解析失败（不是受支持的 SNES 卡带？）");
        return ESP_FAIL;
    }

    S9xSetPlaybackRate(Settings.SoundPlaybackRate);

    /* 目前只给 SMW 开即时存档。名称负责限制功能范围，ROM CRC 负责保证同名改版
     * 或不同区域版本不会误加载；分区挂载失败不影响模拟器本身。X/Y 两种入口
     * 都不删除存档，避免一次误按就丢掉仍可取回的进度。 */
    bool save_enabled = strcmp(Memory.ROMName, "SUPER MARIOWORLD") == 0;
    bool resumed = false;
    bool recovered_previous = false;
    bool cold_start_requested = false;
    if (save_enabled && snes_save_init(rom_crc) == ESP_OK) {
        cold_start_requested = (launch_keys & GAMEPAD_BIT_Y) != 0;
        recovered_previous = !cold_start_requested &&
                             (launch_keys & GAMEPAD_BIT_X) != 0;
        if (!cold_start_requested) {
            resumed = recovered_previous ? snes_save_load_previous(rom_crc)
                                         : snes_save_load_latest(rom_crc);
        }
    }

    esp_err_t rgb_err = rgb_led_init();
    if (rgb_err != ESP_OK) {
        ESP_LOGW(TAG, "板载 RGB 状态灯未启动：%s", esp_err_to_name(rgb_err));
    }

    printf("卡带：%s  映射 %s  %d fps  ROM %u KB\n",
           Memory.ROMName, Memory.LoROM ? "LoROM" : "HiROM",
           (int)Memory.ROMFramesPerSecond,
           (unsigned)(Memory.CalculatedSize / 1024));
    printf("内部 RAM 剩余 %u KB，PSRAM 剩余 %u KB\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    printf("开始模拟，跳帧 %d（画 1 帧跳 %d 帧）。\n\n",
           SNES_FRAMESKIP, SNES_FRAMESKIP);
    if (save_enabled) {
        printf("SMW 即时存档：同时长按 SELECT + A 1 秒保存；下次启动自动恢复。%s\n\n",
               cold_start_requested
                   ? "本次按住 Y 跳过恢复并从头开始，原存档仍保留。"
                   : (resumed ? (recovered_previous
                                      ? "本次按 X 尝试恢复了上一份状态。"
                                      : "本次已恢复上次状态。")
                              : "目前没有可恢复状态。"));
    }

    const int fps = Memory.ROMFramesPerSecond ?: 60;
    const int frame_period_us = 1000000 / fps;

    /* 每帧该产多少采样由卡带帧率决定：NTSC 400，PAL 480。
     * 取错了不只是浪费 CPU —— I2S 按固定速率消费，产多了队列会涨。 */
    s_audio_frames = AUDIO_OUTPUT_SAMPLE_RATE / fps;
    if (s_audio_frames > SNES_AUDIO_MAX_FRAMES) s_audio_frames = SNES_AUDIO_MAX_FRAMES;

    int     skip_frames  = 0;
    int     emu_frames   = 0;      /* 模拟了多少帧 */
    int     drawn_frames = 0;      /* 真正推屏多少帧 */
    int64_t emu_us       = 0;
    int64_t blit_us      = 0;
    int64_t audio_us     = 0;
    uint32_t pcm_peak    = 0;
    uint32_t pcm_nonzero = 0;
    uint32_t resume_zero_frames = 0;
    int64_t stat_t0      = esp_timer_get_time();
    int64_t next_frame   = stat_t0;
    int64_t save_hold_t0 = 0;
    bool save_latched    = false;
    int64_t exit_hold_t0 = 0;

    while (1) {
        s_pad_state = input_serial_poll() | input_gamepad_poll() | input_usb_poll();

        bool save_combo = save_enabled &&
                          (s_pad_state & SAVE_COMBO_BITS) == SAVE_COMBO_BITS;
        if (save_combo) {
            /* 组合键属于系统，不让 SMW 同时收到 SELECT/A。 */
            s_pad_state &= ~SAVE_COMBO_BITS;
            int64_t now = esp_timer_get_time();
            if (save_hold_t0 == 0) save_hold_t0 = now;

            if (!save_latched && now - save_hold_t0 >= SAVE_HOLD_US) {
                save_latched = true;
                show_save_notice("SAVING...", C_YELLOW);
                bool ok = snes_save_write(rom_crc);
                show_save_notice(ok ? "SAVE OK" : "SAVE FAILED",
                                 ok ? C_GREEN : C_RED);
                vTaskDelay(pdMS_TO_TICKS(700));

                /* 写 Flash 的停顿不算模拟性能，也不能让配速器误追赶几百帧。 */
                next_frame = stat_t0 = esp_timer_get_time();
                emu_frames = drawn_frames = 0;
                emu_us = blit_us = audio_us = 0;
                continue;
            }
        } else {
            save_hold_t0 = 0;
            save_latched = false;
        }

        bool exit_combo = (s_pad_state & EXIT_COMBO_BITS) == EXIT_COMBO_BITS;
        if (exit_combo) {
            /* 组合键属于系统，不让游戏同时收到 SELECT/START。 */
            s_pad_state &= ~EXIT_COMBO_BITS;
            int64_t now = esp_timer_get_time();
            if (exit_hold_t0 == 0) exit_hold_t0 = now;

            if (now - exit_hold_t0 >= EXIT_HOLD_US) {
                rgb_led_off();
                show_save_notice("EXITING...", C_YELLOW);
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            exit_hold_t0 = 0;
        }

        bool draw_frame = (skip_frames == 0);

        /* 换渲染目标必须在 S9xMainLoop 之前 —— S9xStartScreenRefresh 会拿
         * 当时的 GFX.Screen 去算 GFX.Delta（gfx.c:285）。这里只有一块渲染
         * 缓冲，所以是常量，但保持这个顺序，将来真做乒乓时不会踩坑。 */
        IPPU.RenderThisFrame = draw_frame;
        GFX.Screen = (uint8_t *)s_framebuf;

        int64_t t0 = esp_timer_get_time();
        S9xMainLoop();
        int64_t t1 = esp_timer_get_time();
        emu_us += t1 - t0;

#if !SNES_DIAG_NO_BLIT
        if (draw_frame) {
            /* 拷到影子缓冲再异步提交：display_stream 立刻返回，条带转换和
             * DMA 全在核 1 上跑，核 0 直接去模拟下一帧。只有上一帧还没推完
             * 时才会在这里阻塞 —— 天然的背压。 */
            memcpy(s_present, s_framebuf, SNES_FB_BYTES);
            display_stream_sized(snes_strip, s_present, DISP_W, DISP_H);
            drawn_frames++;
        }
#endif
        int64_t t2 = esp_timer_get_time();
        blit_us += t2 - t1;

        /* S9xMixSamples 不只是“输出声音”：它还推进包络、采样位置等会被
         * 即时存档序列化的混音器状态。即使菜单关了声音或 I2S 没起来也必须
         * 每帧调用并丢掉 PCM，否则 APU 与 SoundData 停在不同时间点，静音时
         * 保存出来的状态可能在恢复后一直无声。 */
        S9xMixSamples((int16_t *)s_soundbuf, s_audio_frames * 2);
        uint32_t frame_peak = 0;
        for (int i = 0; i < s_audio_frames * 2; i++) {
            int sample = s_soundbuf[i];
            uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
            if (magnitude > frame_peak) frame_peak = magnitude;
        }
        if (frame_peak > pcm_peak) pcm_peak = frame_peak;
        if (frame_peak != 0) pcm_nonzero++;
        if (s_audio_ok && audio_output_get_volume() > 0) {
            audio_output_submit_stereo(s_soundbuf, s_audio_frames);
        }
        audio_us += esp_timer_get_time() - t2;

        /* 旧固件静音时不推进混音器，两个实物存档槽都出现过这种状态：APU
         * 声称仍有活动声道，但 PCM 在短暂残音后永久为零。SoundData 与 SPC700
         * 的执行现场已经互相矛盾，重算音量或重启声道都只能响一两帧，无法
         * 无损修复即时位置。
         *
         * 连续两秒确认后冷重启 SNES 核心。S9xReset() 会清 CPU/PPU/APU 和
         * 工作 RAM，但故意不清 Memory.SRAM，所以仍能保留即时存档里最近一次
         * 游戏内保存的地图进度；代价是回到标题画面，丢失当前关卡的瞬时位置。
         * 两个 FAT 存档文件不删除，用户仍可用旧固件取回原始状态。 */
        if (resumed && frame_peak == 0 && APU.KeyedChannels != 0) {
            resume_zero_frames++;
        } else {
            resume_zero_frames = 0;
        }
        if (resumed && resume_zero_frames >= (uint32_t)(fps * 2)) {
            ESP_LOGW(TAG,
                     "即时存档音频状态已损坏（活动声道=%02x，连续%u帧PCM为零）；"
                     "保留SRAM进度并冷启动游戏",
                     APU.KeyedChannels, (unsigned)resume_zero_frames);
            S9xReset();
            S9xSetPlaybackRate(Settings.SoundPlaybackRate);
            memset(s_soundbuf, 0, SNES_AUDIO_MAX_FRAMES * 2 * sizeof(int16_t));
            resumed = false;
            resume_zero_frames = 0;
            skip_frames = 0;
            next_frame = stat_t0 = esp_timer_get_time();
            emu_frames = drawn_frames = 0;
            emu_us = blit_us = audio_us = 0;
            pcm_peak = pcm_nonzero = 0;
            continue;
        }

        skip_frames = (skip_frames == 0) ? SNES_FRAMESKIP : skip_frames - 1;

        next_frame += frame_period_us;
        int64_t now = esp_timer_get_time();
        if (next_frame > now) {
            int64_t wait_us = next_frame - now;
            if (wait_us > 1500) vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
            while (esp_timer_get_time() < next_frame) { }
        } else {
            next_frame = now;
            vTaskDelay(1);   /* 跑不满时也要喂 idle task / 看门狗 */
        }

        emu_frames++;
        now = esp_timer_get_time();
        if (now - stat_t0 >= 1000000) {
            int emu_fps10  = (int)(emu_frames * 10000000LL / (now - stat_t0));
            int draw_fps10 = (int)(drawn_frames * 10000000LL / (now - stat_t0));
            int headroom   = 100 - (int)((emu_us + blit_us + audio_us) * 100 /
                                          (now - stat_t0));
            printf("SNES 模拟 %d.%d fps / 推屏 %d.%d fps  "
                   "(模拟 %.1f + 拷贝 %.1f + 音频 %.1f ms/帧，CPU 余量 %d%%，"
                   "PCM峰值 %u，非零帧 %u/%d)\n",
                   emu_fps10 / 10, emu_fps10 % 10,
                   draw_fps10 / 10, draw_fps10 % 10,
                   (float)emu_us / 1000.0f / emu_frames,
                   (float)blit_us / 1000.0f / emu_frames,
                   (float)audio_us / 1000.0f / emu_frames,
                   headroom,
                   (unsigned)pcm_peak, (unsigned)pcm_nonzero, emu_frames);
            int fps_pct = draw_fps10 * 10 / fps;   /* draw_fps10 是 *10 定点，先乘 10 再除才是百分比 */
            rgb_led_report_perf(fps_pct > 100 ? 100 : fps_pct);
            emu_frames = drawn_frames = 0;
            emu_us = blit_us = audio_us = 0;
            pcm_peak = pcm_nonzero = 0;
            stat_t0 = now;
        }
    }

    return ESP_OK;
}
