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

被砍的是 Retro-Go 干的，**不是上游 snes9x2005 没有**。拿
`libretro/snes9x2005` 的 `source/` 和本目录 `src/` 比对，上游多出这些文件：

```
fxemu.c  fxemu.h  fxinst.c  fxinst.h      ← Super FX / GSU
sa1.c  sa1.h  sa1cpu.c                    ← SA-1
sdd1.c  sdd1.h  sdd1emu.c  sdd1emu.h      ← S-DD1
spc7110*  seta*  dsp1*/dsp2/dsp4  cheats*
```

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

**补回来不是把文件拷贝回来那么简单**，因为 Retro-Go 顺手把钩子也拆了。
最关键的是 `cpuexec.c`：上游（这段继承自 CATSFC）按整帧用到的芯片在
`S9xMainLoop_{SA1,NoSA1}_{SFX,NoSFX}` 四套主循环里选一套，为的就是省掉
每条指令上的 `Settings.SuperFX` / `SA1.Executing` 判断——本目录只保留了
`NoSA1_NoSFX` 那一条。所以恢复 Super FX 等于部分回退 Retro-Go 的提速改造，
还要把散在 `memmap.c` / `getset.c` / `gfx.c` / `tile.c` 里的 GSU 钩子接回去。
和上游同名文件的纯内容分歧（先 `tr -d '\r'` 去掉 CRLF 再 diff）合计 9097 行，
其中 `memmap.c` 2410 行、`cpuexec.c` 587 行、`gfx.c` 405 行。
代价还包括破坏「`src/` 一个字节都没改、上游可直接覆盖」这条约定。

**体积不是障碍**：`fxemu.c` 7.7 KB + `fxinst.c` 70 KB + 两个头文件 11 KB
≈ 89 KB 源码；app 分区为 Genesis 扩到 2 MB 之后，当前固件 1.46 MiB、
还剩 548 KiB。（此处原先写「app 分区只剩 126 KB」，那是 app 还是 1 MB
时的数据，扩容后已失效。）

**但性能上没戏，别做。** Super FX 游戏里 GSU 才是主力——多边形/位图是它
渲染进 RAM 的，65816 反而在等它。Star Fox 的 GSU 跑 10.7 MHz、Yoshi's
Island 的 GSU-2 跑 21.4 MHz，等于在一颗已经只能跑到 45/60 fps 的 65816
之外再全速模拟一颗更忙的处理器。SA-1 同理（本身就是 10.74 MHz 的 65816）。
下面「到不了可玩状态」的结论对这些卡带只会更成立。

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

### 一帧的时间都花在哪（实测分项，凑满整帧）

`snes_emu.c` 的统计行把一帧拆成六项，**加起来等于整帧**，所以没有能藏东西的缝。
Super Mario World 实跑（`SNES_PROFILE_INPUT` 可另外拆出三路输入）：

| 项 | 轻场景 | 重场景 | 说明 |
|---|---|---|---|
| 输入 | 0.2 | 0.2 | 串口 0.04 + 摇杆 0.18 + **USB 0.00** |
| **模拟** | **9.4** | **18.6** | `S9xMainLoop()`，唯一的瓶颈 |
| 拷贝 | 0.8 | 0.8 | 帧缓冲 memcpy 到 PSRAM 影子缓冲 |
| 音频 | 0.5 | 3.9 | 声道全静音时几乎免费，8 声道全开才贵 |
| 配速 | 6.7 | 1.7 | **跑得越快这项越大** —— 是在等，不是浪费 |
| 其它 | 0.2 | 0.3 | |
| 帧 | 17.8 | 25.1 | 56 fps / 40 fps |

**60 fps 够不到，别再试了**：光模拟就要 14~18.6 ms，而 60 fps 的预算是 16.67 ms。
就算把其余五项全部归零，重场景仍然超预算。

两条走过的弯路，都已实测否定：

- **输入轮询不是瓶颈。** 曾经因为「CPU 余量 48% 却只有 52.9 fps」怀疑没被计时的
  输入轮询吃掉了 9 ms。实测三路加起来 0.2 ms/帧，USB 那路是 0.00。那 9 ms 是
  假象：拿来外推的那行统计是在 `SNES_DIAG_MUTE_PCM` 那次抓的，而当秒
  `PCM峰值 0、非零帧 0/53` —— 游戏停在标题画面没出声，8 个声道全 `SOUND_SILENT`
  时 `MixStereo` 直接 `continue`，模拟和混音都异常轻。**用静止场景的数去推整帧
  是方法错误。**
