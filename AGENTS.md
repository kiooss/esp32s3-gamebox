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
idf.py flash-word-audio                                   # 只在教材词表/发音参数变化后跑
```

- 端口是板载 FT232R（丝印 `COM` 的 Type-C 口）。`A5069RR4` 是这颗芯片的序列号，
  换板子会变，用 `ls /dev/cu.usbserial-*` 确认。
- `flash-word-audio` 是顶层 `CMakeLists.txt` 注册的自定义 target，**故意不挂在 `idf.py flash` 上**：
  英式发音包约 3.62 MiB，烧一次仍较久，而教材词表几乎不变。
  烧录时间只跟镜像实际字节数走（`esptool write_flash` 写的是文件），跟分区开多大无关。
- `sdkconfig` 不入库，由 `sdkconfig.defaults` 生成。要固化配置就改 `.defaults`，
  别改 `sdkconfig`（会被覆盖）。
  ⚠️ **改完 `.defaults` 必须 `rm sdkconfig` 再 build**：IDF 只在 `sdkconfig`
  不存在时读 defaults，已有的 `sdkconfig` 会赢。踩过一次——加了长文件名配置，
  build 通过、烧进去一查压根没生效。里面每一条都有理由（240 MHz CPU、1000 Hz tick、
  OCT PSRAM、`SPIRAM_MALLOC_RESERVE_INTERNAL=8192`）——改动前先读那些注释。

⚠️ **烧录和串口监视需要接着板子**，agent 通常做不到。改完代码把命令交给用户跑，
让他把串口输出贴回来，别假设烧录成功。

### 新克隆的副本会链接失败

`main/CMakeLists.txt` 的 `EMBED_FILES` 引用了 5 个版权 ROM（`main/roms/{smb,tetris,contra,pacman,drmario}.nes`），
它们在 `.gitignore` 里。自备文件放进去，或者把 `main/nes_emu.c` 顶部的 `ROM_CHOICE`
改成 5/6/7（随仓库分发的公有领域测试 ROM）并从 `EMBED_FILES` 删掉缺失的行。
顶层 `roms/` 是旧打包工具留下的本地游戏目录，不入库；运行时游戏只从 TF 卡读。

### 板上验证的诊断开关

改完相关代码优先打开对应开关，比盯代码快：

| 开关 | 位置 | 作用 |
|---|---|---|
| `DISP_PROFILE`（默认 1） | `main/display.c` | 每秒打一行核 1 推屏耗时。调 `BAND_LINES` / 画布尺寸时看这个 |
| `DIAG_TIMING` | `main/nes_emu.c` | 开机跑一遍分阶段计时（只 CPU / +PPU / 完整），定位核 0 瓶颈 |
| `SHOW_DISPLAY_SELFTEST` | `main/main.c` | 点屏诊断图，验旋转/颜色顺序/反色 |
| `SD_SELFTEST`（默认 0） | `main/main.c` | TF 卡自检：列根目录 + 写读校验，只走串口。**换卡或改接线时打开**，慢卡上要多花两秒 |
| `SD_BENCHMARK`（默认 0） | `main/sd_card.c` | 扇区读基准，量这张卡的命令固定开销和吞吐。判读方法和本机实测值见那个函数的注释 |
| `SCAN_PROFILE`（默认 0） | `main/rom_store.c` | 扫描分阶段计时（readdir / stat），定位扫描慢在哪 |
| `OVERCLOCK_LEVEL`（**默认 4，已开启**） | `main/main.c` / `main/overclock.c` | 开机下发 BBPLL 微调寄存器把 CPU 推过 Kconfig 240MHz 上限，档位范围 `[-8, 8]`。没有标定 MHz——效果因片而异，实测主频打在串口 `overclock` tag 下，配合下面的 "CPU 余量" 自报行判断效果、单变量调档。本机实测：4 档能跑，6 档触发看门狗复位（`TG1WDT_SYS_RST`）不稳定，5 档没测过。2026-08-28 为 PC Engine 把默认值从 0 改成 4：实画帧 12~18 → 20~28，其余四个核心一并受益，且一个字节内存都不花。想回出厂主频把这个宏改回 0 |

开机画面（`main.c`）现在会停下来问 GAME/WORDS/SETTINGS：WORDS 进入苏州小学译林版
三至六年级上/下册选择，再选择 Unit 1～8；每册进度按教材版本独立保存；SETTINGS
沿用 retro-go Options 的单列交互；游戏以外的界面统一走 gruvbox light，可调音量
和背光；音量使用以 50% 为锚点的听感曲线，每次调整立即播放 `hello` 试听；也可进入
摇杆位置 + 两路原始 ADC
值的 Controller Test（`input_gamepad_show()`）。

运行时核 0 每秒自报 `NES 60 fps (模拟+转换 8.1 ms/帧，CPU 余量 52%)`。
两个核并行，帧时间是 `max(核 0, 核 1)`，所以两个数要分开看。

## 架构

启动链：`app_main`（main.c）→ `nes_emu_prealloc` → 挂卡 → `display_init` →
启动等待页（声音、三路输入、游戏目录、收藏）→ 入场动画 → GAME/WORDS/SETTINGS。
首页必须在这些准备工作全部完成或确定回退后才出现；不能把读目录/收藏推迟到
第一次进入 GAME 或 Controller Test。SELECT 重扫键在读目录前采样，进入首页前
丢弃等待期间的串口按键；没有 TF 卡仍可进入 WORDS/SETTINGS，GAME 使用内置 ROM。
WORDS 在 `word_study_run()` 内可反复换年级、册次和单元；三上按教材 Word lists
完整收录 127 项（各单元 13/11/17/9/18/12/31/16），其余分册暂为每单元 8 个核心词；
卡片正面自动播放英式发音，X 可重播，QUIZ 判题时答对/答错分别播放上升/下降
8-bit 掌机音效，顶部对应进度格同步变绿/红，本单元全部答对时在结果页播放专属
过关短曲；返回后会释放 24 kHz I2S，
再回到开机模式选择；
GAME 再进 `rom_menu_pick`；平台页是 2×3 卡片网格，游戏列表每页 8 项并把编号做成
独立徽标；游戏列表 B 返回平台页，平台页 B 返回开机模式选择；音量、背光和
Controller Test 集中在 SETTINGS，选定游戏后进入对应模拟器（不返回）。

平台页 SELECT 进入跨平台“我的收藏”，游戏列表 X 收藏/取消，右侧 `★` 表示已收藏；
收藏复用每页 8 项的列表，B 返回平台页，不占用六个平台卡片的位置。
`rom_favorites.c` 以完整 ROM/ZIP 外层路径为键，在 TF 根目录 `.gamebox-favorites`
单独保存（临时文件 + `.bak` 备份 + 完整性校验），不能把收藏绑在易变的目录序号上，
也不能因收藏读写重扫卡或重写 ROM 索引。落盘成功后才更新星标；只展示当前目录中
存在的收藏，但保留暂时缺失路径的记录。返回首页继续复用已读收藏，启动模拟器前
才释放收藏占用的 PSRAM，避免反复进入 GAME 又等待读卡。

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

### 菜单画布 288×224，五款模拟器铺满 320×240 面板

`DISP_FB_W/H` 是画布，`DISP_W/H` 是面板，画布居中，四周黑边由 `display_init()`
开机清一次、之后再不碰。288 = 256×9/8，是为了修 NES 的 8:7 像素宽高比——这段历史
推导仍然成立，但现在只有 `rom_menu.c` 和诊断画面在用这块 288×224 画布。NES、SNES、
GB/GBC（共用 `gbc_emu.c`）、Genesis 都已经改走 `display_stream_sized()` 铺满整块
320×240 面板：早先「铺满 320 宽会把 SPI 带宽拖到 30 fps、且 NES 跳帧会让精灵永久
消失」的测算结论，上板实测已经证伪——铺满后都能稳定 60 fps，没有精灵丢失。
完整推导见 `display.h`。

### ROM 来源：TF 卡，编译期嵌入做回退

**游戏全部从 TF 卡读**。原 13 MB `roms` 分区已经改为 `word_audio`，放
`tools/build_word_audio.py` 生成的 Daniel（en_GB）离线发音包；`tools/pack_roms.py`
只作为旧格式工具保留，不再接入构建。

- 卡上随便怎么摆。`rom_store.c` 从 `/sd` 递归扫描（最多 4 层），认这些扩展名：
  `.nes .gb .gbc .sfc .smc .md .bin .pce .sgx .zip`。前缀是 `.` / `_` / `removed` 的目录整棵跳过，
  `System Volume Information` 也跳过。RAR / 7z 等其他压缩格式仍需先在电脑上解开。
- 裸 ROM 的平台不看目录，只按扩展名定。ZIP 为保证开机速度，路径含
  `nes/gb/gbc/snes/md/pce` 时不 `stat`、不 `open`、不读内容，直接归进对应平台，
  选中后才解析、取第一个支持的 ROM。**路径推断不出平台的 ZIP 才在扫描期读一次
  中央目录**认平台（`add_zip_by_content`），认不出来的直接不收录。以前这种 ZIP
  会落进一个叫「ZIP」的分类占一格平台卡片，那不是平台、是「还没认出来」，
  已经去掉。支持普通单卷 ZIP 的
  store(0) / deflate(8)，不支持加密、ZIP64 和多卷。仓库约定目录仍是
  `/sd/roms/{nes,gb,gbc,snes,md,pce}/`。
- ⚠️ **扫描裸 ROM 时一个文件都不打开**，平台只按扩展名定、大小只靠 `stat()`。这是实测
  逼出来的：SD 命令的固定开销远大于传输字节数（本机那张 2 GB 老卡每条命令要
  40 ms），原来每个文件都 open+读头+seek，39 个游戏扫 14 秒；改成纯 readdir+stat
  之后 2.8 秒；ZIP 连 stat 也跳过后，旧版扫到 256 项就停时两次实测 2.39～5.52 秒。
  取消人为数量上限后，同卡完整收录 971 项、5 个平台实测 30.83 秒；增加的时间是
  遍历剩余目录，不是读取 ZIP 内容。
  **ROM 头照样验，只是挪到了选中后的装载阶段。**
  代价：扩展名骗人的文件（尤其通用的 `.bin`）会进到菜单，选中时才报错。
- ROM 目录没有固定条目上限。`rom_store.c` 在 PSRAM 中按 128、256、512... 动态扩容，
  扫描结束再缩到实数；只有真实 PSRAM 分配失败才停止。名字和路径逐项稳定分配，
  不能换回会在 `realloc` 后令现有 entry 指针失效的单块可移动字符串池。
- 完整扫描后会在卡根目录原子更新 `.gamebox-rom-index`（格式版本 + payload CRC，
  临时文件和 `.bak` 兜底）。后续启动顺序读缓存，缓存无效自动回退扫描；加删游戏后
  开机按住 SELECT 强制刷新。不要为“自动检测变化”重新遍历目录，那会把 30 秒启动
  延迟原样带回来；也不要直接落盘含指针/`size_t` 的 `rom_store_entry_t`。
- GB / GBC 按扩展名分**只影响菜单分组**：`gbc_emu.c` 根本不读 `entry->system`，
  gnuboy 自己从 ROM 头 0x143 判 CGB/SGB/DMG（`gnuboy.c:234`）。
- ⚠️ **`rom_store_load()` 必须经内部 RAM 反弹缓冲，并用 POSIX `open/read`**，不能让
  stdio `fread` 或 FatFs 直接写 PSRAM。
  `sdmmc_read_sectors()` 发现目标不是 DMA-capable 就退化成一次一个 512 字节扇区
  读 + memcpy（IDF 的 `sdmmc_cmd.c`）。本机单扇区读 72 ms —— 4 MiB 卡带这样读要
  十分钟。即使有内部中转，`fread` 仍会把读请求拆碎：同一个 1 MiB ROM 实测
  84 秒 / 12 KB/s；换 `read` 后 16 KB 中转为 5.6 秒。中转缓冲按 64→32→16→8→4 KB
  梯度回退。SNES 特意在 119 KB 内部帧缓冲之前装 ROM；NES 则借用尚未给 PPU 使用的
  约 63 KB vidbuf。ZIP 解压同样复用这块 DMA RAM：`1943.zip` 的 256 KB ROM 从
  16 KB 块的 16.87 秒降到 63 KB 块的 7.25 秒。装完再交还，不和运行期内存争用。
- SNES 的 512 字节拷贝机头只从文件大小就能判出来（整卡带都是 `0x400` 的整数倍），
  不用开文件。`rom_store_load(entry, extra_bytes, ...)` 仍然直接读进最终可写缓冲，
  不能先读一份再复制 4 MiB。
- `loading_screen.c` 在菜单选中后接管默认 288×224 画布。`rom_store.c` 的同步回调
  按实际字节数推进裸 ROM / ZIP，界面每 5% 才重画一次；不要改成每个 64 KB 块都
  无条件推屏，4 MiB ROM 会凭空多刷 64 帧并拖慢加载。
- 所有失败都只让 `rom_store_init()` 返回 0、不 abort：没插卡、卡挂不上、卡上没有
  合法 ROM，都回退到 `nes_emu.c` 里 `ROM_CHOICE` 选的编译期嵌入 ROM。
  ⚠️ 目前**没卡时屏幕上没有任何提示**，直接进内置游戏，看起来像坏了。

### 非游戏 UI 的配色

游戏以外的所有界面（开机画面、模式选择、ROM 菜单、加载页、SETTINGS、
单词学习、Controller Test）只从 `main/display.h` 取色，那里是**唯一入口**，
分两层：

- **原色层 `C_GVB_*`** —— [gruvbox](https://github.com/morhetz/gruvbox) light
  主题的完整调色板，名字和上游 palette 一一对应。gruvbox 的 `faded_*` 和
  `neutral_*` 不是深浅备选，是给两个主题用的：light 主题取 `faded_*`。
- **语义层 `C_UI_*` / `C_SYS_*`** —— 面（BG/PANEL/PANEL_ALT/LINE/EDGE）、
  字（FG/FG_DIM/FG_FAINT/FG_INV/FG_INV_DIM）、选中态（SEL/SEL_EDGE）、
  状态（OK/BAD/WARN/INFO/GOLD/BAR）、六个平台各自的色相（`C_SYS_NES` 等，
  splash 的平台标签和 rom_menu 平台卡片的色条共用）。

⚠ **画界面只用语义层**，别直接摸 `C_GVB_*`，更不要自己 `RGB565()` 一个新色。
以前 `main.c` / `rom_menu.c` 各抄了一份配色约定注释、`word_study.c` 又自己
`#define` 了三个色，就是没有这层的结果。`C_RED`/`C_GREEN` 那组原色只留给
`SHOW_DISPLAY_SELFTEST` 诊断图和 `gbc_emu.c` 的存档提示——那两处要的是
「一眼认出通道对不对」，不该跟着主题走。

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

