# esp32s3-gamebox

ESP32-S3-DevKitC-1 兼容板（N16R8）+ ST7789 SPI 屏（240×320，横屏 320×240）。

**目标**：在这块板上运行 NES、Game Boy 和 Game Boy Color 游戏。

硬件详情见 [`docs/hardware.md`](docs/hardware.md)；Flash、内部 SRAM、PSRAM 和三种
模拟器的完整分配账见 [`docs/memory.md`](docs/memory.md)。

## 路线图

- [x] 板级自检：16 MB Flash + 8 MB Octal PSRAM 实测通过
- [x] ST7789 显示层：条带流式推屏 + 核 1 推屏任务 + 基本绘图 → `main/display.c`
- [x] 帧率实测 → 详见 [`docs/hardware.md`](docs/hardware.md) §7
- [x] 移植 NES 模拟器核心（nofrendo）→ `components/nofrendo/`
- [x] 换 240×320 屏，画面 256×168 → 256×192（抽行从每 4 行丢 1 放宽到每 7 行丢 1）
- [x] 条带流式推屏，画面 256×192 → **288×224**：不再抽行，且第一次把
      NES 的 8:7 像素宽高比修对（误差 1.6%）
- [x] **实测锁定 60 fps**（核 0 模拟 8.1 ms/帧，CPU 余量 52%；核 1 推屏 14.0 ms/帧）
- [x] **《超级马里奥兄弟》标题画面正常运行**，颜色已校准
- [x] 移植 GB/GBC 模拟器核心（gnuboy）并接入同一套菜单、手柄、显示和音频
- [x] 串口键盘当手柄（临时方案）→ `main/input_serial.c`
- [x] JoyStick Shield 手柄 → `main/input_gamepad.c`（摇杆 + 四个面键 + SELECT/START）
- [x] USB HID 手柄 → `main/input_usb.c`（通用描述符解析，优先支持 Raspberry Pi/RetroPie 复古手柄）
- [x] MAX98357 I2S 游戏音频
- [~] 移植 SNES 模拟器核心（snes9x 2005）→ `components/snes9x/`。能跑但**达不到
      可玩帧率**：SMW 45/60 fps、Mario Kart 50/60 fps，大量热数据和渲染中间结果
      塞不进约 179 KB 的可用内部 SRAM。另外 Super FX / SA-1 / S-DD1 没有实现，
      这类卡带会黑屏。详见 [`components/snes9x/README.gamebox.md`](components/snes9x/README.gamebox.md)
- [x] SMW 即时存档：SELECT + START 长按 1 秒，RST 后启动同一 ROM 自动恢复；
      960 KiB FAT + wear levelling，双槽交替写和 CRC 防断电损坏
- [x] ROM 分区扩到 14 MB（35 款游戏，镜像 10.35 MB）

ROM 说明：商业 NES/GB/GBC/SNES ROM 都是版权物，**本仓库不包含**，需由使用者自备。
随仓库分发的三个 `.nes` 是 nofrendo 测试套件里的公有领域 homebrew，用于验证。

## 代码结构

