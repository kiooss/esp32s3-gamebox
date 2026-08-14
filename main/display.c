/*
 * ST7789 显示层实现 —— 基于 ESP-IDF 自带的 esp_lcd 组件
 *
 * 条带流式推屏 + 核 1 推屏任务。
 *
 * ---- 为什么不用常驻帧缓冲 ----
 *
 * 画布 288x224 的 RGB565 是 126 KB，双缓冲要 252 KB，内部 DMA 内存装不下
 * （PSRAM 不能直接 DMA，原因见下面 display_init 里的注释）。
 *
 * 所以这里只留两块最大 320x32 的条带缓冲（各 20 KB）：核 1 把第 N+1 条
 * 转换进一块的同时，DMA 正在推第 N 条，两者流水。整块画布由调用方提供的
 * disp_strip_fn 回调按条带现算，不需要落地。
 *
 * 双缓冲没有消失，只是下移到了 8 位的 NES vidbuf 那一层（每块 64 KB，比
 * RGB565 便宜一半）—— 核 0 渲染下一帧、核 1 读上一帧，帧时间仍然是
 * max(模拟, 推屏) 而不是两者相加。这一点很关键：如果把转换+推送放回核 0，
 * 每帧就变成「模拟 9ms 首尾相接推屏 14ms」= 23ms，直接超出 60fps 预算。
 *
 * ---- 条带切多大 ----
 *
 * esp_lcd_panel_draw_bitmap 在发下一条带的 CASET/RASET/RAMWR 之前必须等上
 * 一条带传完（命令和数据共用一条 SPI 总线），所以每条带的驱动开销是**串行
 * 叠加**在总线时间上的，不是并行掉的。
 *
 * 实测（256x192 = 98304 字节/帧，80MHz，理论 9.83 ms）：
 *
 *     条带数   4      8      12     24
 *     实测   10.34  10.83  11.31  12.78 ms
 *
 * 完美线性，斜率 122 us/条带，每帧固定开销约等于 0。122us 远不是命令本身的
 * 总线时间（11 字节只值 1us），是每次 draw_bitmap 的驱动/ISR/信号量往返。
 *
 * 所以条带**越少越快**，只受条带缓冲内存和流水粒度的约束。取 32 行/条：
 * 224/32 = 7 条整带，开销 0.86 ms，缓冲 2x18 KB。
 * 若取 16 行会多花 0.86ms，取 8 行多花 2.6ms —— 都不划算。
 */

#include <string.h>
#include "display.h"
#include "menu_font.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "disp";

/* 一条带多少行。选值理由见文件头的实测表：32 行 -> 224/32 = 7 条整带。 */
#define BAND_LINES      32
#define MAX_BAND_COUNT  ((DISP_H + BAND_LINES - 1) / BAND_LINES)
#define BAND_BYTES      (DISP_W * BAND_LINES * 2)

_Static_assert(MAX_BAND_COUNT >= 2, "条带数至少 2，否则流水不起来");

/* 每秒往串口打一行推屏耗时。调条带尺寸/画布尺寸时很有用，平时可以关掉。 */
#define DISP_PROFILE    1

static esp_lcd_panel_handle_t   s_panel;
static esp_lcd_panel_io_handle_t s_io;

static uint16_t          *s_strip[2];    /* 两块条带缓冲，转换/DMA 乒乓 */
static disp_strip_fn      s_fn;          /* 本帧的绘制回调 */
static void              *s_ctx;
static int                s_frame_w = DISP_FB_W;
static int                s_frame_h = DISP_FB_H;
static int                s_frame_x = DISP_FB_X;
static int                s_frame_y = DISP_FB_Y;

/* 绘图原语的作用目标：当前正在填的那条带。只在核 1 的回调里有效。 */
static uint16_t          *s_cur;         /* 当前条带缓冲 */
static int                s_cur_y0;      /* 它对应画布的起始行 */
static int                s_cur_h;       /* 它有多少行 */
static int                s_cur_w;       /* 本帧画布宽度，也是条带行跨度 */
static int                s_cur_frame_h; /* 本帧画布总高度 */

static SemaphoreHandle_t  s_band_done;   /* 计数：每条带 DMA 传完 +1 */
static SemaphoreHandle_t  s_submit;      /* 二值：有新帧待推 */
static SemaphoreHandle_t  s_idle;        /* 二值：推屏任务空闲，可以收新帧 */

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev,
                                    void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_band_done, &hp);
    return hp == pdTRUE;
}

/* 推屏任务：钉在核 1。逐条带调用绘制回调、排进 SPI 队列，全部传完再报空闲。
 * 这期间核 0 的调用方可以自由地算下一帧。 */
