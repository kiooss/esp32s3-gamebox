/*
 * JoyStick Shield V1.A 输入实现
 *
 * 两件事：4 个数字按键 + 1 个模拟摇杆转成十字键。
 *
 * ---- 为什么按键要开内部上拉 ----
 *
 * 这块 Shield 的按键只是把引脚短到 GND，板上**没有上拉电阻** —— 原设计是靠
 * Arduino 的内部上拉。所以这边必须开 GPIO_PULLUP_ENABLE，否则松手时引脚悬空，
 * 读到的是随机噪声。按下 = 低电平。
 *
 * ---- 为什么摇杆要死区 + 迟滞 ----
 *
 * 摇杆是两个电位器，中位电压约 VCC/2，但实际有几十到几百 LSB 的偏差，而且
 * 松手回中是个渐变过程，不是干脆的跳变。直接跟中点比大小的话，静止时会在
 * 阈值附近反复横跳，游戏里表现为马里奥自己抽搐。
 *
 * 两道防线：
 *   1. 死区：偏离中位超过 ON 阈值才算按下
 *   2. 迟滞：已经按下之后，要回到更小的 OFF 阈值以内才算松开
 * 两个阈值拉开距离，噪声就没法在中间来回触发了。
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "input_gamepad.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nofrendo.h"

static const char *TAG = "pad";

/* 12 位 ADC，满量程 4095，中位理论值 2048。
 * 阈值是相对中位的偏移量：约 1/3 行程按下，约 1/5 行程松开。
 * 觉得迟钝就把 ON 调小，觉得会漂就把两个都调大。 */
#define AXIS_ON     700
#define AXIS_OFF    450

/* ---- 摇杆「在不在」的判据 ----
 *
 * 没接线时 ADC 脚是悬空的，读数可能是任意值。要是当成中位用，静止时 d 会常年
 * 超过阈值 —— 表现为马里奥自己一直往一个方向跑，而且看不出为什么。
 *
 * 所以开机校准时同时验两件事，任一不过就**把摇杆整个禁掉**（按键照常工作）：
 *   1. 平均值离理论中位 2048 不能太远  -> 排除悬空到某个极端、或上电时手推着摇杆
 *   2. 多次采样的抖动不能太大          -> 悬空脚会飘，接了电位器的脚很稳
 */
#define CENTER_MAX_DRIFT  600
#define CENTER_MAX_SPREAD 300

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_ch_x, s_ch_y;
static int  s_center_x = 2048, s_center_y = 2048;

/* 按键和摇杆分开记「能不能用」：摇杆没接线也不该影响按键。 */
static bool s_btn_ok;
static bool s_axes_ok;

static uint8_t s_dir;           /* 方向位的当前状态，迟滞要用 */
static uint8_t s_last_state;    /* 只在变化时打日志 */

static const struct { int pin; uint8_t mask; const char *name; } BUTTONS[] = {
    { PAD_PIN_A,      NES_PAD_A,      "A"      },
    { PAD_PIN_B,      NES_PAD_B,      "B"      },
    { PAD_PIN_SELECT, NES_PAD_SELECT, "SELECT" },
    { PAD_PIN_START,  NES_PAD_START,  "START"  },
};
#define BUTTON_COUNT  (sizeof(BUTTONS) / sizeof(BUTTONS[0]))

/* GPIO 号 -> ADC1 通道号。ESP32-S3 上 ADC1 是 GPIO1~10 依次对应 CH0~CH9。 */
static bool gpio_to_adc1(int gpio, adc_channel_t *out)
{
    if (gpio < 1 || gpio > 10) return false;
    *out = (adc_channel_t)(gpio - 1);
    return true;
}

static int read_axis(adc_channel_t ch)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc, ch, &raw) != ESP_OK) return 2048;
    return raw;
}

/* 一个轴的死区+迟滞判定。
 * neg/pos 是这个轴两个方向对应的位（比如 X 轴是 LEFT/RIGHT）。 */
static uint8_t axis_bits(int value, int center, bool invert,
                         uint8_t neg, uint8_t pos, uint8_t prev)
{
    int d = value - center;
    if (invert) d = -d;

    /* 已经按着的方向用较小的 OFF 阈值判断松开，这就是迟滞 */
    int thresh_neg = (prev & neg) ? AXIS_OFF : AXIS_ON;
    int thresh_pos = (prev & pos) ? AXIS_OFF : AXIS_ON;

    if (d <= -thresh_neg) return neg;
    if (d >=  thresh_pos) return pos;
    return 0;
}

