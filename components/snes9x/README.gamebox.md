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

宿主的显示 / 输入 / 音频 / 内存布局全部在 `main/snes_emu.c`，上游更新可以
直接覆盖 `src/`。

## ⚠ 授权和 nofrendo/gnuboy 不同

`src/LICENSE` 是 Snes9x 自己的许可证，**明确禁止商业分发**，不是 GPL。
本固件本来就因为 nofrendo 受 GPL v2 约束，链接这份代码之后分发限制更严。

## 性能实测（ESP32-S3 @240MHz，Super Mario World）

结论：**到不了可玩状态**，最好的配置下约 45/60 fps（75% 速度）、推屏 10.8 fps。

逐项试过的优化：

| 改动 | 模拟 fps |
|---|---|
| 基线（帧缓冲在 PSRAM、同步推屏） | 38~39 |
| 帧缓冲挪进内部 SRAM | 43 |
| 异步推屏（PSRAM 影子缓冲）+ 音频按真实帧率产样 | 45 |
| WRAM 挪进内部 SRAM（帧缓冲退回 PSRAM） | 42~44（更差，已回退） |

最后一行最有信息量：**换哪块热内存进内部 SRAM 都救不了**。整个工作集
（ROM 512 KB + SubScreen/ZBuffer 245 KB + WRAM 128 KB + VRAM 64 KB ≈ 950 KB）
塞不进 179 KB 内部 SRAM，内存布局这条路已经走到头，再要速度只能动核心本身。

音频不是瓶颈（实测 1.2 ms/帧）。上游 Retro-Go 的 README 把 SNES 标成
"(slow)"、`main_snes.c` 把跳帧初值写死为 3，都和这里的实测一致。