static void blit_task(void *arg)
{
#if DISP_PROFILE
    int64_t acc = 0, t0 = esp_timer_get_time();
    int     n   = 0;
#endif

    for (;;) {
        xSemaphoreTake(s_submit, portMAX_DELAY);
        const int frame_w = s_frame_w;
        const int frame_h = s_frame_h;
        const int frame_x = s_frame_x;
        const int frame_y = s_frame_y;
        const int band_count = (frame_h + BAND_LINES - 1) / BAND_LINES;
#if DISP_PROFILE
        int64_t push_t0 = esp_timer_get_time();
#endif

        for (int i = 0; i < band_count; i++) {
            int y0 = i * BAND_LINES;
            int h  = frame_h - y0;
            if (h > BAND_LINES) h = BAND_LINES;

            /* 乒乓：第 i 条要写的缓冲正是第 i-2 条用过的那块，
             * 得先确认它的 DMA 已经回执。
             *
             * 实际上 draw_bitmap 内部会等上一条传完才发命令，所以这一步几乎
             * 总是立刻返回。但那是驱动的实现细节，不是契约 —— 显式等一次让
             * 「不会覆写正在 DMA 的缓冲」这件事在本地就能看明白，代价为零。 */
            if (i >= 2) xSemaphoreTake(s_band_done, portMAX_DELAY);

            s_cur    = s_strip[i & 1];
            s_cur_y0 = y0;
            s_cur_h  = h;
            s_cur_w  = frame_w;
            s_cur_frame_h = frame_h;
            s_fn(s_cur, y0, h, s_ctx);

            esp_lcd_panel_draw_bitmap(s_panel,
                                      frame_x,           frame_y + y0,
                                      frame_x + frame_w, frame_y + y0 + h,
                                      s_cur);
        }
        /* 循环里至多漏收两条（i=0、1 时没等），这里补齐最后的 DMA 回执。 */
        int pending = band_count < 2 ? band_count : 2;
        for (int i = 0; i < pending; i++) {
            xSemaphoreTake(s_band_done, portMAX_DELAY);
        }

#if DISP_PROFILE
        acc += esp_timer_get_time() - push_t0;
        n++;
#endif
        xSemaphoreGive(s_idle);     /* 先放行核 0，printf 别卡在关键路径上 */

#if DISP_PROFILE
        int64_t now = esp_timer_get_time();
        if (now - t0 >= 1000000) {
            printf("推屏 %.2f ms/帧（%d 行/条 x %d 条 = %d 字节，%d 帧）\n",
                   (float)acc / 1000.0f / n, BAND_LINES, band_count,
                   frame_w * frame_h * 2, n);
            acc = 0; n = 0; t0 = now;
        }
#endif
    }
}

/* 用一行像素反复推，把整块面板刷成同一个颜色。
 * 只在开机时调一次，用来把画布之外的黑边清干净。
 * 复用同一个行缓冲，所以每推一行都要等它传完。 */
static void fill_whole_panel(uint16_t color)
{
    uint16_t *line = heap_caps_malloc(DISP_W * 2,
                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!line) return;

    for (int i = 0; i < DISP_W; i++) line[i] = color;

    for (int y = 0; y < DISP_H; y++) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, DISP_W, y + 1, line);
        xSemaphoreTake(s_band_done, portMAX_DELAY);
    }
    free(line);
}

