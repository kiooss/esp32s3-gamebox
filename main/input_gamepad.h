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
 * 黄色排针是公针，DevKitC 也是公针 —— 用**母对母**杜邦线，一共 10 根：
 *
 *   黄排针  ESP32-S3   说明
 *   ------  ---------  --------------------------------
 *   G       GND        先接这根
 *   3       3V3        摇杆电位器的供电（不是 V！V 是 5V 档）
 *   X       GPIO1      摇杆 X（= Arduino A0），ADC1_CH0
 *   Y       GPIO2      摇杆 Y（= Arduino A1），ADC1_CH1
 *   A       GPIO15     上方大按键 -> SNES X
 *   B       GPIO16     右方大按键 -> SNES A
 *   C       GPIO17     下方大按键 -> SNES B
 *   D       GPIO18     左方大按键 -> SNES Y
 *   F       GPIO8      左侧小按键 -> SELECT
 *   E       GPIO7      右侧小按键 -> START
 *
 * V、K 两针空着（K 是摇杆按下）。
 *
 * 插之前先确认丝印的左右方向：排针本体会挡住正下方一截丝印，而 X/V 在同一端、
 * Y/G 在另一端，认反就是把 3V3 和 GND 接到摇杆信号上。万用表通断档、黑笔搭
 * ESP32 的 GND 去戳你认定的 G 针，响了就对（板上 GND 是大片铜）。不用上电。
 *
 * 选脚理由：
 *   - X/Y 必须落在 GPIO1~10 —— 那是 ESP32-S3 的 ADC1 通道范围。取 GPIO1/2。
 *     （不用 ADC2：它跟 WiFi 冲突，虽然本项目不开 WiFi，但没必要给以后埋雷。）
 *   - 15/16/17/18 在 DevKitC-1 上是连续四个脚，接四个大按键；
 *     7/8 是剩余且无冲突的普通 GPIO，接 F/E 两个小键。
 *   - 全部避开了 Octal PSRAM 的 33~37、原生 USB 的 19/20、strapping 的 0/3/45/46。
 *
 * 5110 排针和 nRF24 排针（黄色那排）用不上，空着。
 * K 键仍空着，以后做复位或菜单键。
 */
#pragma once

#include <stdint.h>

/* 宿主输入的公共位。低 8 位故意与 NES_PAD_* 一致，NES/GB/GBC
 * 只取这部分；高位补上 SNES 才有的 X/Y，避免为了 SNES 破坏旧输入语义。 */
enum {
    GAMEPAD_BIT_A      = 0x01,
    GAMEPAD_BIT_B      = 0x02,
    GAMEPAD_BIT_SELECT = 0x04,
    GAMEPAD_BIT_START  = 0x08,
    GAMEPAD_BIT_UP     = 0x10,
    GAMEPAD_BIT_DOWN   = 0x20,
    GAMEPAD_BIT_LEFT   = 0x40,
    GAMEPAD_BIT_RIGHT  = 0x80,
    GAMEPAD_BIT_X      = 0x100,
    GAMEPAD_BIT_Y      = 0x200,
};
#include <stdbool.h>

/* ============ 接线（改这里就能换脚） ============ */
#define PAD_PIN_SHIELD_A 15     /* 上 -> SNES X */
#define PAD_PIN_SHIELD_B 16     /* 右 -> SNES A */
#define PAD_PIN_SHIELD_C 17     /* 下 -> SNES B */
#define PAD_PIN_SHIELD_D 18     /* 左 -> SNES Y */
#define PAD_PIN_SELECT   8      /* Shield F，左侧小按键；实物接线确认 */
#define PAD_PIN_START    7      /* Shield E，右侧小按键；实物接线确认 */

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
 * 开机时把摇杆位置和六个按键状态实时画到屏上，同时按
 * SNES A+B（Shield B+C，右+下）退出，然后照常进选单。
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

/* 装 GPIO + ADC。必须在开始模拟之前调用一次。
 * 会顺便采一次摇杆的静止位置做中位校准 —— 所以**上电时手别碰摇杆**。
 * 按键和摇杆分别初始化，一边失败不影响另一边（串口日志里会说是哪边）。 */
void input_gamepad_init(void);

/* 每帧调一次，返回 NES 手柄位掩码（NES_PAD_* 的组合）。
 * 没调 init()、或者两路都没起来时恒返回 0，可以安全地和别的输入源按位或。 */
uint16_t input_gamepad_poll(void);

/* 摇杆/按键可视化，同时按 SNES A+B（Shield B+C）退出。
 * 需要 display_init() 已经跑过。开机画面选 TEST 时 main.c 会调它；
 * 不再是 input_gamepad_init() 自动触发的（那样每次开机都得看一遍，
 * 想跳过看不了）。 */
void input_gamepad_show(void);
