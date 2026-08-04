/*
 * ST7789 SPI 显示层 —— 1.9" 170x320 IPS，横屏使用为 320x170
 *
 * 对上层只暴露「一块 RGB565 帧缓冲 + 推屏」这一件事，
 * 后面接 NES 模拟器时上层代码不用动。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ============ 接线（改这里就能换脚） ============
 *
 * 选脚原则：
 *   - SCK/MOSI/CS 用 SPI2(FSPI) 的 IOMUX 原生脚，才能跑到 80MHz；
 *     换成别的脚会自动走 GPIO 矩阵，上限降到 40MHz（能用，但慢一半）
 *   - 避开 GPIO33~37（Octal PSRAM 占用）
 *   - 避开 GPIO19/20（原生 USB D-/D+）
 *   - 避开 GPIO0/3/45/46（strapping，46 还是只读）
 *
 * 换屏时只改这几个宏 + 下面的方向/偏移即可，上层代码不用动。
 */
#define DISP_PIN_SCLK   12      /* 屏丝印 SCL / SCK   —— IOMUX FSPICLK */
#define DISP_PIN_MOSI   11      /* 屏丝印 SDA / MOSI  —— IOMUX FSPID   */
#define DISP_PIN_CS     10      /* 屏丝印 CS，没这个脚就填 -1          */
#define DISP_PIN_DC     14      /* 屏丝印 DC / RS                       */
#define DISP_PIN_RST    13      /* 屏丝印 RES / RST，没有就填 -1       */
#define DISP_PIN_BL      9      /* 屏丝印 BLK / LED，直接接 3V3 就填 -1 */

/* ============ 时序 ============
 *
 * 杜邦线飞线时先用 40MHz。稳定之后可以试着往上加：
 * 焊死/排线短的话 80MHz 一般没问题，满屏刷新时间直接减半。
 * 症状：太快会花屏、雪花、颜色错乱 —— 降回 40 即可。
 */
#define DISP_SPI_HZ     (80 * 1000 * 1000)

/* ============ 面板方向与偏移 ============
 *
 * ST7789 的显存是 240x320，而这块面板只有 170 列，是「居中」贴的，
 * 所以列地址要整体偏移 (240-170)/2 = 35。不设这个 gap 画面就会
 * 错位、边上出现花条 —— 这是 170x320 屏最常见的坑。
 *
 * 横屏(swap_xy)之后，这 35 的偏移落到 y 轴上，所以是 set_gap(0, 35)。
 *
 * 如果实际显示不对，按现象调这三个宏（每次只改一个）：
 *   画面整体偏移 / 边缘有花条  -> 调 GAP_X / GAP_Y（试 35 和 0 互换）
 *   画面上下或左右颠倒        -> 翻转 MIRROR_X / MIRROR_Y
 *   颜色像底片（黑白反）      -> 翻转 INVERT_COLOR
 *   红蓝互换                  -> 翻转 BGR_ORDER
 */
#define DISP_SWAP_XY     true   /* true = 横屏 320x170 */
#define DISP_MIRROR_X    true
#define DISP_MIRROR_Y    false
#define DISP_GAP_X       0
#define DISP_GAP_Y       35
#define DISP_INVERT_COLOR true  /* IPS 屏基本都要 true，TN 屏多为 false */
#define DISP_BGR_ORDER   false

/* 面板横屏下的可见分辨率 */
#define DISP_W          320
#define DISP_H          170

/* ============ 画布 ============
 *
 * 帧缓冲不一定要铺满整块屏。这里定义它的尺寸和落点，绘图坐标都是相对画布的。
 *
 * 跑 NES 时画布就是 NES 画面区 256x168，居中放，两侧黑边由 display_init()
 * 在开机时清一次、之后再不碰。这样做有三个好处：
 *   - 每块缓冲从 106 KB 降到 84 KB，两块省下 44 KB 内部 RAM
 *   - 每帧少推 21% 的数据，撕裂窗口从 10.9 ms 缩到 8.6 ms
 *   - 静止的黑边不用每帧重画
 *
 * 想让画布铺满整屏（比如做菜单或者点屏诊断），把这两个改成 DISP_W / DISP_H。
 */
