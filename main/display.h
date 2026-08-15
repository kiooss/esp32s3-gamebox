/*
 * ST7789 SPI 显示层 —— 240x320 IPS，横屏使用为 320x240
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
 * 这块面板是满 240x320，正好等于 ST7789 的显存尺寸，所以**不需要 gap**。
 *
 * ⚠ 换成 170x320 那种窄屏时才要设 gap：那种面板只有 170 列，居中贴在
 * 240 列的显存上，列地址要整体偏移 (240-170)/2 = 35；横屏(swap_xy)之后
 * 这 35 落到 y 轴上，即 set_gap(0, 35)。满屏的这块设了反而会错位。
 *
 * 如果实际显示不对，按现象调这几个宏（每次只改一个）：
 *   画面整体偏移 / 边缘有花条  -> 调 GAP_X / GAP_Y（满屏应为 0）
 *   画面上下或左右颠倒        -> 翻转 MIRROR_X / MIRROR_Y
 *   颜色像底片（黑白反）      -> 翻转 INVERT_COLOR
 *   红蓝互换                  -> 翻转 BGR_ORDER
 */
#define DISP_SWAP_XY     true   /* true = 横屏 320x240 */
#define DISP_MIRROR_X    true
#define DISP_MIRROR_Y    false
#define DISP_GAP_X       0
#define DISP_GAP_Y       0
#define DISP_INVERT_COLOR true  /* IPS 屏基本都要 true，TN 屏多为 false */
#define DISP_BGR_ORDER   false

/* 面板横屏下的可见分辨率 */
#define DISP_W          320
#define DISP_H          240

/* ============ 画布 ============
 *
 * 画布不铺满整块屏。这里定义它的尺寸和落点，绘图坐标都是相对画布的。
 * 四周黑边由 display_init() 在开机时清一次，之后再不碰。
 *
 * ---- 288x224 是怎么来的 ----
 *
 * NES 裁掉上下各 8 行 overscan 之后是 256x224。竖向 1:1 直接用满 224 行。
 * 横向从 256 拉到 288，是为了修**像素宽高比**：
 *
 *   NES 在 NTSC 电视上的 PAR 是 8:7 ≈ 1.143，即像素不是方的，是偏宽的。
 *   256x224 原样显示（PAR 1.0）画面偏窄 12%，圆的东西看着是竖椭圆。
 *   拉到 288 之后 PAR = 288/256 = 1.125，误差只剩 1.6%。
 *
 * 288 = 256 * 9/8，即每 8 个源像素输出 9 个。选 9:8 而不是别的比例，是因为
 * NES 图块本来就是 8 像素宽且对齐的，复制点正好落在图块边界上，视觉最不
 * 突兀，而且内循环能 8x 展开、无分支无取模。
 *
 * ---- 现状：这块画布现在只有菜单和诊断画面在用 ----
 *
 * NES/SNES/GB/GBC/Genesis 都已经改走 display_stream_sized() 提交自己的
 * 原生尺寸，铺满整块 320x240 面板（GB/GBC 共用 gbc_emu.c 一套实现，各自
 * xxx_strip 里做缩放）。这里曾经记录过一条测算结论：铺满 320 宽会把 SPI
 * 传输占到 60fps 预算的 97%，只能靠掉到 30fps 换，而且 NES 跳帧会让精灵
 * 永久消失——上板实测已证伪，铺满后都能稳定 60fps，没有精灵丢失。DISP_FB_W/H 这块
 * 288x224 画布现在只被 rom_menu.c 和诊断画面（main.c/input_gamepad.c）
 * 使用，288 这个尺寸对它们没有特别意义，纯粹是历史遗留。
 */
#define DISP_FB_W       288
#define DISP_FB_H       224

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

/* 初始化 SPI + 面板 + 背光，分配两块可覆盖整屏宽度的条带缓冲，
 * 并在核 1 上起一个推屏任务。 */
esp_err_t display_init(void);

/* ============ 条带流式推屏 ============
 *
 * 这里**没有常驻帧缓冲**。默认 288x224 画布的 RGB565 要 126 KB，两块就是
 * 252 KB，内部 DMA 内存装不下（PSRAM 走不了 DMA，原因见 display.c）。
 *
 * 所以改成：只留两块最大 320x32 的小条带缓冲（各 20 KB），核 1 一边把第
 * N+1 条转换进一块、一边 DMA 推第 N 条，流水起来。调用方只要提供「把画布的
 * 第 y0..y0+h-1 行画进这块条带」的回调。
 *
 * 双缓冲并没有消失，只是下移了一层：NES 那边缓冲的是 8 位的 vidbuf（每块
 * 64 KB，比 RGB565 便宜一半），核 0 渲染下一帧的同时核 1 在读上一帧。
 * 帧时间仍然是 max(模拟, 推屏)，不是两者相加。
 *
 * 回调在**核 1** 上执行。传进去的 ctx 必须在整帧推完前保持有效。
 * strip 的每行跨度等于本次提交的画布宽度：display_stream() 是 DISP_FB_W，
 * display_stream_sized() 则是它的 width 参数。 */
typedef void (*disp_strip_fn)(uint16_t *strip, int y0, int h, void *ctx);

/* 提交一帧并立刻返回，实际转换+推送在核 1 上进行。
 * 只有上一帧还没推完时才阻塞 —— 天然把帧率限制在屏幕吃得下的上限。 */
void display_stream(disp_strip_fn fn, void *ctx);

/* 提交一帧自定义尺寸的居中画布。用于 Genesis 原生 320x224；width/height
 * 不能超过面板尺寸。其他模拟器继续调用 display_stream() 使用默认画布。 */
void display_stream_sized(disp_strip_fn fn, void *ctx, int width, int height);

/* 同上，但等这一帧推完才返回。ctx 指向栈上数据时用这个。 */
void display_stream_sync(disp_strip_fn fn, void *ctx);

/* 等待已提交的帧全部推完。 */
void display_wait_idle(void);

/* 背光亮度 0~100。BL 脚接 3V3 常亮时此函数无效果。 */
void display_backlight(int percent);

/* ---- 基本绘图 ----
 *
 * ⚠ 只能在 disp_strip_fn 回调**内部**调用 —— 它们画的是「当前条带」。
 * 坐标是相对整块画布的（左上角 0,0），落在当前条带之外的部分自动丢弃。
 *
 * 也就是说同一段绘制代码每帧会被调用 BAND_COUNT 次，每次画出其中一横条。
 * 菜单/诊断画面这类低帧率场景重复执行绘制列表的开销可以忽略。 */
void display_clear(uint16_t color);
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_rect(int x, int y, int w, int h, uint16_t color);      /* 1px 描边 */
void display_hline(int x, int y, int w, uint16_t color);
void display_vline(int x, int y, int h, uint16_t color);
void display_pixel(int x, int y, uint16_t color);

/* UTF-8 文字：ASCII 用 5x7，菜单所需汉字用 16x16 子集；scale 为整数倍放大。 */
void display_text(int x, int y, const char *s, uint16_t color, int scale);