static void backlight_init(void)
{
    if (DISP_PIN_BL < 0) return;

    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {
        .gpio_num   = DISP_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void display_backlight(int percent)
{
    if (DISP_PIN_BL < 0) return;
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    /* 10 位分辨率下满亮是 1024 而不是 1023 —— 用 1023 会让每个 PWM 周期
     * 留一个 1/1024 的熄灭窗口，虽然 5kHz 下肉眼基本看不出，但那不是真正的常亮。 */
    uint32_t duty = (1024 * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t display_init(void)
{
    s_band_done = xSemaphoreCreateCounting(MAX_BAND_COUNT * 2, 0);
    s_submit    = xSemaphoreCreateBinary();
    s_idle      = xSemaphoreCreateBinary();
    if (!s_band_done || !s_submit || !s_idle) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_idle);     /* 开机时推屏任务是空闲的 */

    /* 两块条带缓冲必须都在内部 SRAM，这是**硬约束**，不能退到 PSRAM。
     *
     * 试过退 PSRAM，不行：spi_master 的 setup_priv_desc() 用 esp_ptr_dma_capable()
     * 判断缓冲能不能直接 DMA，而那个函数只认内部 DRAM 地址段（SOC_DMA_LOW/HIGH），
     * PSRAM 指针一律返回 false。于是驱动会**每条带**临时 malloc 一块内部 DMA
     * 缓冲再 memcpy 过去 —— 数据最后照样落在内部 RAM，等于白绕一圈还多一次拷贝。
     *
     * 好在条带才 18 KB，和以前 96 KB 的整帧缓冲不是一个量级，分配压力小得多。 */
    for (int i = 0; i < 2; i++) {
        s_strip[i] = heap_caps_malloc(BAND_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_strip[i]) {
            ESP_LOGE(TAG, "条带缓冲 %d 分配失败（每块需要 %d 字节内部 DMA 内存，"
                          "当前最大空闲块 %u 字节）—— 调小 display.c 的 BAND_LINES",
                     i, BAND_BYTES,
                     (unsigned)heap_caps_get_largest_free_block(
                         MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            return ESP_ERR_NO_MEM;
        }
        memset(s_strip[i], 0, BAND_BYTES);
    }

    backlight_init();

    spi_bus_config_t bus = {
        .sclk_io_num     = DISP_PIN_SCLK,
        .mosi_io_num     = DISP_PIN_MOSI,
        .miso_io_num     = -1,          /* 只写屏，不读回 */
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = BAND_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = DISP_PIN_CS,
        .dc_gpio_num       = DISP_PIN_DC,
        .spi_mode          = 0,
        .pclk_hz           = DISP_SPI_HZ,
        .trans_queue_depth = MAX_BAND_COUNT + 2,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .on_color_trans_done = on_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DISP_PIN_RST,
        .rgb_ele_order  = DISP_BGR_ORDER ? LCD_RGB_ELEMENT_ORDER_BGR
                                         : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, DISP_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, DISP_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, DISP_MIRROR_X, DISP_MIRROR_Y));
    /* 关键：170 列的面板居中贴在 240 列的显存上，偏移 35 */
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, DISP_GAP_X, DISP_GAP_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* 把整块面板（含画布之外的黑边）清一次。之后每帧只推画布那块，
     * 黑边再也不会被碰到。必须在推屏任务起来之前做，好独占 s_band_done。 */
    fill_whole_panel(C_BLACK);

    display_backlight(100);

    /* 推屏任务钉在核 1：核 0 画图，核 1 推屏，互不抢占 */
    BaseType_t ok = xTaskCreatePinnedToCore(blit_task, "lcd_blit", 3072,
                                            NULL, 5, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "推屏任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ST7789 就绪 面板%dx%d @ %d MHz, gap(%d,%d), "
                  "默认画布%dx%d@(%d,%d), 最大条带宽 %d, 条带流式 %d 行, 缓冲 2x%d KB",
             DISP_W, DISP_H, DISP_SPI_HZ / 1000000, DISP_GAP_X, DISP_GAP_Y,
             DISP_FB_W, DISP_FB_H, DISP_FB_X, DISP_FB_Y,
             DISP_W, BAND_LINES, BAND_BYTES / 1024);
    return ESP_OK;
}

void display_wait_idle(void)
{
    xSemaphoreTake(s_idle, portMAX_DELAY);
    xSemaphoreGive(s_idle);
}

void display_stream(disp_strip_fn fn, void *ctx)
{
    display_stream_sized(fn, ctx, DISP_FB_W, DISP_FB_H);
}

void display_stream_sized(disp_strip_fn fn, void *ctx, int width, int height)
{
    if (!fn || width <= 0 || height <= 0 || width > DISP_W || height > DISP_H) {
        ESP_LOGE(TAG, "拒绝非法画布 %dx%d（面板 %dx%d）", width, height, DISP_W, DISP_H);
        return;
    }

    /* 等推屏任务把上一帧收完。这里是唯一的阻塞点，也正好起到帧率节流的作用。 */
    xSemaphoreTake(s_idle, portMAX_DELAY);

    s_fn  = fn;
    s_ctx = ctx;
    s_frame_w = width;
    s_frame_h = height;
    s_frame_x = (DISP_W - width) / 2;
    s_frame_y = (DISP_H - height) / 2;

    xSemaphoreGive(s_submit);
}

void display_stream_sync(disp_strip_fn fn, void *ctx)
{
    display_stream(fn, ctx);
    display_wait_idle();
}

/* ================= 绘图：目标是「当前条带」 =================
 *
 * 坐标都是相对整块画布的，落在当前条带之外的行直接丢掉。
 * 调用方（菜单、诊断画面）因此完全不用知道条带的存在，同一段绘制代码
 * 被逐条带重复调用就是了。 */

void display_pixel(int x, int y, uint16_t color)
{
    if ((unsigned)x >= (unsigned)s_cur_w) return;
    int row = y - s_cur_y0;
    if ((unsigned)row >= (unsigned)s_cur_h) return;
    s_cur[(size_t)row * s_cur_w + x] = color;
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    /* 先裁到画布 */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_cur_w) w = s_cur_w - x;
    if (y + h > s_cur_frame_h) h = s_cur_frame_h - y;
    if (w <= 0 || h <= 0) return;

    /* 再裁到当前条带 */
    int y_lo = y > s_cur_y0 ? y : s_cur_y0;
    int y_hi = (y + h) < (s_cur_y0 + s_cur_h) ? (y + h) : (s_cur_y0 + s_cur_h);
    if (y_lo >= y_hi) return;

    for (int yy = y_lo; yy < y_hi; yy++) {
        uint16_t *p = s_cur + (size_t)(yy - s_cur_y0) * s_cur_w + x;
        for (int col = 0; col < w; col++) p[col] = color;
    }
}

void display_clear(uint16_t color)
{
    display_fill_rect(0, 0, s_cur_w, s_cur_frame_h, color);
}

void display_hline(int x, int y, int w, uint16_t color)
{
    display_fill_rect(x, y, w, 1, color);
}

void display_vline(int x, int y, int h, uint16_t color)
{
    display_fill_rect(x, y, 1, h, color);
}

void display_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    display_hline(x, y,         w, color);
    display_hline(x, y + h - 1, w, color);
    display_vline(x,         y, h, color);
    display_vline(x + w - 1, y, h, color);
}

/* ---- 5x7 点阵字库，ASCII 0x20~0x7E，每字符 5 列，每列低 7 位为一行 ---- */
static const uint8_t FONT5X7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, /*   ! */
    {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14}, /* " # */
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, /* $ % */
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, /* & ' */
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, /* ( ) */
    {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08}, /* * + */
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, /* , - */
    {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02}, /* . / */
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, /* 0 1 */
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, /* 2 3 */
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, /* 4 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, /* 6 7 */
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, /* 8 9 */
    {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00}, /* : ; */
    {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14}, /* < = */
    {0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06}, /* > ? */
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E}, /* @ A */
    {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22}, /* B C */
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, /* D E */
    {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A}, /* F G */
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, /* H I */
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, /* J K */
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x04,0x02,0x7F}, /* L M */
    {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E}, /* N O */
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, /* P Q */
    {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31}, /* R S */
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F}, /* T U */
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, /* V W */
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, /* X Y */
    {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00}, /* Z [ */
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, /* \ ] */
    {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40}, /* ^ _ */
    {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78}, /* ` a */
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, /* b c */
    {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18}, /* d e */
    {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E}, /* f g */
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, /* h i */
    {0x20,0x40,0x44,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00}, /* j k */
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78}, /* l m */
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, /* n o */
    {0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C}, /* p q */
    {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20}, /* r s */
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, /* t u */
    {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C}, /* v w */
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C}, /* x y */
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, /* z { */
    {0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00}, /* | } */
    {0x08,0x08,0x2A,0x1C,0x08},                             /* ~   */
};

