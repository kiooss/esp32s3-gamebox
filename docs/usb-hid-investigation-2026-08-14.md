# USB HID 手柄排查记录（2026-08-14，暂停）

## 结论

USB HID Host 软件和手柄报告解析已经接入，但当前这块开发板的原生 USB 主机链路
无法稳定完成设备复位。整体断电重启、换设备和换 OTG 转接头后，串口仍稳定出现：

```text
HUB: Root port reset failed
```

这个错误发生在读取 VID/PID、HID Report Descriptor 和按键报告之前，因此当前无反应的
直接原因不是游戏菜单、按键映射或 Retro-Go/noFrendo 输入层，而是更底层的 USB 枚举失败。
现阶段暂停继续投入，日常操作继续使用 JoyStick Shield 或串口键盘。

## 硬件连接

- 丝印 `COM` 的 Type-C 口接电脑，负责开发板供电、烧录和串口监视。
- 丝印 `USB` 的 Type-C 口作为 USB OTG Host，使用 USB-A 母转 Type-C 公 OTG 转接头。
- 手柄是 USB-A 插头，通过上述转接头插入开发板。
- 板背 `USB-OTG` 两个焊盘已经用焊锡短接；照片中焊锡覆盖两侧焊盘，未见明显相邻短路。
- 原生 USB 数据脚为 GPIO19（D-）和 GPIO20（D+）；JoyStick Shield 使用 GPIO1/2、
  GPIO7/8 等其他引脚，两者不冲突。

## 已确认的事实

### 手柄和 OTG 转接头在 Mac 上可用

同一只手柄经过同一个 OTG 转接头连接 Mac 后可以枚举：

| 项目 | 实测值 |
|---|---|
| 设备名 | `USB Joystick` |
| 厂商 | Microntek |
| VID:PID | `0079:0006` |
| USB 速度 | Low Speed，1.5 Mbit/s |
| 状态 | Active |

这组对照实验说明手柄的 USB 线包含数据线，手柄和转接头本身至少能够建立 USB 通信。

### 开发板 OTG 口存在供电

第一只测试鼠标不亮，换另一只鼠标后灯可以点亮。这只能证明 OTG 口存在 VBUS，不能证明
负载下电压稳定或处于 USB 要求的范围。排查期间没有用万用表记录实际 VBUS 电压。

### 开发板能感知设备接入，但无法完成复位

多次插拔、换鼠标、换 OTG 转接头以及整体断电后，均没有稳定进入 HID 设备就绪状态。
观察过的底层错误包括：

```text
HUB: Root port reset failed
HUB: Failed to issue root port reset
USBH: Dev 0 EP 0 Error
ENUM: Bad transfer status 1: CHECK_ADDR
ENUM: [0:0] CHECK_ADDR FAILED
```

其中一次已经走到默认控制端点和地址检查，说明 D+/D- 并非始终完全开路，但链路质量或
供电条件不足以可靠完成枚举。

2026-08-14 最后一次复测是完整冷启动，启动原因明确为 `POWERON`。固件正常启动并打印
`USB HID 手柄已启用`，设备接入后约数秒仍报 `HUB: Root port reset failed`。

## 软件实现和实验

项目使用 ESP-IDF 5.4 的 USB Host HID 组件，输入在菜单和各模拟器中与 JoyStick、串口键盘
按位合并。GPIO19/20 未被项目中其他功能复用。

排查早期曾偶发收到 `0079:0006` 的 8 字节中断报告，并记录出以下格式：

```text
静止：7F 7F 00 80 80 0F 00 00
byte 0：00=左，7F=中，FF=右
byte 1：00=上，7F=中，FF=下
byte 5：面键
byte 6：0x10=SELECT，0x20=START
```

基于这些实测值，当前工作区的 `main/input_usb.c` 对 `0079:0006` 增加了固定映射，并跳过
这款设备曾经超时的 HID Report Descriptor 请求。该版本编译、烧录成功，但固定映射只有在
USB 枚举完成并收到报告后才会生效，无法修复 Root Port Reset 失败。

还试过延长 USB Host 的复位时序：

- Reset Recovery 由默认值改为 100 ms；
- Reset Hold / Recovery 都改为 500 ms。

两组实验均未改善，最终固件已恢复 ESP-IDF 默认的 30 ms / 30 ms 时序。

## 已排除和未确认项

已基本排除：

- 手柄没有数据线：同一连接组合可被 Mac 识别；
- 单一 OTG 转接头损坏：更换转接头后症状相同；
- 只是不支持该手柄的 HID 映射：失败发生在获得 HID 报告之前；
- GPIO19/20 被显示、音频或 JoyStick 占用：项目未将这两个脚用于其他外设；
- 常规复位时序太短：100 ms 和 500 ms 实验均失败；
- 仅由热启动状态导致：完整断电冷启动后仍失败。

尚未确认：

- OTG 口 VBUS 在鼠标/手柄负载下是否稳定在约 5 V；
- Type-C 母座的 D+/D- 引脚焊接、触点和板上走线是否可靠；
- GPIO19/20 到 Type-C 口之间的通断及对地/互相阻值；
- 是否存在只能用示波器或 USB 分析仪观察到的信号完整性问题。

因此不能把根因定论为“电压不够”，也不能只凭鼠标灯亮排除供电问题。当前更准确的结论是：
**开发板侧原生 USB 的数据链路或 VBUS 质量存在硬件层面的高度嫌疑。**

## 如果以后恢复排查

按以下顺序做，避免继续盲改软件：

1. 保持 `COM` 口供电，在 OTG 设备已连接时测量 USB VBUS 对 GND；记录静态值和插入瞬间
   是否跌落。低于 4.75 V 或明显波动时先修供电路径。
2. 完全断电后，用电阻/通断档核对 Type-C D- 到 GPIO19、D+ 到 GPIO20；同时检查 D+/D-
   互相之间及各自对地是否异常低阻。
3. 若 VBUS 和线路通断正常，放大检查或补焊 Type-C 母座的数据引脚；优先交给有细间距
   焊接经验的人处理。
4. 修复硬件后先用鼠标验证串口出现设备打开/接口信息，再接 `0079:0006` 手柄验证方向、
   A/B、SELECT 和 START。
5. 只有枚举稳定后，才继续调整 HID 描述符解析或固定按键映射。

不要在未确认供电方案前，直接从 GPIO19/20 飞线制作带外部 5 V 的 USB-A 母座；错误的
VBUS 供电方向可能导致两路 5 V 电源互相回灌。
