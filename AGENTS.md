# AGENTS.md

本仓库给编码 agent 的说明。`CLAUDE.md` 只是 `@AGENTS.md` 一行引用，
所以**改这个文件就够了**，别把内容抄成两份。

ESP32-S3-DevKitC-1（N16R8）+ ST7789 SPI 屏上跑多平台模拟器。
ESP-IDF v5.4，纯 C，无测试套件 —— 验证靠烧板子 + 看串口输出。

代码注释和提交信息都用中文，重点写「为什么这么做」而不是「做了什么」。
延续这个风格：这个仓库的注释里存了大量硬件实测数据和踩坑结论，是主要的知识载体。

## 编译 / 烧录 / 验证

```bash
. ~/esp/esp-idf/export.sh                                  # 每个新终端都要跑一次
idf.py build
idf.py -p /dev/cu.usbserial-A5069RR4 flash monitor         # 退出 monitor: Ctrl+]
idf.py flash-roms                                          # 只在加/删顶层 roms/ 后跑
```

- 端口是板载 FT232R（丝印 `COM` 的 Type-C 口）。`A5069RR4` 是这颗芯片的序列号，
  换板子会变，用 `ls /dev/cu.usbserial-*` 确认。
- `flash-roms` 是顶层 `CMakeLists.txt` 注册的自定义 target，**故意不挂在 `idf.py flash` 上**：
  ROM 分区 13 MB，Deflate 镜像目前几 MiB（随游戏增删浮动），烧一次仍较久，而它几乎从不变。
  烧录时间只跟镜像实际字节数走（`esptool write_flash` 写的是文件），跟分区开多大无关。
- `sdkconfig` 不入库，由 `sdkconfig.defaults` 生成。要固化配置就改 `.defaults`，
  别改 `sdkconfig`（会被覆盖）。里面每一条都有理由（240 MHz CPU、1000 Hz tick、
  OCT PSRAM、`SPIRAM_MALLOC_RESERVE_INTERNAL=8192`）——改动前先读那些注释。

⚠️ **烧录和串口监视需要接着板子**，agent 通常做不到。改完代码把命令交给用户跑，
让他把串口输出贴回来，别假设烧录成功。

### 新克隆的副本会链接失败

`main/CMakeLists.txt` 的 `EMBED_FILES` 引用了 5 个版权 ROM（`main/roms/{smb,tetris,contra,pacman,drmario}.nes`），
它们在 `.gitignore` 里。自备文件放进去，或者把 `main/nes_emu.c` 顶部的 `ROM_CHOICE`
改成 5/6/7（随仓库分发的公有领域测试 ROM）并从 `EMBED_FILES` 删掉缺失的行。
顶层 `roms/`（给 `flash-roms` 用的本地游戏）同样不入库。

### 板上验证的诊断开关

改完相关代码优先打开对应开关，比盯代码快：

| 开关 | 位置 | 作用 |
|---|---|---|
| `DISP_PROFILE`（默认 1） | `main/display.c` | 每秒打一行核 1 推屏耗时。调 `BAND_LINES` / 画布尺寸时看这个 |
| `DIAG_TIMING` | `main/nes_emu.c` | 开机跑一遍分阶段计时（只 CPU / +PPU / 完整），定位核 0 瓶颈 |
| `SHOW_DISPLAY_SELFTEST` | `main/main.c` | 点屏诊断图，验旋转/颜色顺序/反色 |
| `OVERCLOCK_LEVEL`（默认 0，关闭） | `main/main.c` / `main/overclock.c` | 开机下发 BBPLL 微调寄存器把 CPU 推过 Kconfig 240MHz 上限，档位范围 `[-8, 8]`。没有标定 MHz——效果因片而异，实测主频打在串口 `overclock` tag 下，配合下面的 "CPU 余量" 自报行判断效果、单变量调档。本机实测：4 档能跑，6 档触发看门狗复位（`TG1WDT_SYS_RST`）不稳定，5 档没测过 |

开机画面（`main.c`）现在会停下来问 GAME/TEST：选 TEST 才会进摇杆位置 +
两路原始 ADC 值的诊断画面（`input_gamepad_show()`），不再是编译期开关。

运行时核 0 每秒自报 `NES 60 fps (模拟+转换 8.1 ms/帧，CPU 余量 52%)`。
两个核并行，帧时间是 `max(核 0, 核 1)`，所以两个数要分开看。

## 架构

启动链：`app_main`（main.c）→ `nes_emu_prealloc` → `display_init` → `rom_menu_pick` → 对应模拟器（不返回）。

### 双核分工与「条带流式推屏」

这是整个项目最要紧的一件事，改显示或性能相关代码前必须先理解：

