# esp32s3-gamebox

ESP32-S3-DevKitC-1 兼容板（N16R8）+ ST7789 SPI 屏（240×320，横屏 320×240）。

**目标**：在这块板上运行 NES、Game Boy、Game Boy Color、SNES 和 Genesis 游戏，
并提供不依赖网络的儿童英语学习模式。

硬件详情见 [`docs/hardware.md`](docs/hardware.md)；Flash、内部 SRAM、PSRAM 和各条
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
- [~] USB HID 手柄 → `main/input_usb.c`（软件已接入；当前开发板 Root Port Reset 失败，
      实机验收暂停，见 [`docs/usb-hid-investigation-2026-08-14.md`](docs/usb-hid-investigation-2026-08-14.md)）
- [x] MAX98357 I2S 游戏音频
- [x] 移植 Genesis / Mega Drive 模拟器核心（retro-go Gwenesis）：按 retro-go 的
      单核逐扫描线顺序运行 68000、Z80、VDP、YM2612 和 PSG，固定每 4 帧提交一张
      画面。Sonic 实测轻场景约 58–59 fps，重场景约 38–49 fps，显示约 9–15 fps；
      画面校验持续变化，I2S 无丢帧或写错。重场景因音频生成追不上实时播放会
      间歇性断音；尝试逐扫描线声音和 `-Ofast` 后更慢，已恢复 cycle-accurate `-O2`
- [~] 移植 SNES 模拟器核心（snes9x 2005）→ `components/snes9x/`。能跑但**达不到
      可玩帧率**：SMW 45/60 fps、Mario Kart 50/60 fps，大量热数据和渲染中间结果
      塞不进约 179 KB 的可用内部 SRAM。另外 Super FX / SA-1 / S-DD1 没有实现，
      这类卡带会黑屏。详见 [`components/snes9x/README.gamebox.md`](components/snes9x/README.gamebox.md)
- [x] retro-go 风格四槽即时存档：NES、GB/GBC、SNES、Genesis 游戏中同时按
      SELECT + X 打开统一菜单，可 Save & Continue、Save & Quit、Load game、
      软/硬 Reset 或 Quit；状态按平台和 ROM CRC 写入 TF 卡，临时文件 + 备份改名
      避免写卡中断直接覆盖唯一好档
- [x] 苏州小学译林版单词学习：三至六年级按上/下册选择；三上按教材 Word lists
      完整收录 127 项（包含带 * 的非核心词），其余分册暂为每单元 8 个核心词；
      可选单元进行认读和测验，每册用独立 NVS 键保存掌握进度
- [x] WORDS 离线英式发音：Daniel（en_GB）首次自动读、X 重播；524 个不重复
      词条压成 3.62 MiB IMA ADPCM，语音包缺失时学习功能仍可用
- [x] WORDS QUIZ 即时声音反馈：答对播放四级上升 8-bit 琶音，答错播放三级
      下降掌机提示音；本单元全部答对时在结果页播放专属过关短曲
- [x] WORDS QUIZ 顶部逐题反馈：答对格变绿、答错格变红，未答题保持原高亮

ROM 说明：商业 NES/GB/GBC/SNES/Genesis ROM 都是版权物，**本仓库不包含**，需由使用者自备。
随仓库分发的三个 `.nes` 是 nofrendo 测试套件里的公有领域 homebrew，用于验证。

## 代码结构

