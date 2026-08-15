# Gamebox 集成说明

本目录取自 `ducalex/retro-go` 的 `gwenesis/components/gwenesis`，基线提交：

```text
4ced120669750ca7228fd0414211430c1d923166
```

为了接入 ESP-IDF 5.4 和本机内存布局，只保留三类必要改动：

1. `CMakeLists.txt` 从旧 `register_component()` 改成 `idf_component_register()`，
   并定义 `RETRO_GO=1`，让卡带直接引用宿主已解压到 PSRAM 的数据。
2. 原版静态 `M68K_RAM[64 KiB]` 改成指针，由 `main/genesis_emu.c` 在选中
   Genesis、释放 NES 预留缓冲后申请片内 SRAM。
3. `ZRAM[8 KiB]`（`gwenesis_bus.c`）以及 `ym2612.c` 里开机算一次的三张查表
   `tl_tab`（26 KiB）/`sin_tab`（4 KiB）/`lfo_pm_table`（16 KiB）同样改成指针，
   在各自第一次真正用到时（`load_cartridge()` / `init_tables()`）用
   `heap_caps_malloc(MALLOC_CAP_INTERNAL)` 申请，不再是编译期常驻数组。
   原因和第 2 条一样：`static` 数组只要链接进固件就永久占内部 SRAM，跟选没选
   Genesis 无关。第 2 条只改了 M68K_RAM 一处，遗漏了这四处，实测 SNES
   仍然会在启动阶段因内部 SRAM 不足而黑屏——2026-08-15 定位并补上。
   这四处加起来约 54 KiB，改完后开机内部 SRAM 剩余量基本回到接入 Genesis
   之前的水平（用 `idf.py size` 对比 DIRAM 的 `.bss`/`Remain` 可复核）。

Genesis 执行顺序、CPU、VDP、Z80、YM2612 和 PSG 算法均保持 retro-go 版本。

实机还对比过 `GWENESIS_AUDIO_ACCURATE=0` 加 `-Ofast`：Sonic 重场景从约
38–49 fps 降到 28–43 fps，声音断续更严重，因此恢复上游的 cycle-accurate
声音和 `-O2`。单核算力不足时，I2S 队列无丢包也不能避免 PCM 供给速度低于播放速度。

授权元数据沿用上游原样：目录 `LICENSE` 是 GNU AGPL v3，各 Gwenesis 源码文件头
写 GNU GPL v3 or later。两者不一致；公开分发前应向上游确认准确授权。
