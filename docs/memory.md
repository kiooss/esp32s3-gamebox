# 内存与 Flash 布局

本文统一说明本项目里容易混淆的三种 “SRAM”、16 MB Flash 的用途，以及
NES、GB/GBC、SNES 三条模拟路径各自把数据放在哪里。

除特别注明外，本文的 `KiB` / `MiB` 都按 1024 进位；串口日志为了简短仍打印 `KB`。

## 1. 先分清三个同名概念

### 1.1 ESP32-S3 片内 SRAM

ESP32-S3 芯片标称有 512 KiB 片内 SRAM，CPU 直接访问。这个数字不是应用能拿来
`malloc` 的连续空间：代码、静态数据、任务栈、驱动、DMA 保留和堆碎片都会占掉一部分。

本项目进入 SNES 前先释放 NES 预留的两块视频缓冲；即便如此，当时能拿来争抢的大块
内部 heap 也只有约 179 KiB。这个 179 KiB 是特定启动阶段的实测预算，不是芯片容量。

### 1.2 片外 Octal PSRAM

模组是 ESP32-S3-WROOM-1-N16R8：`R8` 表示 8 MiB 片外 PSRAM。它通过 8 根数据线的
Octal SPI 总线访问，当前配置为 80 MHz，并经过 CPU cache。cache 未命中时远慢于片内
SRAM，因此适合大块、较冷或顺序访问的数据，不适合最热的逐像素随机访问。

PSRAM 不能直接充当本项目的 SPI LCD DMA 源。显示条带必须带
`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`；把它们放进 PSRAM 会触发驱动逐条带申请内部
DMA 缓冲并额外复制，既没有省下最终所需的内部内存，又增加开销。

### 1.3 被模拟 SNES 的 `Memory.SRAM`

Snes9x 的 `Memory.SRAM` 是被模拟卡带的电池存档 RAM，只是一个 64 KiB C heap 数组。
它和 ESP32-S3 的片内 SRAM 不在同一个概念轴上。类似地：

| Snes9x 字段 | 大小 | 被模拟硬件里的含义 |
|---|---:|---|
| `Memory.RAM` | 128 KiB | SNES 主内存，本文简称 WRAM |
| `Memory.SRAM` | 64 KiB | 卡带存档 RAM；实际卡带可能只使用其中一部分 |
| `Memory.VRAM` | 64 KiB | 图块图形和 tilemap；精灵属性表 OAM 是另一块状态 |
| `Memory.FillRAM` | 32 KiB | I/O 寄存器映射的后备区 |

这些字段的语义不决定访问速度。速度取决于承载数组最后落在片内 SRAM 还是片外 PSRAM，
以及访问模式和 cache 命中率。

## 2. 当前硬件与分配器规则

| 资源 | 物理容量 | 当前配置 | 主要用途 |
|---|---:|---|---|
| Flash | 16 MiB | QIO 80 MHz | bootloader、分区表、固件和 ROM 镜像 |
| 片外 PSRAM | 8 MiB | Octal 80 MHz | 大块模拟器工作内存和非 DMA 帧缓冲 |
| 片内 SRAM | 512 KiB 标称 | CPU 240 MHz；内部/DMA 保留池 8 KiB | 热数据、任务栈、DMA 条带、音频包 |

配置启用了 `CONFIG_SPIRAM_USE_MALLOC`。普通 `malloc` / `calloc` 的当前策略是：

- 申请不大于 16 KiB 时优先片内 SRAM；
- 申请大于 16 KiB 时优先 PSRAM；
- “优先”不是硬约束，分配器在首选区域不足时可以回退；
- 必须固定位置的缓冲使用 `heap_caps_malloc/calloc` 明确指定 capability。

因此，Snes9x 源码中的普通 `malloc(0x10000)` 在当前配置下通常落入 PSRAM，但它并不
像 `MALLOC_CAP_SPIRAM` 那样构成硬保证。判断实机结果时以 capability 查询和串口日志为准。

## 3. 16 MiB Flash 怎么使用

Flash 和 PSRAM 是两套独立资源。ROM 分区的 mmap 只消耗 Flash 地址映射窗口和少量页表，
不会把整个 ROM 镜像复制到 PSRAM。

| 分区 | 起点 | 分配大小 | 当前用途 |
|---|---:|---:|---|
| NVS | `0x009000` | 24 KiB | 声音开关等持久配置 |
| PHY init | `0x00F000` | 4 KiB | ESP-IDF PHY 数据 |
| factory app | `0x010000` | 1 MiB | 固件；当前参考构建为 919,312 字节 |
| roms | `0x110000` | 14 MiB | 当前参考镜像 10,848,792 字节（约 10.35 MiB） |

