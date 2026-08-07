/*
 * ST7789 显示层实现 —— 基于 ESP-IDF 自带的 esp_lcd 组件
 *
 * 双缓冲 + 核 1 推屏任务。
 *
 * 为什么要这么做：esp_lcd_panel_draw_bitmap 在发下一条带的 CASET/RASET/RAMWR
 * 之前必须等上一条带的数据传完（命令和数据共用一条 SPI 总线），所以一轮推屏
 * 是**阻塞**的。单缓冲时「画图」和「推屏」只能串行，实测 40MHz 下推屏 21.8ms
 * + 画图 8.5ms = 30.3ms/帧，只有 33fps —— 明明带宽还没跑满。
 *
 * 改成两块缓冲、推屏丢给核 1 的独立任务之后，主任务画下一帧的同时核 1 在推
 * 上一帧，每帧耗时变成 max(画图, 推屏) 而不是两者之和。
 *
 * 两块缓冲各 256*192*2 = 96 KB，都放内部 SRAM（DMA 可达且比 PSRAM 快）。
 */

#include <string.h>
#include "display.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "disp";

/* 一次 DMA 传多少行。分条带是为了避开单次传输的长度上限，
 * 同时让 SPI 队列能流水起来。除不尽也没关系，最后一条短一点。
 * 48 是挑过的：192 / 48 = 4 条整带，比除不尽时少一轮 CASET/RASET/RAMWR。 */
#define BAND_LINES      48
#define BAND_COUNT      ((DISP_FB_H + BAND_LINES - 1) / BAND_LINES)
#define BAND_BYTES      (DISP_FB_W * BAND_LINES * 2)
#define FB_BYTES        (DISP_FB_W * DISP_FB_H * 2)

static esp_lcd_panel_handle_t   s_panel;
static esp_lcd_panel_io_handle_t s_io;

static uint16_t          *s_buf[2];      /* 两块帧缓冲 */
static int                s_back;        /* 当前后台缓冲下标，绘图写这块 */
static uint16_t          *s_sending;     /* 交给推屏任务的那块 */

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

/* 推屏任务：钉在核 1，把整帧切成条带排进 SPI 队列，等全部传完再报空闲。
 * 这期间核 0 的调用方可以自由地画下一帧。 */
static void blit_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_submit, portMAX_DELAY);

        for (int i = 0; i < BAND_COUNT; i++) {
            int y  = i * BAND_LINES;
            int h  = DISP_FB_H - y;
            if (h > BAND_LINES) h = BAND_LINES;

            esp_lcd_panel_draw_bitmap(s_panel,
                                      DISP_FB_X,         DISP_FB_Y + y,
                                      DISP_FB_X + DISP_FB_W, DISP_FB_Y + y + h,
                                      s_sending + (size_t)y * DISP_FB_W);
        }
        /* 等这一帧所有条带的 DMA 回执，之后这块缓冲才能被重新绘制 */
        for (int i = 0; i < BAND_COUNT; i++) {
            xSemaphoreTake(s_band_done, portMAX_DELAY);
        }

        xSemaphoreGive(s_idle);
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
    s_band_done = xSemaphoreCreateCounting(BAND_COUNT * 2, 0);
    s_submit    = xSemaphoreCreateBinary();
    s_idle      = xSemaphoreCreateBinary();
    if (!s_band_done || !s_submit || !s_idle) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_idle);     /* 开机时推屏任务是空闲的 */

    /* 两块帧缓冲必须都在内部 SRAM，而且这是**硬约束**，不能退到 PSRAM。
     *
     * 试过退 PSRAM，不行：spi_master 的 setup_priv_desc() 用 esp_ptr_dma_capable()
     * 判断缓冲能不能直接 DMA，而那个函数只认内部 DRAM 地址段（SOC_DMA_LOW/HIGH），
     * PSRAM 指针一律返回 false。于是驱动会**每条带**临时 malloc 一块内部 DMA
     * 缓冲再 memcpy 过去 —— 每帧多拷一整屏，还要每秒 240 次分配/释放 28 KB。
     * 数据最后照样落在内部 RAM，等于白绕一圈。
     *
     * 所以画布高度是被内部 RAM 反推出来的，不是随便定的，见 display.h。 */
    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_malloc(FB_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_buf[i]) {
            ESP_LOGE(TAG, "帧缓冲 %d 分配失败（每块需要 %d 字节内部 DMA 内存，"
                          "当前最大空闲块 %u 字节）—— 调小 display.h 的 DISP_FB_H",
                     i, FB_BYTES,
                     (unsigned)heap_caps_get_largest_free_block(
                         MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            return ESP_ERR_NO_MEM;
        }
        memset(s_buf[i], 0, FB_BYTES);
    }
    s_back = 0;

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
        .trans_queue_depth = BAND_COUNT + 2,
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
                  "画布%dx%d@(%d,%d), 双缓冲 2x%d KB",
             DISP_W, DISP_H, DISP_SPI_HZ / 1000000, DISP_GAP_X, DISP_GAP_Y,
             DISP_FB_W, DISP_FB_H, DISP_FB_X, DISP_FB_Y, FB_BYTES / 1024);
    return ESP_OK;
}

uint16_t *display_fb(void) { return s_buf[s_back]; }

void display_wait_idle(void)
{
    xSemaphoreTake(s_idle, portMAX_DELAY);
    xSemaphoreGive(s_idle);
}

void display_flush(void)
{
    /* 等推屏任务把上一帧交出去。这里是唯一的阻塞点，也正好起到帧率节流的作用。 */
    xSemaphoreTake(s_idle, portMAX_DELAY);

    s_sending = s_buf[s_back];
    s_back ^= 1;                /* 交换：调用方接着画另一块 */

    xSemaphoreGive(s_submit);
}

/* ================= 帧缓冲绘图 ================= */

void display_pixel(int x, int y, uint16_t color)
{
    if ((unsigned)x >= DISP_FB_W || (unsigned)y >= DISP_FB_H) return;
    s_buf[s_back][y * DISP_FB_W + x] = color;
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    /* 裁剪到屏内 */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISP_FB_W) w = DISP_FB_W - x;
    if (y + h > DISP_FB_H) h = DISP_FB_H - y;
    if (w <= 0 || h <= 0) return;

    uint16_t *fb = s_buf[s_back];
    for (int row = 0; row < h; row++) {
        uint16_t *p = fb + (size_t)(y + row) * DISP_FB_W + x;
        for (int col = 0; col < w; col++) p[col] = color;
    }
}

void display_clear(uint16_t color)
{
    display_fill_rect(0, 0, DISP_FB_W, DISP_FB_H, color);
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

void display_text(int x, int y, const char *s, uint16_t color, int scale)
{
    if (scale < 1) scale = 1;

    for (int cx = x; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        const uint8_t *g = FONT5X7[ch - 0x20];

        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (g[col] & (1 << row)) {
                    display_fill_rect(cx + col * scale, y + row * scale,
                                      scale, scale, color);
                }
            }
        }
        cx += 6 * scale;    /* 5 列字形 + 1 列间距 */
    }
}