| 文件 | 作用 |
|---|---|
| `main/main.c` | 启动流程：板级信息 → 初始化屏 → 启动模拟器 |
| `main/display.c` | ST7789 显示层。条带流式推屏 + 核 1 推屏任务，对上层只暴露「按条带填像素」 |
| `main/nes_emu.c` | 适配层。把 nofrendo 的 8 位调色板画面逐条带转成 RGB565 推屏 |
| `main/gbc_emu.c` | GB/GBC 适配层。把 160×144 大端 RGB565 等比放大到 240×216，并接入公共输入/音频 |
| `roms/` | 本地游戏库；`.nes/.gb/.gbc` 会被打成 mmap ROM 分区镜像，不入库 |
| `main/roms/` | 内置 ROM（公有领域测试 ROM） |
| `components/nofrendo/` | NES 模拟器核心，取自 [retro-go](https://github.com/ducalex/retro-go)，**未改动源码** |
| `components/gnuboy/` | GB/GBC 模拟器核心，取自 Retro-Go；宿主适配放在 `main/` |
| `components/nofrendo/rg_system.h` | 唯一的粘合层：nofrendo 只需要日志 / CRC32 / IRAM_ATTR 三样东西 |

### 画面怎么放

NES 输出 256×240，裁掉上下各 8 行 overscan 得 256×224。画布是 **288×224**：

- **竖向 224 行 1:1**，不抽行（以前受内部 RAM 限制要每 7 行丢 1 行）
- **横向 256 → 288**，每 8 个源像素输出 9 个

横向拉伸是为了修**像素宽高比**：NES 在 NTSC 电视上的 PAR 是 8:7 ≈ 1.143，像素不是
方的。256 原样显示（PAR 1.0）画面偏窄 12%。拉到 288 之后 PAR = 1.125，误差只剩
1.6%。选 9:8 是因为 NES 图块本来就是 8 像素宽且对齐，复制点正好落在图块边界上，
视觉最不突兀，内循环也能 8× 展开、无分支无取模。

**为什么不铺满 320×240**：不是内存不够，是 SPI 带宽。288×224 已占 60 fps 预算的
84%，铺满要 97%，只能掉到 30 fps 换 —— 而 NES 跳帧会让「靠逐帧轮换实现的精灵闪烁」
永久消失（超过 8 精灵/扫描线的那些会彻底看不见）。而且拉满 320 的 PAR 是 1.25，
偏宽 9%，宽高比反而更差。

画布**只覆盖这块 288×224 的画面区**，不是整屏（见 `display.h` 的 `DISP_FB_W/H`）。
黑边（左右各 16 列、上下各 8 行）由 `display_init()` 开机时清一次、之后再不碰。

GB/GBC 原生输出是 160×144 方形像素，在同一块 288×224 画布内按 3:2 等比放大为
240×216，左右各留 24 列、上下各留 4 行。两块源帧放 PSRAM；条带 DMA 缓冲仍在
内部 RAM，因此不会改变 NES 已验证的热内存布局。

### 显示层为什么没有帧缓冲

288×224 的 RGB565 是 126 KB，双缓冲要 252 KB，内部 DMA 内存装不下。所以
`display.c` 只留两块 288×32 的条带缓冲（各 18 KB），核 1 一边把第 N+1 条转换进一块、
一边 DMA 推第 N 条。整块画布由调用方的 `disp_strip_fn` 回调按条带现算，不落地。

**双缓冲没有消失，只是下移到了 8 位的 NES vidbuf 那一层**（每块 64 KB，比 RGB565
便宜一半）：核 0 渲染一块的同时核 1 读另一块，帧时间仍是 `max(模拟, 推屏)` 而不是
两者相加。合计内存 163 KB，比原来的 256 KB 省 92 KB，分辨率还涨了。

⚠️ 条带缓冲**不能放 PSRAM**：`spi_master` 用 `esp_ptr_dma_capable()` 判断能否直接
DMA，那个函数只认内部 DRAM 地址段，PSRAM 指针一律返回 false，驱动会每条带临时
malloc 一块内部缓冲再 memcpy —— 数据最后照样落在内部 RAM，白绕一圈还多一次拷贝。

⚠️ 绘图原语（`display_text` 等）只能在 `disp_strip_fn` 回调**内部**调用 —— 它们画的
是「当前条带」，坐标仍是相对整块画布的，落在条带外的部分自动丢弃。同一段绘制代码
每帧会被调用 7 次，各画出一横条。菜单/诊断画面这类低帧率场景开销可忽略。

### 已知问题：轻微撕裂

这块屏没引出 TE 信号，推屏和面板扫描无法同步，画面剧烈变化时会看到撕裂。
画布升到 288×224 之后推屏窗口从 10.4 ms 涨到 14.0 ms，撕裂窗口相应长了 35% ——
这是那次改动唯一的退步。详见 [`docs/hardware.md`](docs/hardware.md) §7。

### 排障记录：摇杆两轴读数恒等（已解决）

**症状**：推任何方向，X/Y 两路 ADC 读数总是同向变化，从不独立。
连采 100 帧量化下来，两路**相关系数 r = +0.9990**，同向分量 `(X+Y)/2` 跨度 4072
（满量程），正交分量 `(X-Y)/2` 只有 128（3%，噪声级别）——输入端只剩一个自由度。

**根因**：洞洞板上两根轴线之间有**电阻性短路**，几百欧到几 kΩ。两个 10kΩ 的
电位器被它死死拉在一起，各自被对方钳住，都读成中间值。重新焊接后恢复正常。

⚠️ **为什么万用表没查出来**：通断档一般只在几十欧以下才响，而这个短路在
几百欧到几 kΩ 量级——**不响，但足以毁掉两个 10k 源的独立性**。查这类耦合要打
**电阻档读具体数值**，别信通断档的蜂鸣。助焊剂残留是这个阻值区间的常见元凶，
而且会随吸潮恶化，符合「一开始正常、用着用着坏」。

**最终定位靠的是一个对照实验**，而不是任何间接推理：一次只插一根轴线，
看连着的那一路响应哪个方向。

| 只插 | 满量程出现在 | 说明 |
|---|---|---|
| X | 推左右（上下不动） | X 电位器正常，单轴干净 |
| Y | 推上下 | Y 电位器也正常 |

两根单独都好、一起就废 → 问题必然在两根之间的通路上。

⚠️ **走过的弯路**（值得记住，都是同一类错误——用间接推理代替直接实验）：

1. 用「连读两次间隔 1 ms，读数一致」排除悬空引脚 —— 推论不成立，
   悬空脚的 S/H 电荷能维持远超 1 ms
2. 用「先读恒 4095 的悬空脚给 S/H 电容下毒，再读目标」判定两路都被真实低阻源
   驱动 —— 结论也是错的，和后来的对照实验直接矛盾
3. 「两轴不独立」的特征从第一份数据起就在，却先后加了 45° 旋转、两点标定去拟合它

**教训：一次只改一个变量的对照实验（拔掉一根线看行为变不变），
比任何隔着几层的推理都可靠。硬件问题先做实验，再动代码。**

### ⚠️ 克隆后先补 ROM

`main/roms/smb.nes` 是版权物，**不在仓库里**（见 `.gitignore`）。
新克隆的副本缺这个文件会**链接失败**（`main/CMakeLists.txt` 的 `EMBED_FILES` 引用了它）。

自备一份《超级马里奥兄弟》的 `.nes`，放成 `main/roms/smb.nes` 即可。
不想弄的话把 `main/nes_emu.c` 顶部的 `ROM_CHOICE` 改成 `1`/`2`/`3`，
并从 `EMBED_FILES` 里删掉 `roms/smb.nes` 那行 —— 剩下三个公有领域测试 ROM 随仓库分发。

### 换 ROM

日常游戏库放在顶层 `roms/`：支持 `.nes`、`.gb`、`.gbc`，运行 `idf.py build` 会生成
`build/roms.bin`，再用 `idf.py flash-roms` 单独烧入。开机菜单会显示 `NES/GB/GBC`
类型；换游戏仍按板子 RST 重启，避免在不同模拟器之间留下运行时状态。

选单里按下方面键（SNES B / Shield C）可切换声音开关，标题右侧会
显示当前状态。开关只对本次开机有效；按 RST 换游戏或重新上电后总会恢复为开启。

`main/roms/` 只用于 ROM 分区不可用时的 NES 编译期回退。只有要更换这个回退游戏时，
才需要同步修改 `main/CMakeLists.txt` 的 `EMBED_FILES` 和 `main/nes_emu.c` 的
`ROM_CHOICE`。

### 操作（手柄）

JoyStick Shield，接线见「接线」一节。飞线手柄、USB 手柄、串口键盘三路输入
**并存**（按位或），串口留着当调试后路。

| 物理位置 | Shield 丝印 | SNES | NES / GB / GBC |
|---|---|---|---|
| 上方大键 | A | X | 无 |
| 左方大键 | D | Y | 无 |
| 右方大键 | B | A | A |
| 下方大键 | C | B | B（选单中切换声音） |
| 左侧小键 | F | SELECT | SELECT |
| 右侧小键 | E | START | START |
| 摇杆 | — | 上下左右 | 上下左右 |

SNES 的 Super Mario World 里，同时长按小按键 F（SELECT）和 E（START）1 秒会冻结
画面并显示 `SAVING...`，看到 `SAVE OK` 后即可安全按 RST。下次重新选择同一份 SMW ROM
会自动回到保存瞬间。存档与 ROM CRC 绑定，不会加载到同名改版或其他区域版本；目前
只有 SMW 启用此功能，其他游戏不会占用这份全局即时存档。需要恢复双槽中的上一份时，
在菜单进入 SMW 时按住上方大键 X，再按 A 或 START 确认；上一份不存在或损坏时仍会退回
最新一份，不会清空或覆盖任何存档。

上电时**手别碰摇杆**：开机要采 16 次静止读数做中位校准，碰着会被判定为
「没接好」而整路禁用（按键不受影响）。

### 操作（串口键盘）

真手柄到货前的临时方案，现在仍作为调试后路保留。
`idf.py monitor` 会把你敲的字符经串口发给板子。

```bash
idf.py -p /dev/cu.usbserial-A5069RR4 monitor
```

**焦点要在 monitor 窗口里**，然后：

| 键 | 作用 |
|---|---|
| `W` `A` `S` `D` 或方向键 | 上下左右 |
| `K` 或 `Z` | A（跳） |
| `J` 或 `X` | 选单：声音开关；游戏：B（跑 / 发射） |
| 回车 | START |
| Tab | SELECT |
| 空格 | 全部松开（按键卡住时用） |
| `Ctrl+]` | 退出 monitor |

还可以接有线 USB HID 手柄，三路输入会按位合并，不需要在菜单中切换。使用丝印
`USB` 的 Type-C 口并短接板背面的 `USB-OTG` 焊盘；丝印 `COM` 的 Type-C 口继续
负责供电、烧录和串口。默认按通用 RetroPie/SNES 编号映射：Button 1/3 → B，
Button 2/4 → A，Button 9 → SELECT，Button 10 → START。首次接入会在串口打印
VID/PID、解析结果和前 40 次原始报告，便于识别换芯片的手柄批次。
开机摇杆诊断页也接受 USB 手柄 A 或串口 `K/Z` 退出，不要求保留飞线手柄。

**限制**：串口只有「按下」没有「松开」事件，所以每次按键让按钮保持 250ms
（`input_serial.c` 的 `HOLD_MS`），长按靠终端的按键重复维持。因此：

- 长按开头有个停顿（终端的重复延迟），马里奥会先走一步再连续跑
- 点不出「轻跳」，每次跳跃都是固定时长

把系统的「按键重复速度」调到最快、「重复前延迟」调到最短会好很多。
接上真手柄后这些问题都不存在。

### 调色板

NES 的颜色本质是 NTSC 相位信号，没有唯一正确的 RGB 值，各家解码出来色相差别不小。
nofrendo 内置 6 套，改 `main/nes_emu.c` 顶部的 `NES_PALETTE` 即可切换，无性能影响。

当前用 `NES_PALETTE_NESCLASSIC`（任天堂 NES Classic Edition 的官方调色板）——
拿马里奥标题画面的 4 个关键色跟参考画面比对，它的总色差最小。
默认的 `SMOOTH` 在天空色 `$22` 上是 6 套里唯一 G>R 的，会让天空偏青蓝而不是紫蓝。

另外还有个 `NES_SATURATION`（百分比，100 = 原样，当前 150）。
这 6 套调色板的红色 `$16`（马里奥的帽子衣服）**都偏暗** —— R 分量只有常见
FCEUX 调色板 `(216,40,0)` 的 67%~74%，在小屏上看着发褐，换哪套都一样。
所以围绕亮度拉开饱和度：灰阶和白色不受影响，只有带颜色的像素变鲜艳。

| $16 | 饱和度 |
|---|---|
| (146, 52, 4) 发褐 | 100% |
| (182, 41, 0) | **150%（当前）** |
| (203, 35, 0) 接近 FCEUX 观感 | 180% |

只在开机建表时算一次，运行时零开销。

### 性能诊断

两个核各有各的计时，因为它们是并行的，帧时间取两者的较大值：

- **核 0（模拟）**：运行时每秒往串口打一行 `NES 60 fps (模拟+转换 8.1 ms/帧…)`。
  想拆得更细就把 `main/nes_emu.c` 顶部的 `DIAG_TIMING` 改成 `1`，开机会先跑一遍
  分阶段计时（只跑 CPU / +PPU 渲染 / 完整）。
  GB/GBC 路径对应输出 `GB 59.7 fps` 或 `GBC 59.7 fps`。
- **核 1（转换+推屏）**：`main/display.c` 顶部的 `DISP_PROFILE`（默认 `1`）每秒打一行
  `推屏 14.03 ms/帧（32 行/条 x 7 条…）`。调条带尺寸或画布尺寸时直接看这个数。
  嫌吵改成 `0`。

条带尺寸（`display.c` 的 `BAND_LINES`）对推屏时间影响很大：每条带有约 122 µs 的
固定开销，**条带越少越快**。实测数据见 [`docs/hardware.md`](docs/hardware.md) §7。

## 接线

### ST7789 屏

| 屏丝印 | 开发板 |
|---|---|
| GND | GND |
| VCC | 3V3 |
| SCL / SCK | GPIO12 |
| SDA / MOSI | GPIO11 |
| RES | GPIO13 |
| DC | GPIO14 |
| CS | GPIO10 |
| BLK | GPIO9（或直接 3V3 常亮） |

改引脚改 `main/display.h` 顶部的 `DISP_PIN_*`。SCK/MOSI/CS 这三个是 SPI2 的
IOMUX 原生脚，换成别的会降到 40 MHz，别动。

### JoyStick Shield V1.A（手柄）

Arduino UNO 的扩展板，**插不到 DevKitC 上**，只能飞线。走板子中央那个 6×2 的
黄色排针 —— 底边 Arduino 排母的丝印被排母本体挡住，数针位容易错一位。
黄排针旁边就印着针位表：

```
上排:  V  A  C  E  K  X
下排:  G  B  D  F  3  Y
```

| 黄排针 | 开发板 | 用途 |
|---|---|---|
| G | GND | |
| 3 | **3V3** | ⚠️ 不是 `V`，`V` 是 5V |
| X | GPIO1 | 摇杆 X（ADC1_CH0） |
| Y | GPIO2 | 摇杆 Y（ADC1_CH1） |
| A | GPIO15 | 上方大键 → SNES X |
| B | GPIO16 | 右方大键 → SNES A |
| C | GPIO17 | 下方大键 → SNES B |
| D | GPIO18 | 左方大键 → SNES Y |
| F | GPIO8 | 左侧小键 → SELECT |
| E | GPIO7 | 右侧小键 → START |

`V`、`K` 空着（K 是摇杆按下）。

⚠️ **板子左边的 3V3/5V 拨动开关必须拨到 3V3**。摇杆是两个电位器跨在 VCC 和
GND 之间，拨 5V 档输出就是 5V，而 ESP32-S3 的 GPIO 不是 5V 耐受的。

摇杆两轴必须落在 GPIO1~10（ADC1 的范围），当前用 GPIO1/2；GPIO7/8
已分给 F/E 小键。
改引脚改 `main/input_gamepad.h` 顶部的 `PAD_PIN_*`。

开机会先进摇杆自检画面：除了摇杆光点和 ADC 数值，下方还会显示
`X/Y/A/B/SELECT/START`，按下时变绿。同时按 SNES A+B（Shield B+C，右+下）
继续进选单。
不想要就把 `main/input_gamepad.h` 的 `PAD_DIAG_SCREEN` 改成 0。

**接线时断电**，插好再上电。

## 编译烧录

```bash
. ~/esp/esp-idf/export.sh          # 每个新终端都要执行一次
idf.py build
idf.py -p /dev/cu.usbserial-A5069RR4 flash monitor
```

烧录用丝印 `COM` 的 Type-C 口（板载 FTDI FT232R 桥）。本机上枚举为
`/dev/cu.usbserial-A5069RR4`（A5069RR4 是这颗 FT232R 的序列号，换板子会变，
用 `ls /dev/cu.usbserial-*` 确认）。退出串口监视器：`Ctrl+]`。

`COM` 口没枚举出设备就换另一个 Type-C 口（丝印 `USB`，原生 USB），
**按住 BOOT 再上电**进下载模式，端口名形如 `/dev/cu.usbmodem*`。

## 换屏 / 显示不正常

`main/display.c` 对上层只暴露「按条带填 RGB565 像素」，换屏只需改
`main/display.h` 顶部的宏，上层代码不动。调的时候**一次只改一个**再烧：

| 现象 | 改哪个 |
|---|---|
| 画面偏移 / 边缘花条 | `DISP_GAP_X` / `DISP_GAP_Y`（满屏 240×320 用 0；170 列的窄屏用 35） |
| 上下或左右颠倒 | `DISP_MIRROR_X` / `DISP_MIRROR_Y` |
| 颜色像底片 | `DISP_INVERT_COLOR` |
| 红蓝互换 | `DISP_BGR_ORDER` |
| 花屏 / 雪花 | `DISP_SPI_HZ` 降到 40 MHz 或更低 |
| 分辨率变了 | `DISP_W` / `DISP_H`，以及画布 `DISP_FB_W` / `DISP_FB_H` |

⚠️ 换屏时 `DISP_GAP_Y` 和 `DISP_H` 要一起改。只改一个的典型症状是：画面能正常显示，
但整体偏移、并且屏幕有一条边永远刷不到 —— 看着像「边缘被浪费了」。

⚠️ 改 `DISP_FB_W/H` 时记得同步 `nes_emu.c` 里的 `_Static_assert`（它会挡住宽高和
NES 画面对不上的组合），以及 `display.c` 的 `BAND_LINES`（最好能整除 `DISP_FB_H`，
除不尽虽然能跑，但会多出一条短带、多花约 122 µs）。

点屏诊断图（`main.c` 的 `SHOW_DISPLAY_SELFTEST` 改成 `1`）画的是**画布**而不是面板，
所以白框贴的是画布边界，外面还有黑边。要验 `DISP_GAP_*` 得先把 `DISP_FB_W/H` 临时
改成 `DISP_W`/`DISP_H`。

## 授权

`components/nofrendo/` 取自 [retro-go](https://github.com/ducalex/retro-go)，
源自 Matthew Conte 的 Nofrendo，**GPL v2**
（见 `components/nofrendo/COPYING` 和 `CREDITS`，源码未做修改）。

`components/gnuboy/` 同样取自 Retro-Go，基线提交记录在该目录的
`README.gamebox.md`，许可文本见 `components/gnuboy/COPYING`。

`components/snes9x/` 也取自 Retro-Go，但**授权和上面两个不同**：
`components/snes9x/src/LICENSE` 是 Snes9x 自己的许可证，不是 GPL，
**明确禁止商业分发**。

由于链接了 GPL 代码，**整个固件在分发时受 GPL v2 约束**。自己玩没影响，
但如果要公开发布二进制或仓库，需要一并提供源码。而 snes9x 那条又叠加了
禁止商业用途 —— 两者取交集，实际上这个固件只能非商业地分发。

`main/` 下的代码是本项目自己写的。ROM 文件的版权归各自权利人所有。