static bool buttons_init(void)
{
    uint64_t mask = 0;
    for (unsigned i = 0; i < BUTTON_COUNT; i++) {
        mask |= 1ULL << BUTTONS[i].pin;
    }
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,    /* 板上没有上拉，必须开 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "按键 GPIO 配置失败");
        return false;
    }
    return true;
}

/* 开 ADC 并校准中位。返回 false 表示摇杆这一路不可信，poll() 会只报按键。 */
static bool axes_init(void)
{
    if (!gpio_to_adc1(PAD_PIN_X, &s_ch_x) || !gpio_to_adc1(PAD_PIN_Y, &s_ch_y)) {
        ESP_LOGE(TAG, "摇杆引脚必须在 GPIO1~10（ADC1 的范围），当前 X=%d Y=%d",
                 PAD_PIN_X, PAD_PIN_Y);
        return false;
    }

    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 初始化失败");
        return false;
    }

    /* 12 dB 衰减，量程约 0~3.1V。摇杆跨在 3V3 上，两端会轻微削顶，
     * 但我们只做阈值判断，不关心绝对值，无所谓。 */
    adc_oneshot_chan_cfg_t ch = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(s_adc, s_ch_x, &ch) != ESP_OK ||
        adc_oneshot_config_channel(s_adc, s_ch_y, &ch) != ESP_OK) {
        ESP_LOGE(TAG, "ADC 通道配置失败");
        return false;
    }

    /* 中位校准：多采几次取平均压掉噪声，同时记下极差用来判断脚是不是悬空 */
    int sx = 0, sy = 0;
    int lo_x = 4096, hi_x = -1, lo_y = 4096, hi_y = -1;
    for (int i = 0; i < 16; i++) {
        int x = read_axis(s_ch_x), y = read_axis(s_ch_y);
        sx += x; sy += y;
        if (x < lo_x) lo_x = x;
        if (x > hi_x) hi_x = x;
        if (y < lo_y) lo_y = y;
        if (y > hi_y) hi_y = y;
    }
    sx /= 16;
    sy /= 16;

    if (abs(sx - 2048) > CENTER_MAX_DRIFT || abs(sy - 2048) > CENTER_MAX_DRIFT) {
        ESP_LOGW(TAG, "摇杆中位离 2048 太远（X=%d Y=%d）：线没接对，"
                      "或者上电时手推着摇杆。这一路先禁掉，按键照常用", sx, sy);
        return false;
    }
    if (hi_x - lo_x > CENTER_MAX_SPREAD || hi_y - lo_y > CENTER_MAX_SPREAD) {
        ESP_LOGW(TAG, "摇杆读数抖得太厉害（X 极差 %d，Y 极差 %d）：ADC 脚大概是悬空的。"
                      "这一路先禁掉，按键照常用", hi_x - lo_x, hi_y - lo_y);
        return false;
    }

    s_center_x = sx;
    s_center_y = sy;
    return true;
}

void input_gamepad_show(void)
{
    if (!s_axes_ok) {
        ESP_LOGW(TAG, "摇杆没起来，跳过可视化");
        return;
    }

    /* 画布是 DISP_FB_W x DISP_FB_H = 256x192，纵向要精确排：
     * 方框 0~149，三行文字 153~187，正好卡在 192 以内。 */
    const int BOX = 150;
    const int BX  = (DISP_FB_W - BOX) / 2;
    const int BY  = 0;
    const int HC  = BX + BOX / 2, VC = BY + BOX / 2;
    const int R   = BOX / 2;

    printf("\n摇杆可视化：点跟着手走就说明映射对了。按大按键 A 退出。\n");
    while (gpio_get_level(PAD_PIN_A) == 0) vTaskDelay(pdMS_TO_TICKS(10));

    while (gpio_get_level(PAD_PIN_A) == 1) {
        int rx = read_axis(s_ch_x);
        int ry = read_axis(s_ch_y);

        /* 和 poll() 里同一套方向修正，保证屏上看到的就是游戏收到的 */
        int ex = rx - s_center_x, ey = ry - s_center_y;
        if (PAD_SWAP_XY)  { int t = ex; ex = ey; ey = t; }
        if (PAD_INVERT_X) ex = -ex;
        if (PAD_INVERT_Y) ey = -ey;

        display_clear(C_BLACK);
        display_rect(BX, BY, BOX, BOX, C_GRAY);
        display_hline(BX, VC, BOX, C_GRAY);
        display_vline(HC, BY, BOX, C_GRAY);

        int px = HC + ex * R / 2000;
        int py = VC - ey * R / 2000;
        if (px < BX)           px = BX;
        if (px > BX + BOX - 1) px = BX + BOX - 1;
        if (py < BY)           py = BY;
        if (py > BY + BOX - 1) py = BY + BOX - 1;
        display_fill_rect(px - 3, py - 3, 7, 7, C_GREEN);

        char line[48];
        snprintf(line, sizeof(line), "raw %4d %4d", rx, ry);
        display_text(4, BY + BOX + 3, line, C_WHITE, 1);
        snprintf(line, sizeof(line), "off %+5d %+5d", ex, ey);
        display_text(4, BY + BOX + 12, line, C_GRAY, 1);

        uint8_t d = 0;
        d |= axis_bits(ex, 0, false, NES_PAD_LEFT, NES_PAD_RIGHT, d);
        d |= axis_bits(ey, 0, false, NES_PAD_DOWN, NES_PAD_UP,    d);
        snprintf(line, sizeof(line), "%c%c%c%c  A exit",
                 (d & NES_PAD_UP)    ? 'U' : '-', (d & NES_PAD_DOWN)  ? 'D' : '-',
                 (d & NES_PAD_LEFT)  ? 'L' : '-', (d & NES_PAD_RIGHT) ? 'R' : '-');
        display_text(4, BY + BOX + 23, line, C_YELLOW, 2);

        display_flush();
    }
    while (gpio_get_level(PAD_PIN_A) == 0) vTaskDelay(pdMS_TO_TICKS(10));
    printf("退出摇杆可视化\n\n");
}

