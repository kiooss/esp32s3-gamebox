/*
 * 串口键盘当手柄
 *
 * ---- 为什么要「保持时间」这个机制 ----
 *
 * 串口传过来的只有字符，也就是只有「按下」事件，**没有「松开」事件**。
 * 终端不会告诉你键什么时候抬起来。
 *
 * 所以这里的做法是：收到一个键，就让对应按钮保持 HOLD_MS 毫秒再自动松开。
 * 长按靠终端自己的**按键重复**来维持 —— 你按住 D 不放，终端会先停顿一下
 * （macOS 默认 ~500ms），然后开始以 ~30ms 的间隔连续发 'd'，
 * 每一个都会把保持时间续上，于是按钮一直是按下的。
 *
 * 这带来两个已知的手感问题，是方案本身的限制，不是 bug：
 *   1. 长按开头有个停顿（终端的重复延迟），马里奥会先走一步再连续跑
 *   2. 点不出「轻跳」—— 每次跳跃都是固定 HOLD_MS 的按键时长
 *
 * 想改善就去系统设置里把「按键重复速度」调到最快、「重复前延迟」调到最短。
 * 真手柄接上 GPIO 之后这些问题都不存在。
 */

#include <string.h>
#include "input_serial.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nofrendo.h"
#include "input_gamepad.h"   /* SNES 才有的 X/Y 位 */

static const char *TAG = "input";

/* 一次按键保持多久。
 * 要大于终端的按键重复间隔（~30ms）才能长按连贯，
 * 又不能太大否则松手后还在动。250ms 下跳跃接近满高度。 */
#define HOLD_MS     250

#define RX_BUF      512

/* 位数按 GAMEPAD_BIT_Y(0x200) 取到 10 —— 低 8 位和 NES_PAD_* 一致，
 * 高两位是 SNES 的 X/Y，串口调试也要覆盖完整四面键。 */
#define PAD_BITS    10

static int64_t  s_until[PAD_BITS];  /* 每个按键位的保持截止时间（微秒） */
static uint16_t s_last_state;       /* 上一次的状态，用来只在变化时打日志 */
static int     s_esc;           /* 方向键转义序列的解析状态 */

static void press(uint16_t mask)
{
    int64_t t = esp_timer_get_time() + (int64_t)HOLD_MS * 1000;
    for (int b = 0; b < PAD_BITS; b++) {
        if (mask & (1 << b)) s_until[b] = t;
    }
}

static void release_all(void)
{
    memset(s_until, 0, sizeof(s_until));
}

static void feed(uint8_t c)
{
    /* 方向键是 ESC [ A/B/C/D 三字节转义序列 */
    if (s_esc == 0 && c == 0x1B) { s_esc = 1; return; }
    if (s_esc == 1) { s_esc = (c == '[') ? 2 : 0; return; }
    if (s_esc == 2) {
        s_esc = 0;
        switch (c) {
        case 'A': press(NES_PAD_UP);    return;
        case 'B': press(NES_PAD_DOWN);  return;
        case 'C': press(NES_PAD_RIGHT); return;
        case 'D': press(NES_PAD_LEFT);  return;
        default:  return;
        }
    }

    switch (c) {
    case 'w': case 'W': press(NES_PAD_UP);     break;
    case 's': case 'S': press(NES_PAD_DOWN);   break;
    case 'a': case 'A': press(NES_PAD_LEFT);   break;
    case 'd': case 'D': press(NES_PAD_RIGHT);  break;

    case 'k': case 'K': case 'z': case 'Z': press(NES_PAD_A); break;  /* 跳 */
    case 'j': case 'J': case 'x': case 'X': press(NES_PAD_B); break;  /* 跑/发射 */

    /* SNES 专有的两颗。借 u/i 是因为它们正好在 j/k 上一排，和 B/A 同一个手位；
     * 'x' 已经被 B 占了，不能拿来当 SNES 的 X。NES/GB 会忽略这两位。 */
    case 'u': case 'U': press(GAMEPAD_BIT_X); break;
    case 'i': case 'I': press(GAMEPAD_BIT_Y); break;

    case '\r': case '\n': press(NES_PAD_START);  break;
    case '\t':            press(NES_PAD_SELECT); break;

    case ' ':  release_all(); break;   /* 卡住了按空格全部松开 */
    default:   break;
    }
}

void input_serial_init(void)
{
    /* 幂等：开机选单和 nes_emu_run() 都会调这个函数。没有这道保护的话
     * 第二次 uart_driver_install() 返回 ESP_ERR_INVALID_STATE，
     * 被 ESP_ERROR_CHECK 变成 abort —— 板子起不来。 */
    static bool done;
    if (done) return;
    done = true;

    /* 只装驱动，不动波特率和引脚 —— bootloader 已经把 UART0 配好了。
     * printf 走的是另一条（轮询 TX FIFO）路径，和这个驱动共存没问题。 */
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, RX_BUF, 0, 0, NULL, 0));

    printf("\n串口手柄已启用：\n");
    printf("  方向  W A S D  或 方向键\n");
    printf("  A(跳) K 或 Z      B(跑) J 或 X\n");
    printf("  SNES X U          SNES Y I\n");
    printf("  START 回车        SELECT Tab\n");
    printf("  游戏菜单 SELECT+SNES X（Tab+U）\n");
    printf("  按键卡住了敲空格全部松开\n");
    printf("  注意：串口没有「松手」事件，按住不放靠终端的按键重复维持\n\n");
}

void input_serial_discard(void)
{
    /* 启动等待时积压的字符不能变成首页出现后的新操作。转义序列和保持状态
     * 也必须一起清掉，否则残留方向键前缀会吞掉玩家就绪后第一次按键。 */
    uart_flush_input(UART_NUM_0);
    release_all();
    s_esc = 0;
    s_last_state = 0;
}

uint16_t input_serial_poll(void)
{
    uint8_t buf[32];
    int n = uart_read_bytes(UART_NUM_0, buf, sizeof(buf), 0);   /* 0 = 不阻塞 */
    for (int i = 0; i < n; i++) {
        feed(buf[i]);
    }

    int64_t now = esp_timer_get_time();
    uint16_t state = 0;
    for (int b = 0; b < PAD_BITS; b++) {
        if (s_until[b] > now) state |= (1 << b);
    }

    if (state != s_last_state) {
        ESP_LOGI(TAG, "%c%c%c%c %s %s %s %s %s %s",
                 (state & NES_PAD_UP)     ? 'U' : '-',
                 (state & NES_PAD_DOWN)   ? 'D' : '-',
                 (state & NES_PAD_LEFT)   ? 'L' : '-',
                 (state & NES_PAD_RIGHT)  ? 'R' : '-',
                 (state & NES_PAD_A)      ? "A" : "-",
                 (state & NES_PAD_B)      ? "B" : "-",
                 (state & NES_PAD_START)  ? "START"  : "-",
                 (state & NES_PAD_SELECT) ? "SELECT" : "-",
                 (state & GAMEPAD_BIT_X)  ? "X" : "-",
                 (state & GAMEPAD_BIT_Y)  ? "Y" : "-");
        s_last_state = state;
    }
    return state;
}