- **没有常驻帧缓冲**。默认画布 288×224 的 RGB565 是 126 KB，双缓冲 252 KB，内部 DMA 内存装不下。
- `display.c` 只留两块最大 320×32 的条带缓冲（各 20 KB），核 1 上的 `blit_task` 一边把第 N+1 条
  转换进一块、一边 DMA 推第 N 条。整块画布由调用方的 `disp_strip_fn` 回调**按条带现算**，不落地。
- **双缓冲下移到了 8 位的 NES `vidbuf` 那一层**（每块 64 KB，比 RGB565 便宜一半）：
  核 0 渲染一块的同时核 1 读另一块。`blit_frame`（nes_emu.c）提交完立刻 `nes_setvidbuf` 换块。
- 默认接口是 `display_stream(fn, ctx)` / `display_stream_sync(...)`；Genesis 用
  `display_stream_sized(..., 320, 224)` 提交原生宽度画布。
  `display_stream` 只在上一帧未推完时阻塞，天然节流。

由此产生几条容易踩的约束：

- ⚠️ **绘图原语（`display_clear` / `display_text` / `display_fill_rect` …）只能在
  `disp_strip_fn` 回调内部调用** —— 它们画的是「当前条带」，坐标仍相对整块画布，
  条带外的部分自动丢弃。同一份默认画布绘制列表每帧被调用 7 次。
  所以「清屏」这种一次性操作也得包成回调（见 `nes_emu.c` 的 `black_strip`）。
- ⚠️ **条带缓冲不能放 PSRAM**：`spi_master` 用 `esp_ptr_dma_capable()` 判断，PSRAM 指针
  一律 false，驱动会每条带临时 malloc + memcpy，白绕一圈。同理 `vidbuf` 必须在内部 SRAM
  （放 PSRAM 时 PPU 渲染从 2 ms 涨到 8.5 ms）。
- ⚠️ **`nes_emu_prealloc()` 必须在 `display_init()` 之前调用**，否则凑不出两块 64 KB 连续内部内存。
- ⚠️ **帧缓冲里存的是字节交换后（大端）的 RGB565**。用 `display.h` 的 `RGB565()` 宏，
  直接往缓冲写像素的代码（调色板表）同样要存大端值。
- 条带越少越快：每条带约 122 µs 固定开销，串行叠加在总线时间上（实测数据见
  `display.c` 文件头和 `docs/hardware.md` §7）。

### 菜单画布 288×224，四款模拟器铺满 320×240 面板

`DISP_FB_W/H` 是画布，`DISP_W/H` 是面板，画布居中，四周黑边由 `display_init()`
开机清一次、之后再不碰。288 = 256×9/8，是为了修 NES 的 8:7 像素宽高比——这段历史
推导仍然成立，但现在只有 `rom_menu.c` 和诊断画面在用这块 288×224 画布。NES、SNES、
GB/GBC（共用 `gbc_emu.c`）、Genesis 都已经改走 `display_stream_sized()` 铺满整块
320×240 面板：早先「铺满 320 宽会把 SPI 带宽拖到 30 fps、且 NES 跳帧会让精灵永久
消失」的测算结论，上板实测已经证伪——铺满后都能稳定 60 fps，没有精灵丢失。
完整推导见 `display.h`。

### ROM 来源：分区 mmap，编译期嵌入做回退

- `roms/**/*.{nes,gb,gbc,sfc,smc,md,bin,zip}` → `tools/pack_roms.py` 打成自定义镜像
  （`.zip` 里恰好有一个可识别 ROM 时自动取出；0 个或多个都只打一行提示跳过。
  不认识的扩展名和 `.7z`/`.rar` 同样会打提示——以前是静默消失，最难查）
  （magic + 定长目录表 + 每个 ROM 独立 raw Deflate）
  → 烧进 `partitions.csv` 里 offset `0x210000` 的 13 MB `roms` 分区（子类型 0x40）。
  分区容量由 `pack_roms.py` 的 `roms_partition_size()` 现读 `partitions.csv`，
  改分区大小不用再同步脚本（以前写死 8 MB，扩容时忘了改会在打包这步炸）。
- `roms/` 按平台分子目录（`nes/` `gb/` `gbc/` `snes/` `md/`），**纯粹是给人看的**：
  平台由 ROM 头判定，不看目录名，放错目录也不影响结果。菜单顺序也和目录无关
  （排序键取文件名，不含路径）。想临时下架一个游戏就挪进 `removed-YYYYMMDD/`，
  `pack_roms.py` 会整棵跳过前缀为 `removed` / `_` / `.` 的目录。
- `rom_store.c` 一次 `esp_partition_mmap` 整个分区，菜单只读目录；确认后只把选中的
  Deflate ROM 解到 PSRAM。SNES 必须直接解到最终可写缓冲，不能先解一份再复制 4 MiB。
  **故意不用 SPIFFS/ZIP 文件系统**：自定义目录已经提供随机访问，套第二层目录只会重复。