void input_gamepad_init(void)
{
    /* 幂等：开机选单和 nes_emu_run() 都会调。重复 adc_oneshot_new_unit()
     * 会返回 ESP_ERR_INVALID_STATE，摇杆就整个废了。 */
    static bool done;
    if (done) return;
    done = true;

    s_btn_ok  = buttons_init();
    s_axes_ok = axes_init();

    if (!s_btn_ok && !s_axes_ok) {
        printf("\nJoyStick Shield 没起来，只能用串口键盘\n\n");
        return;
    }

    printf("\nJoyStick Shield 已启用：\n");
    if (s_axes_ok) {
        printf("  方向  摇杆（GPIO%d/%d，中位 X=%d Y=%d）\n",
               PAD_PIN_X, PAD_PIN_Y, s_center_x, s_center_y);
    } else {
        printf("  方向  摇杆没起来（见上面的警告），十字键请用串口 WASD\n");
    }
    if (s_btn_ok) {
        printf("  A(跳) 大按键A       B(跑) 大按键B\n");
        printf("  START 大按键D       SELECT 大按键C\n");
    } else {
        printf("  按键没起来（见上面的警告），请用串口 K/J/回车/Tab\n");
    }
    printf("  方向不对就改 input_gamepad.h 里的 PAD_INVERT_X/Y、PAD_SWAP_XY\n\n");

#if PAD_DIAG_SCREEN
    input_gamepad_show();
#endif
}

uint8_t input_gamepad_poll(void)
{
    uint8_t state = 0;

    if (s_btn_ok) {
        for (unsigned i = 0; i < BUTTON_COUNT; i++) {
            if (gpio_get_level(BUTTONS[i].pin) == 0) {   /* 按下 = 低 */
                state |= BUTTONS[i].mask;
            }
        }
    }

    if (s_axes_ok) {
        int vx = read_axis(s_ch_x);
        int vy = read_axis(s_ch_y);
        int cx = s_center_x, cy = s_center_y;
        if (PAD_SWAP_XY) {          /* 常量条件，编译期就折叠掉了 */
            int t;
            t = vx; vx = vy; vy = t;
            t = cx; cx = cy; cy = t;
        }

        uint8_t dir = 0;
        dir |= axis_bits(vx, cx, PAD_INVERT_X, NES_PAD_LEFT, NES_PAD_RIGHT, s_dir);
        dir |= axis_bits(vy, cy, PAD_INVERT_Y, NES_PAD_DOWN, NES_PAD_UP,    s_dir);
        s_dir = dir;
        state |= dir;
    }

    if (state != s_last_state) {
        ESP_LOGI(TAG, "%c%c%c%c %s %s %s %s",
                 (state & NES_PAD_UP)     ? 'U' : '-',
                 (state & NES_PAD_DOWN)   ? 'D' : '-',
                 (state & NES_PAD_LEFT)   ? 'L' : '-',
                 (state & NES_PAD_RIGHT)  ? 'R' : '-',
                 (state & NES_PAD_A)      ? "A" : "-",
                 (state & NES_PAD_B)      ? "B" : "-",
                 (state & NES_PAD_SELECT) ? "SEL" : "---",
                 (state & NES_PAD_START)  ? "STA" : "---");
        s_last_state = state;
    }

    return state;
}