⚠️ **本仓库只分发源码，不分发构建产物（`.bin`），这是有意的许可决定。**
`main/` 是 GPL v2（根 `LICENSE`），五个核心各自保持上游许可。链接出来的固件
二进制有两处不可回避的不兼容：Snes9x 的「仅限非商业」附加限制与 GPL 冲突
（注意方向 —— Snes9x 本身允许非商业分发，是 GPL 不许附加限制）；nofrendo 是
GPLv2-**only**（源文件无 or later）而 gwenesis 是 GPL v3+/AGPL v3，两者不能合并。
（pce-go 是 GPL v2，和 nofrendo/gnuboy 同阵营，不改变这个结论。）
所以**不要往仓库里提交 `.bin`/`.elf`，也不要加发布二进制的 CI**。
完整推导和三处上游元数据矛盾见 `README.md` 的「授权」一节。

## 改动时要同步的跨文件不变量

| 改这个 | 必须同步 |
|---|---|
| `DISP_FB_W` / `DISP_FB_H`（display.h） | `nes_emu.c` 的三个 `_Static_assert`；`display.c` 的 `BAND_LINES`（最好整除 `DISP_FB_H`） |
| `DISP_H` | `DISP_GAP_Y` 一起改。只改一个的症状是画面偏移 + 有一条边永远刷不到 |
| `partitions.csv` 的 word_audio offset | 顶层 `CMakeLists.txt` 的 `WORD_AUDIO_OFFSET` |
| `pack_roms.py` 的 `NAME_LEN` | `rom_store.h` 的 `ROM_STORE_NAME_LEN` |
| 加 `main/roms/*.nes` | `main/CMakeLists.txt` 的 `EMBED_FILES` + `nes_emu.c` 的 `ROM_CHOICE` 分支和 `_binary_<名字>_nes_start` 符号名（非字母数字→下划线） |
| 菜单要显示新汉字 | 不用管了：`main/menu_font.c` 收了 Unifont 的 95 个 8x16 ASCII 和 GB2312 全部 6763 个 16x16 汉字。真撞上 GB2312 之外的字（繁体、假名）才需要往 `tools/gen_menu_font.py` 的 `EXTRA_TEXT` 加一个字并重跑（要 unifont 源文件，不入库） |
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
TF 卡走 SPI3（GPIO39/41/40/42 = CLK/MOSI/MISO/CS），**不能挂 SPI2** —— 那条总线被
ST7789 的条带流式推屏独占，见 `docs/hardware.md` §10。剩下完全自由的只有 21/38/47，
加模拟量输入则只剩 GPIO3（ADC1 里唯一空位）。

## 更多文档

- `README.md` —— 路线图、接线表、操作说明、调色板选择、排障记录
- `docs/hardware.md` —— 板卡细节、引脚、ST7789 三个坑、逐次优化的实测数据（§7）
- `docs/memory.md` —— Flash、内部 SRAM、PSRAM 的统一定义和各模拟器分配账
- 已知问题：屏没引出 TE 信号，推屏和面板扫描无法同步，画面剧变时有轻微撕裂
- 已知问题：只跑 SNES 时背光有轻微闪烁，且和画面忙不忙无关（一直都有）。换了
  供电来源后消失，判断是 SNES 单核跑 CPU 模拟+PPU+音频、持续功耗明显高于
  NES/GBC/Genesis，供电余量不够时电压轨被拉低连带背光一起暗。不是代码 bug，
  用电流更足的供电（真的 5V/1A+ 墙充或独立供电口，别用电脑弱口或劣质线）