- **减少 vTaskDelay 无效。** 试过「每 4 帧才让一次核 0」，按场景对齐的 A/B 里
  每一档 fps 都在 ±0.2 内，配速也没变。原因见 `snes_emu.c` 配速那段注释。

### 跑不满 60 fps 直接决定了音频怎么产样

上面那条 45/60 fps 不只是"慢"，它还会让音频欠喂。原来每帧固定产
`24000/60 = 400` 个采样，只有跑满 60 fps 时每秒才凑够 24000 个；实际
45~53 fps 下每秒只产 18000~21000 个，喂给固定 24 kHz 的 I2S 缺 15%~25%，
由驱动的 `auto_clear_after_cb` 补零，等于音频流里有那么多比例是静音。

⚠ **别把这条当成"杂音"的解释。** 修完欠喂，实测听感没有任何变化 ——
真正的杂音来自 SMW 恢复即时存档（见下一节）。这条是独立的真缺陷，
改它是因为往 24 kHz 的管道里灌 18 kHz 的料本身就不对，不是因为它有声音症状。

**换 I2S 采样率解决不了**：设采样率 R、每帧产 R/60，实际产出 F·R/60，
要等于 R 就得 F=60，R 直接约掉。唯一的解是按真实经过的墙钟时间产样
（`snes_emu.c` 主循环里的 `audio_accum`）。这样音画一起变成 75% 慢放，
音高不变（音高由 DSP pitch 寄存器和 `so.freqbase` 决定，与产多少个采样无关），
只有音符事件跟着模拟速度一起变慢，和画面是一致的。

代价是混音量按同样比例上涨（45 fps 下每帧 533 而不是 400 个采样，约 +33%
的 `dsp.c` 混音时间）。这也意味着**这条路径会随帧率自动变化**：以后要是把
SMW 优化到 60 fps，每帧就自然回到 400，不用改任何常数。

## SMW 即时存档

仅 ROM 内部名为 `SUPER MARIOWORLD` 的卡带启用：同时长按 SELECT + START 1 秒保存，
下次启动同一 ROM 自动恢复。单份快照 365,120 字节；Flash 尾部 960 KiB 的 FAT 分区
套 wear levelling，并用 A/B 双槽、ROM CRC、状态 CRC 和序号保证中途断电仍能回退。
功能只包装本核心已有的 `S9xSaveState` / `S9xLoadState`，没有修改 `src/`。

### 恢复快照会毁掉声音（以及排查它走过的弯路）

**症状**：SMW 恢复即时存档后声音永久是杂音；菜单里按住 Y 冷启动同一个卡带
声音正常。只有 SMW 有这个问题，因为它是唯一会自动恢复快照的卡带。

**成因**：`snapshot.c` 把整个 `SoundData` 原样写盘、原样读回，而上游的
`S9xFixSoundAfterSnapshotLoad()`（`soundux.c:266`）只重建三样：echo 参数、
8 个滤波系数、每声道的 `frequency` 和 `envxx`。包络（`mode`/`state`/各段速率/
`erate`/`envx_target`）、每声道音量档位、主音量全部沿用快照里的原值。

**修法**在 `main/snes_emu.c` 的 `rebuild_sound_after_resume()`：不就地修补，
而是 `S9xResetSound(false)` 把混音器拉回静止，再整体从 `APU.DSP[]` 重放一遍。
DSP 寄存器和 `IAPU.RAM` 都是快照里逐字节恢复的权威数据，从它们推导出来的
状态一定自洽。就地修补那条路以前走过一次，失败结论记在主循环的看门狗注释里。

**排查花了四轮，值得记下来的教训**：一上来就该问「以前好过吗」。
「最早的时候声音是好的」这一句直接把范围缩到"存过档之后"，而在问出这句之前
查掉的三条全是死路 —— 音频欠喂、`MixBuffer` 越界、24 kHz 重采样关掉插值。
三条都是真缺陷（前两条已修，第三条的推导记在 `snes_emu.c` 音频常量那段注释里），但都不是
这个症状的原因。**排查回归时，先定位"什么时候变坏的"，再定位"哪里坏了"。**

另外两个当时很有用的对照手段：
- `SNES_DIAG_MUTE_PCM`（`snes_emu.c`）—— I2S 照常跑、只把 PCM 清零。一次就
  分开了「杂音在数据里」和「杂音在电源/功放上」。
- ⚠ **不能用菜单音量 0% 做这个对照**：那条路径连 I2S 都不创建，而 MAX98357
  在 BCLK/LRCLK 消失时自动关断 —— 功放一关，两种来源的噪声都会一起消失。