- `rom_store_init()` 的校验写得很啰嗦是因为 offset/size 直接当指针用，而数据来自 flash
  （没烧过时全是 0xFF）。所有失败都只返回 0、不 abort，调用方回退到 `ROM_CHOICE` 选的嵌入 ROM。

### 输入

`input_serial_poll() | input_gamepad_poll()`，两路按位或并存，位掩码是 nofrendo 的
`NES_PAD_*`（`components/nofrendo/nes/input.h`）。串口键盘是手柄不灵时的调试后路。

- 摇杆报的是**状态**不是事件，所以菜单必须做边沿检测（`rom_menu.c`）。
- 串口那路只有「按下」没有「松开」，靠 `HOLD_MS=250` 超时松开 + 终端按键重复维持长按。
- `input_gamepad_init()` 开机采 16 次静止读数做中位校准 —— **上电时手别碰摇杆**，
  碰着会被判定「没接好」而整路禁用。

### nofrendo

`components/nofrendo/` 取自 [retro-go](https://github.com/ducalex/retro-go)，**源码未做修改**，
上游更新可以直接覆盖。**不要改这个目录下的文件** —— 有需求就加进 `rg_system.h` 那层垫片，
或者在 `main/` 里适配。唯一的粘合层是 `rg_system.h`（只提供日志 / CRC32 / `IRAM_ATTR` 三样）。
`RETRO_GO` 宏必须是 `PUBLIC`，否则 main 和组件对 `IRAM_ATTR` 的取值会打架。
`-O3` 是性能关键。

⚠️ Gwenesis 的目录 `LICENSE` 是 AGPL v3、源码头却写 GPL v3+，上游元数据不一致；
Snes9x 另有非商业分发限制。发布完整固件前必须先澄清并逐项满足组件许可。

## 改动时要同步的跨文件不变量

| 改这个 | 必须同步 |
|---|---|
| `DISP_FB_W` / `DISP_FB_H`（display.h） | `nes_emu.c` 的三个 `_Static_assert`；`display.c` 的 `BAND_LINES`（最好整除 `DISP_FB_H`） |
| `DISP_H` | `DISP_GAP_Y` 一起改。只改一个的症状是画面偏移 + 有一条边永远刷不到 |
| `partitions.csv` 的 roms offset | 顶层 `CMakeLists.txt` 的 `ROMS_OFFSET` |
| `pack_roms.py` 的 `NAME_LEN` | `rom_store.h` 的 `ROM_STORE_NAME_LEN` |
| 加 `main/roms/*.nes` | `main/CMakeLists.txt` 的 `EMBED_FILES` + `nes_emu.c` 的 `ROM_CHOICE` 分支和 `_binary_<名字>_nes_start` 符号名（非字母数字→下划线） |
| `main/` 新增 .c 或用新驱动 | `main/CMakeLists.txt` 的 `SRCS` 和 `REQUIRES`（显式写了 REQUIRES 就不再自动依赖全部组件） |

## 硬件排障的教训

`README.md` 里记着一次摇杆故障的完整排查（洞洞板上两轴之间几百欧的电阻性短路，
万用表通断档不响）。结论值得每次遇到硬件疑点时重读一遍：

**一次只改一个变量的对照实验（拔掉一根线看行为变不变），比任何隔着几层的推理都可靠。
硬件问题先做实验，再动代码。** 那次走的三条弯路全是同一类错误 —— 用间接推理代替直接实验，
其中两次还得出了和事实相反的结论。

选脚时永久避开：GPIO33~37（Octal PSRAM）、19/20（原生 USB）、0/3/45/46（strapping）。
屏的 SCK/MOSI/CS 必须是 SPI2 的 IOMUX 原生脚（12/11/10），换脚会降到 40 MHz。
摇杆两轴必须在 ADC1 范围（GPIO1~10），当前用 GPIO1/2；GPIO7/8
已给 Shield E/F 小键用 GPIO7/8 做 START/SELECT。

## 更多文档

- `README.md` —— 路线图、接线表、操作说明、调色板选择、排障记录
- `docs/hardware.md` —— 板卡细节、引脚、ST7789 三个坑、逐次优化的实测数据（§7）
- `docs/memory.md` —— Flash、内部 SRAM、PSRAM 的统一定义和各模拟器分配账
- 已知问题：屏没引出 TE 信号，推屏和面板扫描无法同步，画面剧变时有轻微撕裂
- 已知问题：只跑 SNES 时背光有轻微闪烁，且和画面忙不忙无关（一直都有）。换了
  供电来源后消失，判断是 SNES 单核跑 CPU 模拟+PPU+音频、持续功耗明显高于
  NES/GBC/Genesis，供电余量不够时电压轨被拉低连带背光一起暗。不是代码 bug，
  用电流更足的供电（真的 5V/1A+ 墙充或独立供电口，别用电脑弱口或劣质线）