#define DISP_FB_W       256
#define DISP_FB_H       168

#define DISP_FB_X       ((DISP_W - DISP_FB_W) / 2)
#define DISP_FB_Y       ((DISP_H - DISP_FB_H) / 2)

/* RGB565 颜色构造。
 *
 * ⚠ 帧缓冲里存的是**字节交换后**（大端）的 RGB565。
 *
 * ST7789 走 SPI 时按大端接收 16 位像素（高字节先出），而 DMA 是照内存顺序
 * 逐字节发的，小端机器上存 0xF800 会先发出 0x00 再发 0xF8 —— 到屏上就变成
 * 0x00F8（暗蓝）。esp_lcd 没有提供字节序开关（它的 RGB/BGR 选项管的是
 * MADCTL 的红蓝通道顺序，不是字节序），所以只能在这里换好。
 * 这也是 ESP-IDF 的 LVGL 移植都要开 LV_COLOR_16_SWAP 的原因。
 *
 * 参数是常量时 __builtin_bswap16 会在编译期折叠，运行时零开销；
 * 参数是变量时也只是一条指令。
 *
 * 直接往帧缓冲里写像素的代码（比如 NES 的调色板查表）同样要存大端值。 */
#define RGB565(r, g, b) \
    ((uint16_t)__builtin_bswap16(                                     \
        (uint16_t)(((((r) & 0xF8) << 8)) | ((((g) & 0xFC) << 3)) | ((b) >> 3))))

#define C_BLACK   RGB565(0,   0,   0)
#define C_WHITE   RGB565(255, 255, 255)
#define C_RED     RGB565(255, 0,   0)
#define C_GREEN   RGB565(0,   255, 0)
#define C_BLUE    RGB565(0,   0,   255)
#define C_YELLOW  RGB565(255, 255, 0)
#define C_CYAN    RGB565(0,   255, 255)
#define C_MAGENTA RGB565(255, 0,   255)
#define C_GRAY    RGB565(128, 128, 128)

/* 初始化 SPI + 面板 + 背光，分配两块帧缓冲，并在核 1 上起一个推屏任务。
 * 成功后 display_fb() 才有效。 */
esp_err_t display_init(void);

/* 当前**后台**缓冲的首地址，DISP_FB_W*DISP_FB_H 个 uint16_t，行优先。写它就是画画。
 *
 * 注意：双缓冲，这个指针在每次 display_flush() 之后都会变，
 * 所以不要跨帧缓存它 —— 每帧重新取一次。 */
uint16_t *display_fb(void);

/* 提交当前后台缓冲，交换前后台，然后立刻返回。
 *
 * 真正的 SPI 推送由核 1 上的推屏任务完成，与调用方后续的绘图并行。
 * 只有在上一帧还没推完时才会阻塞 —— 也就是说这个函数天然把帧率
 * 限制在屏幕能吃下的上限，上层不用自己做节流。
 *
 * 交换后拿到的新后台缓冲里是**上上帧**的内容，不是空白。
 * 每帧整屏重画的话无所谓；只画局部的话记得先 display_clear()。 */
void display_flush(void);

/* 等待已提交的帧全部推完。一般不需要调，除非要精确测时序。 */
void display_wait_idle(void);

/* 背光亮度 0~100。BL 脚接 3V3 常亮时此函数无效果。 */
void display_backlight(int percent);

/* ---- 帧缓冲上的基本绘图，坐标越界会被裁剪 ---- */
void display_clear(uint16_t color);
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_rect(int x, int y, int w, int h, uint16_t color);      /* 1px 描边 */
void display_hline(int x, int y, int w, uint16_t color);
void display_vline(int x, int y, int h, uint16_t color);
void display_pixel(int x, int y, uint16_t color);

/* 5x7 点阵字符，scale 为整数倍放大。只支持 ASCII 0x20~0x7E */
void display_text(int x, int y, const char *s, uint16_t color, int scale);