`roms` 分区止于 `0xF10000`，到 16 MiB Flash 末尾还有 960 KiB 未分区。ROM 镜像只在
加、删或替换游戏时用 `idf.py flash-roms` 单独烧录，普通 `idf.py flash` 不更新它。

NES 和 GB/GBC 可以直接使用 mmap 的只读 ROM 指针。SNES 是例外：Snes9x 的装载流程会
就地移动 512 字节头并改写映射区域，所以适配层必须把所选 ROM 复制到 PSRAM。

## 4. 公共显示内存

显示层没有常驻 RGB565 整帧缓冲。两块 288×32 RGB565 条带各 18,432 字节，合计
36 KiB，固定放在片内 DMA 内存。核 1 一边 DMA 推第 N 条，一边生成第 N+1 条。

各模拟器提交给显示层的源数据不要求 DMA capable，因为条带回调会先把它转换进上述
内部缓冲。这允许 GB/GBC 和 SNES 把“只供 CPU 读写”的大块源缓冲放进 PSRAM。

## 5. 各模拟器的运行时布局

### 5.1 NES

| 对象 | 大小 | 位置 | 原因 |
|---|---:|---|---|
| 两块 nofrendo `vidbuf` | 2 × 65,280 B，约 127.5 KiB | 片内 SRAM | PPU 最热数据；放 PSRAM 时渲染从约 2 ms 涨到 8.5 ms |
| ROM | 随卡带变化 | Flash mmap | `rom_loadmem` 只保存指针，不复制 |
| 显示条带 | 36 KiB | 片内 DMA SRAM | LCD DMA 硬约束 |

NES 适配层没有强制申请大块 PSRAM。nofrendo 自身若做大于 16 KiB 的普通 heap 分配，
仍可能按全局策略落入 PSRAM，但 NES 的关键视频路径不依赖它。

### 5.2 GB/GBC

| 对象 | 大小 | 位置 | 原因 |
|---|---:|---|---|
| 两块 160×144 RGB565 源帧 | 2 × 46,080 B = 90 KiB | 强制 PSRAM | 不参与 DMA，核 0 写、核 1 读 |
| `hw.rambanks` | 32 KiB | 通常 PSRAM | 被模拟主 RAM |
| `hw.vbanks` | 16 KiB | 通常优先片内 | 正好不大于 16 KiB 阈值 |
| 卡带 RAM | 8–128 KiB | 随大小和 allocator 决定 | 大于 16 KiB 时通常优先 PSRAM |
| ROM | 随卡带变化 | Flash mmap | bank 指针直接指向只读镜像，不复制 |

所以 GB/GBC 明确占用的 PSRAM 下限约为 90 KiB；实际值还要加卡带 RAM 和普通 heap。

### 5.3 SNES，`SNES_MEM_PROFILE=0`

这是当前最依赖 PSRAM 的路径。下表列大块对象；不包含 FreeRTOS/驱动、allocator 元数据、
小对象和大小随声音核心状态变化的缓冲。

| 对象 | 大小 | 分配方式 | 当前预期位置 |
|---|---:|---|---|
| `GFX.Screen` | 122,368 B，119.5 KiB | 明确先要 `INTERNAL`，失败回退 | 片内 SRAM |
| `s_present` 推屏影子 | 122,368 B，119.5 KiB | 明确 `SPIRAM` | PSRAM |
| `GFX.SubScreen` | 122,368 B | 明确 `SPIRAM` | PSRAM |
| `GFX.ZBuffer` | 61,184 B | 明确 `SPIRAM` | PSRAM |
| `GFX.SubZBuffer` | 61,184 B | 明确 `SPIRAM` | PSRAM |
| WRAM / `Memory.RAM` | 128 KiB | 普通 `malloc` | 通常 PSRAM |
| `Memory.SRAM` | 64 KiB | 普通 `malloc` | 通常 PSRAM；冷数据，留在这里合理 |
| `Memory.VRAM` | 64 KiB | 普通 `malloc` | 通常 PSRAM；尚未做内部 SRAM 对照实验 |
| `Memory.FillRAM` | 32 KiB | 普通 `malloc` | 通常 PSRAM |
| `IPPU.TileCache` | 512 KiB | 普通 `calloc` | 通常 PSRAM |
| `IAPU.RAM` | 64 KiB | 普通 `malloc` | 通常 PSRAM |
| `GFX.ZERO` | 128 KiB | 普通 `malloc` | 通常 PSRAM |
| 最终 ROM 副本 | `rom_size + 66,048 B` | 明确 `SPIRAM` | PSRAM |
| 立体声音频包 | 1,920 B | 明确 `INTERNAL` | 片内 SRAM |

