/*
 * JoyStick Shield V1.A 当手柄
 *
 * 这块板本来是 Arduino UNO 的扩展板（叠在 UNO 上用），跟 ESP32-S3 DevKitC 的
 * 排针对不上，**插不上去**，只能用杜邦线把信号引出来。
 *
 * ⚠ 板子左边那个 3V3/5V 拨动开关必须拨到 **3V3**。
 *   摇杆是两个电位器直接跨在 VCC 和 GND 之间，拨在 5V 档输出就是 5V，
 *   而 ESP32-S3 的 GPIO 不是 5V 耐受的 —— 会打坏 ADC 脚。
 *
 * ---- 接线 ----
 *
 * **走板子中央那个 6x2 的黄色公排针**，不用碰底边的 Arduino 排母 —— 所有信号
 * 它都引出来了，而且旁边就印着针位表（两行丝印对应排针两排）：
 *
 *       上排:  V  A  C  E  K  X
 *       下排:  G  B  D  F  3  Y
 *
 * 底边 Arduino 排母的丝印被排母本体挡住，肉眼数针位容易错一位，别用。
 * 黄色排针是公针，DevKitC 也是公针 —— 用**母对母**杜邦线，一共 8 根：
 *
 *   黄排针  ESP32-S3   说明
 *   ------  ---------  --------------------------------
 *   G       GND        先接这根
 *   3       3V3        摇杆电位器的供电（不是 V！V 是 5V 档）
 *   X       GPIO1      摇杆 X（= Arduino A0），ADC1_CH0
 *   Y       GPIO2      摇杆 Y（= Arduino A1），ADC1_CH1
 *   A       GPIO15     大按键 A -> NES A（跳）
 *   B       GPIO16     大按键 B -> NES B（跑）
 *   C       GPIO17     大按键 C -> SELECT
 *   D       GPIO18     大按键 D -> START
 *
 * V、E、F、K 四针空着（E/F 是两个小键，K 是摇杆按下，以后做存档/复位用）。
 *
 * 插之前先确认丝印的左右方向：排针本体会挡住正下方一截丝印，而 X/V 在同一端、
 * Y/G 在另一端，认反就是把 3V3 和 GND 接到摇杆信号上。万用表通断档、黑笔搭
 * ESP32 的 GND 去戳你认定的 G 针，响了就对（板上 GND 是大片铜）。不用上电。
 *
 * 选脚理由：
 *   - X/Y 必须落在 GPIO1~10 —— 那是 ESP32-S3 的 ADC1 通道范围。9~14 被屏占了，
 *     只剩 1/2/6/7/8，取头两个。（不用 ADC2：它跟 WiFi 冲突，虽然本项目不开
 *     WiFi，但没必要给以后埋雷。）
 *   - 15/16/17/18 在 DevKitC-1 上是连续四个脚，接线顺手。
 *   - 全部避开了 Octal PSRAM 的 33~37、原生 USB 的 19/20、strapping 的 0/3/45/46。
 *
 * 5110 排针和 nRF24 排针（黄色那排）用不上，空着。
 * E、F、K 三个键也空着，以后做存档/读档/复位正好。
 */
#pragma once

#include <stdint.h>

/* 两种宿主输入都返回这套公共位。数值故意与 NES_PAD_* 一致，所以 NES 路径
 * 可以零转换直传；GB/GBC 适配层再显式映射到 gnuboy 的不同位序。 */
enum {
    GAMEPAD_BIT_A      = 0x01,
    GAMEPAD_BIT_B      = 0x02,
    GAMEPAD_BIT_SELECT = 0x04,
    GAMEPAD_BIT_START  = 0x08,
    GAMEPAD_BIT_UP     = 0x10,
    GAMEPAD_BIT_DOWN   = 0x20,
    GAMEPAD_BIT_LEFT   = 0x40,
    GAMEPAD_BIT_RIGHT  = 0x80,
};
#include <stdbool.h>

/* ============ 接线（改这里就能换脚） ============ */
#define PAD_PIN_A       15      /* Shield D2，大按键 A -> NES A */
#define PAD_PIN_B       16      /* Shield D3，大按键 B -> NES B */
#define PAD_PIN_SELECT  17      /* Shield D4，大按键 C */
#define PAD_PIN_START   18      /* Shield D5，大按键 D */

/* 摇杆两轴。必须是 ADC1 的通道，也就是 GPIO1~10。 */
#define PAD_PIN_X       1
#define PAD_PIN_Y       2

/* ============ 摇杆方向 ============
 *
 * 摇杆焊的方向、以及你握持的角度，决定了哪个轴是左右、推哪边是正。
 * 实际方向不对就改这三个宏（每次只改一个，看串口日志里的 U D L R 确认）：
 *
 *   左右反了      -> 翻转 PAD_INVERT_X
 *   上下反了      -> 翻转 PAD_INVERT_Y
 *   推左右却动上下 -> 翻转 PAD_SWAP_XY
 */
#define PAD_INVERT_X    false
#define PAD_INVERT_Y    false   /* 实测：这块板往上推是电压变大，不用翻 */
#define PAD_SWAP_XY     false

/* ============ 摇杆可视化 ============
 *
 * 开机时把摇杆位置实时画到屏上，按大按键 A 退出，然后照常进选单。
 * 默认开着 —— 它同时也是开机自检，一眼就能确认手柄接好了没有。
 * 嫌开机多一步就改成 0。
 *
 * 画的就是 poll() 用的同一套坐标，所以「点跟着手走」= 映射正确：
 * 推右点往右、推上点往上。屏下方同时打出两路原始 ADC 读数 ——
 * **只推一个方向时应该只有一个数字大幅变化**，两个一起变就是两路耦合。
 *
 * 最后这条判据实测有用：2026-08-09 排查摇杆失灵时，就是靠它发现两路读数
 * 恒等（相关系数 0.999），最终定位到洞洞板上两根轴线之间的电阻性短路 ——
 * 几百欧到几 kΩ，万用表通断档不响，但足以把两个 10k 电位器拉在一起。 */
#define PAD_DIAG_SCREEN  1

/* 装 GPIO + ADC。必须在开始模拟之前调用一次。
 * 会顺便采一次摇杆的静止位置做中位校准 —— 所以**上电时手别碰摇杆**。
 * 按键和摇杆分别初始化，一边失败不影响另一边（串口日志里会说是哪边）。 */
void input_gamepad_init(void);

/* 每帧调一次，返回 NES 手柄位掩码（NES_PAD_* 的组合）。
 * 没调 init()、或者两路都没起来时恒返回 0，可以安全地和别的输入源按位或。 */
uint8_t input_gamepad_poll(void);

/* 摇杆位置可视化，按大按键 A 退出。需要 display_init() 已经跑过。
 * PAD_DIAG_SCREEN=1 时 input_gamepad_init() 末尾会自动调它。 */
void input_gamepad_show(void);