| 文件 | 作用 |
|---|---|
| `main/main.c` | 启动流程：板级信息 → 初始化屏 → GAME / WORDS / SETTINGS 分流 |
| `main/word_study.c` | 教材/单元选择、单词认读/三选一测验与分册 NVS 进度 |
| `main/word_study_data.c` | 译林版三至六年级上下册词表；三上为教材完整表，其余暂为核心词 |
| `main/word_audio.c` | 读取独立分区中的英式发音索引，流式解码 IMA ADPCM 并异步播放 |
| `tools/build_word_audio.py` | 用 macOS Daniel 语音生成 24 kHz 英式发音包 |
| `main/display.c` | ST7789 显示层。条带流式推屏 + 核 1 推屏任务，对上层只暴露「按条带填像素」 |
| `main/nes_emu.c` | 适配层。把 nofrendo 的 8 位调色板画面逐条带转成 RGB565 推屏 |
| `main/gbc_emu.c` | GB/GBC 适配层。把 160×144 大端 RGB565 等比放大到 240×216，并接入公共输入/音频 |
| `main/genesis_emu.c` | retro-go Gwenesis 单核宿主层。320×241 索引帧以原生 320×224 送屏，并混合 YM2612/PSG 音频 |
| TF 卡 `/roms/` | 本地游戏库；支持 `.nes/.gb/.gbc/.sfc/.smc/.md/.bin/.zip`，不入库 |
| `main/roms/` | 内置 ROM（公有领域测试 ROM） |
| `components/nofrendo/` | NES 模拟器核心，取自 [retro-go](https://github.com/ducalex/retro-go)，**未改动源码** |
| `components/gnuboy/` | GB/GBC 模拟器核心，取自 Retro-Go；宿主适配放在 `main/` |
| `components/gwenesis/` | Genesis 核心，取自 retro-go 的 Gwenesis 组件；Gamebox 只适配构建和 M68K RAM 分配时机 |
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

NES、SNES、菜单的默认画布**只覆盖这块 288×224 的画面区**，不是整屏（见
`display.h` 的 `DISP_FB_W/H`）。黑边（左右各 16 列、上下各 8 行）由
`display_init()` 开机时清一次、之后再不碰。Genesis 则通过 `display_stream_sized()`
单独使用 **320×224**：H40 模式逐像素原生显示，H32 模式 256 像素内容居中，均不做横向缩放。

GB/GBC 原生输出是 160×144 方形像素，在同一块 288×224 画布内按 3:2 等比放大为
240×216，左右各留 24 列、上下各留 4 行。两块源帧放 PSRAM；条带 DMA 缓冲仍在
内部 RAM，因此不会改变 NES 已验证的热内存布局。

### 显示层为什么没有帧缓冲

288×224 的 RGB565 是 126 KB，双缓冲要 252 KB，内部 DMA 内存装不下。所以
`display.c` 只留两块最大 320×32 的条带缓冲（各 20 KB），核 1 一边把第 N+1 条转换进一块、
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

### 排障记录：TF 卡「写入报成功但什么都没落盘」（已解决）

**症状**：板子这边 SD 卡能挂载、能读根目录、容量数字全对（185 MB 可用，和 Mac 读数
一致），但写任何文件都失败——`fopen("wb")` 成功、`fwrite` 返回完整字节数、
`fclose` 返回 0，紧接着 `stat()` 就报 `ENOENT`。20 MHz 和 4 MHz 表现完全一样。

**根因**：卡坏了。它对写操作回 ACK、报成功，但什么都不提交；读永远返回原始内容。
这是 SD 卡寿命到头（或假卡）的典型失效模式——连整盘 `diskutil eraseVolume`
都会被静默吞掉。换一张卡，同一套接线、同一版固件，全部一次通过。

⚠️ **走过的最大一段弯路：把卡插到 Mac 上验证，得出了完全相反的结论。**
`echo > test.txt` 之后 `cat` 能读出内容、8 MB `dd` 跑出 13 MB/s，于是判定
「卡是好的，问题在固件」，掉头去查 FATFS 配置。全是 **page cache 在撒谎**，
数据压根没离开内存。真相要靠 `diskutil unmountDisk` + `mountDisk` 强制清缓存后再看：
卷名没变、已用空间没变、刚建的文件消失、2017 年的旧内容原样还在。

**教训：查嵌入式端的存储问题时，宿主 OS 的文件系统缓存会把失败伪装成成功。
没有 page cache 的 MCU 反而是更诚实的证人——它报 `ENOENT` 的时候，
第一个该怀疑的是介质，不是它。**

对照实验这次同样是决定性的那一步：同一套接线、同一版固件、只换一张卡就全通过，
唯一变量锁死在卡上。和上面摇杆那次是同一个方法。

顺带一个诊断陷阱，值得单独记：**`fwrite` 只写进 stdio 缓冲，真正落盘发生在
`fclose` 的 flush**。第一版自检没检查 `fclose` 的返回值，于是写失败一路溜到
下一个 `fopen` 才暴露，报成了「刚写的文件读不开」——错误信息指向了完全无关的
地方，白查一轮。现在 `sd_card.c` 检查 `fclose`，并把 `stat()`（目录项落没落盘）
和回读（数据对不对）分开报。

### ⚠️ 克隆后先补 ROM

`main/roms/smb.nes` 是版权物，**不在仓库里**（见 `.gitignore`）。
新克隆的副本缺这个文件会**链接失败**（`main/CMakeLists.txt` 的 `EMBED_FILES` 引用了它）。

自备一份《超级马里奥兄弟》的 `.nes`，放成 `main/roms/smb.nes` 即可。
不想弄的话把 `main/nes_emu.c` 顶部的 `ROM_CHOICE` 改成 `1`/`2`/`3`，
并从 `EMBED_FILES` 里删掉 `roms/smb.nes` 那行 —— 剩下三个公有领域测试 ROM 随仓库分发。

### 换 ROM

**游戏放在 TF 卡上**，拔下来插电脑拷进去就行，不用重烧固件。支持 `.nes`、`.gb`、
`.gbc`、`.sfc`、`.smc`、`.md`、`.bin`，也可以把这些 ROM 直接装在 `.zip` 里。
约定的目录结构是：

```
/sd/roms/nes/    /sd/roms/gb/    /sd/roms/gbc/    /sd/roms/snes/    /sd/roms/md/
```

裸 ROM 的目录名纯粹是给人看的，平台按扩展名归类，放错目录照样能玩。想临时下架
一批就挪进 `removed-YYYYMMDD/`——前缀是 `removed` / `_` / `.` 的目录整棵跳过。

ZIP 为了开机快，扫描时不会打开或检查内容，直接用外层文件名显示。放在推荐的
`nes/gb/gbc/snes/md/` 目录下会先归到对应平台；其他目录下先显示在 ZIP 分类，选中后
才识别并解压第一个受支持的 ROM。支持普通单卷 ZIP 的 store/deflate，不支持加密、
ZIP64、多卷、RAR 或 7z；不支持的包只会在选中时提示错误，不拖慢开机。

游戏目录没有固定数量上限，会在 PSRAM 中按需扩容，直到收录完整张卡；只有真实
内存不足才会停止。游戏越多，开机遍历全部文件名所需时间也越长——当前这张高延迟
EZSD1 卡实测收录 971 个游戏、5 个平台需要 30.83 秒，期间仍不会打开任何 ZIP。

第一次完整扫描后会在卡根目录写入隐藏文件 `.gamebox-rom-index`。以后开机直接顺序
读取这份带版本号和 CRC 的目录缓存，不再逐个遍历近千个文件；缓存缺失或损坏会自动
回退完整扫描。往卡里加、删或改名游戏后，**按住 SELECT（Shield F）再开机/复位**，
直到串口出现“检测到 SELECT”即可松手，设备会忽略旧缓存、重扫并原子更新索引。
直接在电脑上删除 `.gamebox-rom-index` 也能触发重建。

⚠️ **显示名来自文件名**，会自动剥掉 `(Japan, USA)` `[!]` 这类标记和开头的 `NN_`
排序前缀。`Super Mario World (USA).sfc` 在菜单里显示成 `Super Mario World`。

换游戏仍按板子 RST 重启，避免在模拟器之间留下状态。

#### 卡的选择很影响体验

ROM 是选中之后才从卡上整个读进 PSRAM 的；ZIP 也在这时才识别和解压，所以
**卡的速度直接决定进游戏要等多久**。
加载页会显示当前游戏、阶段和百分比；裸 ROM 按已读取字节推进，ZIP 依次显示
“识别 ZIP / 解压 ZIP / 校验 ROM / 准备启动”。进度最多每 5% 刷新一次，避免
为了动画反过来拖慢读卡，加载期间也会明确提示不要拔 TF 卡。
本机拿一张 2 GB 的老 SDSC 卡实测：每条 SD 命令有约 **40 ms** 的固定响应延迟
（正常卡应当 <1 ms），结果开机扫 39 个游戏要 2.8 秒，1 MB 的塞尔达要读 8 秒。
把 SPI 时钟从 20 MHz 降到 4 MHz 做过单变量对照，固定开销纹丝不动（38→40 ms），
只有传输部分按比例变慢——**所以那是卡自己的延迟，不是接线或缺上拉，降频治不了。**

换一张正常的 SDHC 卡这些数字会小一个量级。想量自己手上这张，把
`main/sd_card.c` 的 `SD_BENCHMARK` 打开，判读方法写在那个函数的注释里。

开机首页先选 **GAME / WORDS / SETTINGS**：GAME 进入游戏，WORDS 进入离线单词学习，
SETTINGS 调节音量、背光并进入手柄诊断。游戏选单分两级：**平台选择页**
（NES / GB / GBC / SNES / MD / PCE，各带游戏数）
采用 2×3 卡片网格，按 A 进入该平台的**游戏列表**；列表每页 8 项，编号独立显示
为徽标，右上角同时显示平台游戏总数和页码。在列表里按 B 退回平台页，编号每个
平台都从 01 起，左右仍是整页翻页。

**游戏收藏**：在游戏列表选中一项，按 **X（Shield 上方大键 A / 串口 U）**收藏，
游戏名右侧出现 `★`；再按一次取消。平台页按 **SELECT（Shield F / 串口 Tab）**
进入“我的收藏”，集中显示所有平台的收藏，名字前带平台标记，A / START 直接启动。
收藏页同样支持翻页和 X 取消收藏，B 返回平台页；取消最后一个收藏后显示空列表。

收藏按完整文件路径保存在 TF 卡根目录的 `.gamebox-favorites`，重启或按住 SELECT
重建 ROM 索引后仍保留，普通 ROM 和 ZIP 都支持。加删游戏后仍需按前述方式刷新索引；
收藏中只显示当前索引里的游戏，移走的游戏记录会保留，同路径放回并刷新后恢复。
改名或移动路径后需要重新收藏。保存采用临时文件和 `.bak` 备份，界面提示保存失败时
收藏状态保持原样；启动读取主文件失败会尝试备份，不会自动覆盖损坏的收藏记录。

| 键 | 平台选择页 | 游戏列表 |
|---|---|---|
| A / START | 进入该平台 | 启动游戏 |
| B（SNES B / Shield C） | 返回 GAME / WORDS / SETTINGS | **返回平台页** |
| X（Shield A / 串口 U） | — | 收藏 / 取消收藏 |
| SELECT（Shield F / 串口 Tab） | 打开“我的收藏” | — |
| 摇杆上下 | 选平台 | 选游戏 |
| 摇杆左右 | 选同一行的平台 | 翻页 |

B 在两页始终返回上一级：游戏列表退到平台页，平台页退到 GAME / WORDS / SETTINGS。

SETTINGS 采用与主页一致的 gruvbox light retro-go Options 风格：上下选择
`Brightness`、`Volume` 或 `Controller Test`，左右调节当前档位；调音量时会立即
播放一次 `hello` 试听，A 进入手柄测试，B 返回首页。设置只对本次开机有效，RST
或重新上电恢复默认。

### 单词学习（WORDS）

面向苏州小学三至六年级，按译林版教材的年级、上/下册和 Unit 1–8 选择；三上
完整收录教材 Word lists 第 78～80 页的 127 项，包含带 * 的非核心词，各单元分别为
13/11/17/9/18/12/31/16 项；其余分册暂时每单元内置 8 个核心词。词表不依赖 TF 卡。
每轮严格学习所选单元的全部词条：先看
英文并猜意思，按 A 翻卡查看中文；看完后对同一组词做三选一。答对会提升掌握度，
答错会回到待复习，经过三次成功提取才计入“已掌握”。进度按教材版本和词分别保存
在 NVS，换单元或断电后仍保留。

| 页面 | 操作 |
|---|---|
| 教材 / 单元选择 | 方向键选择，A / START 确认；B / SELECT 返回上一级 |
| 单词正面 | A / START 翻开；SELECT 返回所选单元首页 |
| 单词背面 | A / START 下一张；B 盖回去再想一次 |
| 三选一 | 上下选择，A / START 确认；答题后再按 A 继续 |
| 本轮结算 | A / START 再来一轮；B / SELECT 返回所选单元首页 |

`main/roms/` 只用于 TF 卡不可用时的 NES 编译期回退——没插卡、卡挂不上、或者卡上
一个合法 ROM 都没有时，会直接进这个内置游戏（当前是魂斗罗）。只有要更换这个
回退游戏时，才需要同步修改 `main/CMakeLists.txt` 的 `EMBED_FILES` 和
`main/nes_emu.c` 的 `ROM_CHOICE`。

⚠️ **已知问题**：没插卡时屏幕上没有任何提示，直接跳进内置游戏，看起来像坏了。
串口有日志。该加一屏「未检测到 TF 卡」的提示。

### 操作（手柄）

JoyStick Shield，接线见「接线」一节。飞线手柄、USB 手柄、串口键盘三路输入
**并存**（按位或），串口留着当调试后路。

| 物理位置 | Shield 丝印 | SNES | Genesis | NES / GB / GBC |
|---|---|---|---|---|
| 上方大键 | A | X | 无 | 无 |
| 左方大键 | D | Y | C | 无 |
| 右方大键 | B | A | B | A |
| 下方大键 | C | B（选单中：返回上一级） | A | B（选单中：返回上一级） |
| 左侧小键 | F | SELECT | SELECT |
| 右侧小键 | E | START | START |
| 摇杆 | — | 上下左右 | 上下左右 |

**游戏内菜单 / 即时存档**：不分平台，游戏中同时按小按键 F（SELECT）和上方大键 A（X）
即可打开菜单，不需要长按。摇杆上下选择，A 确认，B 返回游戏或上一级：

- `SAVE & CONTINUE`：选择 SLOT 0～3，保存后继续游戏；
- `SAVE & QUIT`：存档成功后软重启回开机选单，失败则留在菜单；
- `LOAD GAME`：从四个槽中恢复，空槽会标成 `EMPTY`；
- `RESET`：再选软重置或硬重置；
- `QUIT`：不保存，软重启回开机选单。

槽文件放在 `/gamebox/saves/{nes,gb,gbc,snes,md}/<ROM CRC>.sav0`～`.sav3`。
同名改版、不同区域版或重命名后的同一 ROM 都按内容 CRC 区分/识别。保存先完整写 `.tmp`，
再用 `.bak` 保护旧档后改名提交；若上次恰好在改名中间断电，读档会回退 `.bak`。
GB/GBC 和 NES 的核心快照也包含卡带 SRAM，因此 Pokemon 等游戏内电池进度会随槽一起保存。

上电时**手别碰摇杆**：开机要采 16 次静止读数做中位校准，碰着会被判定为
「没接好」而整路禁用（按键不受影响）。

### 操作（串口键盘）

真手柄到货前的临时方案，现在仍作为调试后路保留。
`idf.py monitor` 会把你敲的字符经串口发给板子。

```bash
idf.py -p /dev/cu.usbserial-A5069RR4 monitor
```

**焦点要在 monitor 窗口里**，然后：

（`U`/`I` 是给 SNES 补的两颗面键。低 8 位和 NES 手柄一致，NES/GB/GBC 会忽略这两位。）

| 键 | 作用 |
|---|---|
| `W` `A` `S` `D` 或方向键 | 上下左右 |
| `K` 或 `Z` | A（跳） |
| `J` 或 `X` | 选单：返回上一级；游戏：B（跑 / 发射） |
| `U` | 游戏：SNES X |
| `I` | SNES Y |
| 回车 | START |
| Tab | SELECT |
| `Tab` + `U` | 打开游戏内菜单（SELECT + X） |
| 空格 | 全部松开（按键卡住时用） |
| `Ctrl+]` | 退出 monitor |

还可以接有线 USB HID 手柄，三路输入会按位合并，不需要在菜单中切换。使用丝印
`USB` 的 Type-C 口并短接板背面的 `USB-OTG` 焊盘；丝印 `COM` 的 Type-C 口继续
负责供电、烧录和串口。默认按通用 RetroPie/SNES 编号映射：Button 1/3 → B，
Button 2/4 → A，Button 9 → SELECT，Button 10 → START。首次接入会在串口打印
VID/PID、解析结果和前 40 次原始报告，便于识别换芯片的手柄批次。
开机摇杆诊断页也接受 USB 手柄 A 或串口 `K/Z` 退出，不要求保留飞线手柄。

> **当前实机状态（2026-08-14）**：这块开发板能检测到 USB 设备接入，但持续报
> `HUB: Root port reset failed`，尚未通过 USB 手柄验收。已暂停继续排查；详细实验、
> 已排除项和恢复入口见
> [`docs/usb-hid-investigation-2026-08-14.md`](docs/usb-hid-investigation-2026-08-14.md)。

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

**`main/` 下的代码（本项目自己写的部分）以 GPL v2 发布**，许可文本见仓库根的
`LICENSE`。选 GPL v2 是被约束出来的：`main/` 链接 GPLv2-only 的 nofrendo，
再宽松的选择都会让合并作品不合规。

`components/` 下的五个模拟器核心各自保持上游许可，**没有被本项目重新授权**：

| 组件 | 上游 | 许可 | 备注 |
|---|---|---|---|
| `nofrendo/` | [retro-go](https://github.com/ducalex/retro-go) → Matthew Conte 的 Nofrendo | `COPYING` 是 GPL v2 | ⚠ 73 个源文件头写的是「version 2 of the GNU **Library** GPL」，且**没有 or later**。头和 COPYING 不是同一个许可证 |
| `gnuboy/` | retro-go | `COPYING` 是 GPL v2 | ⚠ 源文件**完全没有许可头**，只能靠 COPYING 推定 |
| `snes9x/` | retro-go → libretro/snes9x2005 | `src/LICENSE` 是拼接的：前半 ndssfc 为 GPL v2+，后半是 Snes9x 自有许可 | 见下 |
| `gwenesis/` | retro-go | 目录 `LICENSE` 是 **AGPL v3** | ⚠ 各源码文件头写的是 **GPL v3 or later**，与目录 LICENSE 不一致 |
| `pce-go/` | retro-go → [HuExpress](https://github.com/kallisti5/huexpress) → Hu-Go! → BERO 的 PCE 模拟器 | `COPYING` 是 GPL v2 | ⚠ 和 gnuboy 一样，13 个源文件**完全没有许可头**，只能靠 COPYING 推定。作者链条见 `CREDITS` |

菜单里的 8×16 半角英文和 16×16 全角中文都来自 GNU Unifont 17.0.04，字高和
基线一致。该字体采用 SIL OFL 1.1，或 GPL v2+ 带字体嵌入例外，没有许可问题。

### ⚠ 为什么本仓库不分发构建产物

**只发源码、不发 `.bin`，是一个有意的许可决定，不是偷懒。** 源码目录里各组件并列
属于 GPL 意义上的「聚合」；真正的合并作品是链接出来的固件二进制，而它有两处
无法回避的不兼容：

1. **Snes9x 的「仅限非商业」限制和 GPL 不兼容。** 注意方向：Snes9x 许可证原文
   *明确允许*非商业地分发源码和二进制（"Permission to use, copy, modify and/or
   distribute Snes9x in both binary and source form, for non-commercial purposes,
   is hereby granted without fee"）。违反的是 GPL —— GPL 禁止在再分发时附加任何
   额外限制，而 nofrendo/gnuboy 是 GPL。**所以这不是「只能非商业分发」，
   是合并作品根本不能按 GPL 分发。**

2. **GPLv2-only 和 GPLv3/AGPLv3 不能合并。** nofrendo 的源文件写死 version 2、
   没有 or later；gwenesis 是 GPL v3+ / AGPL v3。**就算删掉 snes9x，这个组合
   仍然不合规。**

   加入 pce-go 不改变这个结论：它是 GPL v2，和 nofrendo/gnuboy 同一阵营，
   冲突的仍然是 snes9x 和 gwenesis 那两条。

加上上表里三处上游元数据自相矛盾（nofrendo 头 vs COPYING、gnuboy 没有头、
gwenesis 目录 vs 头），每一条都得先定性才谈得上发布二进制。

所以本仓库的立场是：**自己 `idf.py build`。** 个人编译自用不触发任何分发义务。
想发布烧好的固件，得先砍到单一许可阵营（比如只留 NES + GB/GBC 走 GPL v2），
或者换掉冲突的核心。

ROM 文件的版权归各自权利人所有，商业 ROM 一律不入库，由使用者自备。

（以上是读各许可文件得出的结构分析，不构成法律意见。真要对外发布二进制，
请找人正式审一遍。）