`SubScreen + ZBuffer + SubZBuffer` 合计 244,736 字节，即 239 KiB（十进制约 245 kB）。
ROM 多出的 66,048 字节由 64 KiB 映射余量和 512 字节头部余量组成。

#### SNES 为什么启动时几乎吃满 8 MiB

`S9xInitMemory()` 不知道实际 ROM 大小，会先尝试申请：

```text
6 MiB ROM 内容 + 64 KiB 映射余量 + 512 B 头部余量 = 6,357,504 B
```

当前调用顺序在释放它之前还会初始化 APU 和 GFX。把这块临时 ROM 与上表中已知、按
当前策略应落入 PSRAM 的大块相加，启动阶段的已知大块合计是：

```text
7,740,416 B = 7.38 MiB
```

这不是串口实测峰值，也不包括 allocator 元数据、声音环形缓冲和其他小对象；另一方面，
普通 `malloc` 只有位置偏好而非硬保证，少量对象可能回退内部 SRAM。因此 7.38 MiB 是
当前布局的静态模型值，不应当作实机精确峰值。它仍足以解释为什么 SNES 启动阶段确实
需要 8 MiB PSRAM，而不只是“启用了但没用”。

初始化结束后，适配层释放临时 6 MiB 缓冲，改为正好容纳所选 ROM 的副本。按同一组
已知大块计算，稳定期 PSRAM 模型值约为：

| ROM 内容大小 | 已知大块合计 |
|---:|---:|
| 512 KiB | 1,973,248 B = 1.88 MiB |
| 2 MiB | 3,546,112 B = 3.38 MiB |

仍应把这两个数字理解为静态模型值，不是精确的运行时总占用。

## 6. 已验证的性能结论与尚未验证的假设

Super Mario World 的现有对照结果：

| 改动 | 模拟帧率 |
|---|---:|
| 帧缓冲在 PSRAM、同步推屏 | 38–39 fps |
| `GFX.Screen` 挪入片内 SRAM | 43 fps |
| 内部 `GFX.Screen` + PSRAM 影子异步推屏 + 正确音频产样 | 约 45 fps |
| WRAM 挪入片内 SRAM、`GFX.Screen` 退回 PSRAM | 42–44 fps，已回退 |

这些数据证明当前机器上的片内/片外落点会显著影响性能，也证明“用 WRAM 替换内部
`GFX.Screen`”没有收益。它们没有证明 VRAM 方案无效：仓库可见历史里没有把 64 KiB
VRAM 挪入片内 SRAM 的对照实验。

但 `GFX.Screen` 119.5 KiB + VRAM 64 KiB 已超过约 179 KiB 的内部预算，所以 VRAM
实验大概率必须把 `GFX.Screen` 退回 PSRAM。渲染器会直接读 VRAM 中的 tilemap，并在
cache miss 时从 VRAM 转换图块；转换后又会频繁读取 512 KiB `IPPU.TileCache`。因此
“VRAM 值得测试”是合理假设，“VRAM 就是渲染耗时的主要来源”则不是已有结论。

`Memory.SRAM` 是冷的存档数据，留在 PSRAM 是明确选择；把它搬进稀缺的内部 SRAM 没有
已知收益。

## 7. 如何在板上核对

开机时 `main.c` 会打印物理资源：

```text
Flash     : 16 MB
PSRAM     : 8192 KB
内部空闲  : ... KB
```

GB/GBC 和 SNES 在完成主要分配后还会打印：

```text
内部 RAM 剩余 ... KB，PSRAM 剩余 ... KB
```

静态表只能解释分配意图和模型值；板上日志才是当前 ROM、当前 ESP-IDF allocator 和
当前驱动组合下的实际值。需要精确峰值时，应在 SNES 启动的以下四个位置分别记录
`heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` 和
`heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)`：

1. `alloc_buffers()` 之后；
2. `S9xInitMemory()` 之后；
3. `S9xInitAPU()` / `S9xInitGFX()` 之后；
4. `install_rom()` 之后。