/* 菜单标题来自 flash 里的 UTF-8 文件名。这里只解码标准 UTF-8；坏序列每次
 * 消耗一个字节并画成问号，避免卡在同一个指针上死循环。 */
static uint32_t utf8_next(const char **text)
{
    const uint8_t *s = (const uint8_t *)*text;
    uint32_t cp;

    if (s[0] < 0x80) {
        *text = (const char *)(s + 1);
        return s[0];
    }
    if (s[0] >= 0xC2 && s[0] <= 0xDF && (s[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *text = (const char *)(s + 2);
        return cp;
    }
    if (s[0] >= 0xE0 && s[0] <= 0xEF &&
        (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
        (s[0] != 0xE0 || s[1] >= 0xA0) &&
        (s[0] != 0xED || s[1] < 0xA0)) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12) |
             ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *text = (const char *)(s + 3);
        return cp;
    }

    *text = (const char *)(s + 1);
    return '?';
}

void display_text(int x, int y, const char *s, uint16_t color, int scale)
{
    if (scale < 1) scale = 1;

    int cx = x;
    while (*s) {
        uint32_t codepoint = utf8_next(&s);
        const uint8_t *han = codepoint > 0x7E ? menu_font_glyph(codepoint) : NULL;

        if (han) {
            for (int row = 0; row < 16; row++) {
                uint16_t bits = ((uint16_t)han[row * 2] << 8) | han[row * 2 + 1];
                for (int col = 0; col < 16; col++) {
                    if (bits & (0x8000u >> col)) {
                        display_fill_rect(cx + col * scale, y + row * scale,
                                          scale, scale, color);
                    }
                }
            }
            cx += 17 * scale;   /* 16 列字形 + 1 列间距 */
            continue;
        }

        unsigned char ch = codepoint >= 0x20 && codepoint <= 0x7E
                         ? (unsigned char)codepoint : '?';
        const uint8_t *ascii = FONT5X7[ch - 0x20];

        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (ascii[col] & (1 << row)) {
                    display_fill_rect(cx + col * scale, y + row * scale,
                                      scale, scale, color);
                }
            }
        }
        cx += 6 * scale;    /* 5 列字形 + 1 列间距 */
    }
}
