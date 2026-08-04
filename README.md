# esp32s3-app2

ESP32-S3-DevKitC-1 兼容板（N16R8）+ 1.9" ST7789 SPI 屏（170×320）。

**目标**：在这块板上跑 NES 模拟器（Super Mario Bros）。

硬件详情见 [`docs/hardware.md`](docs/hardware.md)。

## 路线图

- [x] 板级自检：16 MB Flash + 8 MB Octal PSRAM 实测通过
- [x] ST7789 显示层：双缓冲 + 核 1 推屏任务 + 基本绘图 → `main/display.c`
- [x] 帧率实测：**满屏 86 fps**（11.6 ms/帧，画图仅占 2.0 ms，CPU 余量 83%）
      → 60 fps 有富余，详见 [`docs/hardware.md`](docs/hardware.md) §7
- [x] 移植 NES 模拟器核心（nofrendo）→ `components/nofrendo/`
- [x] 256×224 → 256×168 的 3/4 缩放输出 → `main/nes_emu.c`
- [x] **实测锁定 60 fps**（测试 ROM 8.6 ms/帧、马里奥 11.3 ms/帧，CPU 余量 33%）
- [x] **《超级马里奥兄弟》标题画面正常运行**，颜色已校准
- [x] 串口键盘当手柄（临时方案）→ `main/input_serial.c`
- [ ] 真手柄：8 个按键接 GPIO（手柄未到）
- [ ] 音频（I2S DAC + 喇叭，可选）

ROM 说明：《超级马里奥兄弟》的 ROM 是任天堂版权物，**本仓库不包含**，需自备
（见下方「克隆后先补 ROM」）。随仓库分发的三个是 nofrendo 测试套件里的
公有领域 homebrew，用于验证。

## 代码结构

| 文件 | 作用 |
|---|---|
| `main/main.c` | 启动流程：板级信息 → 初始化屏 → 启动模拟器 |
| `main/display.c` | ST7789 显示层。双缓冲 + 核 1 推屏任务，对上层只暴露「帧缓冲 + 推屏」 |
| `main/nes_emu.c` | 适配层。把 nofrendo 的 8 位调色板画面转成 RGB565 画进帧缓冲 |
| `main/roms/` | 内置 ROM（公有领域测试 ROM） |
| `components/nofrendo/` | NES 模拟器核心，取自 [retro-go](https://github.com/ducalex/retro-go)，**未改动源码** |
| `components/nofrendo/rg_system.h` | 唯一的粘合层：nofrendo 只需要日志 / CRC32 / IRAM_ATTR 三样东西 |

### 画面怎么放

NES 输出 256×240，裁掉上下各 8 行 overscan 得 256×224。屏幕横屏是 320×170，
高度放不下，所以竖着按 3/4 缩：**每 4 行丢 1 行**，224 × 3/4 = 168，正好塞进 170。
整数运算不用插值，几乎不花 CPU。横向 256 不缩（320 装得下），两侧各留 32 像素黑边，
像素保持 1:1 最锐利。

帧缓冲**只覆盖这块 256×168 的画面区**，不是整屏（见 `display.h` 的 `DISP_FB_W/H`）。
黑边由 `display_init()` 开机时清一次、之后再不碰。省 44 KB 内存、每帧少推 21% 数据。

### 已知问题：轻微撕裂

这块屏没引出 TE 信号，推屏和面板扫描无法同步，画面剧烈变化时会看到撕裂。
详见 [`docs/hardware.md`](docs/hardware.md) §7。

### ⚠️ 克隆后先补 ROM

`main/roms/smb.nes` 是版权物，**不在仓库里**（见 `.gitignore`）。
新克隆的副本缺这个文件会**链接失败**（`main/CMakeLists.txt` 的 `EMBED_FILES` 引用了它）。

自备一份《超级马里奥兄弟》的 `.nes`，放成 `main/roms/smb.nes` 即可。
不想弄的话把 `main/nes_emu.c` 顶部的 `ROM_CHOICE` 改成 `1`/`2`/`3`，
并从 `EMBED_FILES` 里删掉 `roms/smb.nes` 那行 —— 剩下三个公有领域测试 ROM 随仓库分发。

### 换 ROM

把 `.nes` 文件放进 `main/roms/`，加到 `main/CMakeLists.txt` 的 `EMBED_FILES`，
再改 `main/nes_emu.c` 顶部的 `ROM_CHOICE` 和对应的 `asm("_binary_..._nes_start")` 符号名
（符号名由文件名生成：`foo-bar.nes` → `_binary_foo_bar_nes_start`）。

### 操作（串口键盘）

真手柄到货前的临时方案。`idf.py monitor` 会把你敲的字符经串口发给板子。

```bash
idf.py -p /dev/cu.usbserial-A5069RR4 monitor
```

**焦点要在 monitor 窗口里**，然后：

| 键 | 作用 |
|---|---|
| `W` `A` `S` `D` 或方向键 | 上下左右 |
| `K` 或 `Z` | A（跳） |
| `J` 或 `X` | B（跑 / 发射） |
| 回车 | START |
| Tab | SELECT |
| 空格 | 全部松开（按键卡住时用） |
| `Ctrl+]` | 退出 monitor |

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

`main/nes_emu.c` 顶部把 `DIAG_TIMING` 改成 `1`，开机会先跑一遍分阶段计时，
把每帧耗时拆成 CPU 模拟 / PPU 渲染 / 调色板转换 / 推屏四段。换硬件或调优时很有用。

## 接线

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

**接线时断电**，插好再上电。改引脚改 `main/display.h` 顶部的 `DISP_PIN_*`。

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

`main/display.c` 对上层只暴露「一块 RGB565 帧缓冲 + 推屏」，换屏只需改
`main/display.h` 顶部的宏，上层代码不动。调的时候**一次只改一个**再烧：

| 现象 | 改哪个 |
|---|---|
| 画面偏移 / 边缘花条 | `DISP_GAP_X` / `DISP_GAP_Y`（试 35 与 0 互换） |
| 上下或左右颠倒 | `DISP_MIRROR_X` / `DISP_MIRROR_Y` |
| 颜色像底片 | `DISP_INVERT_COLOR` |
| 红蓝互换 | `DISP_BGR_ORDER` |
| 花屏 / 雪花 | `DISP_SPI_HZ` 降到 40 MHz 或更低 |
| 分辨率变了 | `DISP_W` / `DISP_H` |

## 授权

`components/nofrendo/` 取自 [retro-go](https://github.com/ducalex/retro-go)，
源自 Matthew Conte 的 Nofrendo，**GPL v2**
（见 `components/nofrendo/COPYING` 和 `CREDITS`，源码未做修改）。

由于链接了 GPL 代码，**整个固件在分发时受 GPL v2 约束**。自己玩没影响，
但如果要公开发布二进制或仓库，需要一并提供源码。

`main/` 下的代码是本项目自己写的。ROM 文件的版权归各自权利人所有。
