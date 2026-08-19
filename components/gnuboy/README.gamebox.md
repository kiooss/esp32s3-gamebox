# gnuboy 来源

本目录从 Retro-Go 的 `retro-core/components/gnuboy` 原样引入，基线提交：

`4ced120669750ca7228fd0414211430c1d923166`

上游地址：https://github.com/ducalex/retro-go

只对 `gnuboy.h` 做了一处 ESP-IDF 适配：在 ESP 平台包含 `esp_attr.h`，让 CPU
热点函数继续使用真正的 `IRAM_ATTR`。宿主显示、输入和音频适配全部放在
`main/gbc_emu.c`，以后更新上游时不需要反复修改模拟器核心。

## ⚠ mapper：MBC6 / MBC7 / MMM01 没有实现

`gnuboy.c` **认得出**这几种卡带类型、给 `cart.mbc` 赋了对应的枚举值（MBC7 还
顺带设了 `cart.has_sensor`），但 `hw.c` 的 `mbc_write()` 分发里只写了 MBC1 /
MBC2 / MBC3 / MBC5 / HuC1 / HuC3 六个 `case`，**没有 `default:`**。于是这几种
卡带的 bank 切换写入被静默丢弃，`cart.rombank` 永远停在 1，游戏被困在头 32 KB。

**表现是黑屏**，不是报错——和崩溃、性能问题看着一模一样，极难判断。

| 卡带类型（头 `0x147`） | mapper | 状态 |
|---|---|---|
| 0x00, 0x08~0x09 | ROM only | ✅ 不需要 bank 切换 |
| 0x01~0x03 | MBC1 | ✅ `hw.c:351` |
| 0x05~0x06 | MBC2 | ✅ `hw.c:373` |
| **0x0B~0x0D** | **MMM01** | ❌ `gnuboy.c` 认，`hw.c` 无 case |
| 0x0F~0x13 | MBC3 | ✅ `hw.c:383` |
| 0x19~0x1E | MBC5 | ✅ `hw.c:404` |
| **0x20** | **MBC6** | ❌ 同上 |
| **0x22** | **MBC7 + 加速度计** | ❌ 同上 |
| 0xFE / 0xFF | HuC3 / HuC1 | ✅ `hw.c:381` / `hw.c:432` |

实测触发过：**Kirby Tilt 'n' Tumble**（コロコロカービィ，内部标题 `KORO2 KIRBY`，
`0x147 = 0x22`）。同批 8 个能跑的 GB/GBC 游戏全是 MBC1 或 MBC5。顺带排除过
CGB-only 标志（`0x143 = 0xC0`）这个嫌疑——能跑的三个 GBC 游戏也都是 `0xC0`。

`tools/pack_roms.py` 现在会在打包时按这张表检查并警告，不用烧完才发现。

**补 MBC7 意义不大**：那类游戏全靠倾斜卡带里的加速度计操作，没有方向键控制。
这块板子的两轴 ADC 摇杆理论上能映射过去，但 `mbc_write()` 是 `hw.c` 里的
`static inline`，`rg_system.h` 那层垫片够不到，必须直接改本目录——那就破坏了
「原样引入、上游可直接覆盖」这条约定。
