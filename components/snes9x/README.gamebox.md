# snes9x 来源

本目录从 Retro-Go 的 `retro-core/components/snes9x` 原样引入，基线提交：

`4ced120669750ca7228fd0414211430c1d923166`

上游地址：https://github.com/ducalex/retro-go
（Retro-Go 那份又来自 https://github.com/libretro/snes9x2005）

`src/` 下**一个字节都没改**。粘合只有本目录新增的两个文件：

- `rg_system.h` —— `src/port.h` 在 `RETRO_GO` 下会 include 它。snes9x 其实
  一个 `rg_*` 都没调用，这个垫片只是让 include 不报错。
- `CMakeLists.txt` —— 换成本项目的写法。三个 `-D` 是上游给 ESP32 定的性能
  开关，不是可选项；`-O2` 而不是 nofrendo 那样的 `-O3`，因为这份核心大一个
  数量级（`cpuops.c` 73 KB、`dsp.c` 141 KB、`gfx.c` 101 KB），`-O3` 的循环
  展开会把 `.text` 撑出 1 MB 的 app 分区。

宿主的显示 / 输入 / 音频 / 内存布局在 `main/snes_emu.c`，SMW 即时存档的 FAT、
双槽和 CRC 外壳在 `main/snes_save.c`；两者都只调用核心现成接口，上游仍可直接覆盖
`src/`。

## ⚠ 授权和 nofrendo/gnuboy 不同

`src/LICENSE` 是 Snes9x 自己的许可证，**明确禁止商业分发**，不是 GPL。
本固件本来就因为 nofrendo 受 GPL v2 约束，链接这份代码之后分发限制更严。

## ⚠ 协处理器：Super FX / SA-1 / S-DD1 没有实现

这份核心砍掉了几个协处理器，**卡带能被识别、能加载、就是跑不动**，
表现不是报错而是黑屏或停在 logo，很容易误判成性能问题。

| 芯片 | 状态 | 依据 |
|---|---|---|
| DSP-1/2/3/4 | ✅ 有实现 | `dsp.c`（DSP-1 已在板上实测，见下） |
| OBC1 | ✅ 有实现 | `obc1.c`，`getset.c` 里有路由 |
| S-RTC | ✅ 有实现 | `srtc.c`，`cpu.c`/`ppu.c` 里有调用 |
| C4 | ⚠ 有实现文件 | `c4.c`/`c4emu.c` 存在，但没在 `getset.c` 里找到路由，未实测 |
| **Super FX (GSU)** | ❌ **只有声明** | `ppu.h:209` 有 `S9xSuperFXExec()` 一行声明，无实现文件、无调用者 |
| **SA-1** | ❌ 只认卡带 | `memmap.c` 有 `Settings.SA1` 检测，无 `S9xSA1Main` |
| **S-DD1** | ❌ 只认卡带 | 同上，`SDD1` 只出现在 `memmap.c`/`snes9x.h` |

想确认一个卡带用了什么芯片，看内部头 `0x7FC0`（LoROM）或 `0xFFC0`（HiROM）
往后第 0x16 字节：`0x05` = ROM+DSP，`0x13~0x1A` = Super FX，`0x34/0x35` = SA-1。

**Super FX 的典型症状**：模拟耗时反而变低并卡平（Yoshi's Island 实测从
11.0 ms 掉到 6.8 ms）、稳定 60 fps、CPU 余量 53%。这不是「跑得快」，
是 65816 在空转等一个永远不来的 GSU 应答。看到「fps 满、余量大、画面不动」
就该往这里想，别去调跳帧或内存布局。

要补的话得从上游 snes9x 移植 `fxemu.c`/`fxinst.c`，代价是破坏
「`src/` 一个字节都没改、上游可直接覆盖」这条约定，且 app 分区只剩 126 KB。

## 性能实测（ESP32-S3 @240MHz）

结论：**到不了可玩状态**。Super Mario World 最好的配置下约 45/60 fps
（75% 速度）、推屏 10.8 fps。

| 卡带 | 芯片 | 模拟 fps | 备注 |
|---|---|---|---|
| Super Mario World | 无 | 45~46 | 基准 |
| Super Mario Kart | DSP-1 | **49~50** | 比 SMW 还快，音频零丢帧 |
| Yoshi's Island | Super FX | （60，空转） | 跑不了，见上 |

Mario Kart 更快是反直觉的 —— Mode 7 + 每帧 DSP-1 运算看着该更重。
说明瓶颈不在这些「看起来贵」的特性上，而在下面那条内存墙上：
DSP-1 的调用量小，Mode 7 在 snes9x 里也比多层卷轴 + 大量精灵便宜。
**别用「这游戏特效多所以慢」来估帧率，只信串口那行统计。**

以下是用 Super Mario World 逐项试过的优化：

| 改动 | 模拟 fps |
|---|---|
| 基线（帧缓冲在 PSRAM、同步推屏） | 38~39 |
| 帧缓冲挪进内部 SRAM | 43 |
| 异步推屏（PSRAM 影子缓冲）+ 音频按真实帧率产样 | 45 |
| WRAM 挪进内部 SRAM（帧缓冲退回 PSRAM） | 42~44（更差，已回退） |

最后一行证明的是：**用 WRAM 替换内部帧缓冲没有收益**。它不能推出所有内存布局都已
测完——64 KiB VRAM 还没有做过内部 SRAM 对照。只是帧缓冲 119.5 KiB + VRAM 64 KiB
已经超过约 179 KiB 的内部预算，测试 VRAM 时大概率必须把帧缓冲退回 PSRAM；而且
512 KiB `IPPU.TileCache` 等热数据仍留在外部，所以不可能靠一次搬迁装下整个工作集。

完整对象表、启动阶段 7.28 MiB 已知大块合计、稳定期估算和 SRAM 名词区分见
[`../../docs/memory.md`](../../docs/memory.md)。

音频不是最大瓶颈，但实测混音和提交约 1.2~1.5 ms/帧；SMW 本来就常常超过
16.7 ms 帧预算，进游戏前关闭声音会直接释放约 7%~9% 的核 0 时间，主观和实际都
可能更流畅。上游 Retro-Go 的 README 把 SNES 标成 "(slow)"、`main_snes.c` 把
跳帧初值写死为 3，都和这里的实测一致。

## SMW 即时存档

仅 ROM 内部名为 `SUPER MARIOWORLD` 的卡带启用：同时长按 SELECT + START 1 秒保存，
下次启动同一 ROM 自动恢复。单份快照 365,120 字节；Flash 尾部 960 KiB 的 FAT 分区
套 wear levelling，并用 A/B 双槽、ROM CRC、状态 CRC 和序号保证中途断电仍能回退。
功能只包装本核心已有的 `S9xSaveState` / `S9xLoadState`，没有修改 `src/`。
