#!/usr/bin/env python3
"""把 roms/ 下的 NES / GB / GBC / SNES 游戏打包进 flash 的 roms 分区。

镜像格式（小端，和 ESP32 一致）：

    偏移 0    magic  "GAMEBOX\\0"        8 字节
    偏移 8    count                      uint32，条目数
    偏移 12   目录项 x count，每项 52 字节：
                  name    char[40]       显示名，NUL 结尾
                  system  uint32         1=NES, 2=GB, 3=GBC, 4=SNES
                  offset  uint32         ROM 数据在**镜像**内的绝对偏移
                  size    uint32         ROM 字节数
    之后      各 ROM 数据，4 字节对齐

故意不用文件系统。这个格式能让固件直接 esp_partition_mmap 整个分区，
ROM 指针原样传给对应模拟器 —— 走 flash cache，零拷贝、不占 RAM。
SPIFFS 做不到这点（不能 mmap），得把 ROM 整份读进内存。

用法：
    python3 tools/pack_roms.py roms/ build/roms.bin
"""

import os
import re
import struct
import sys

MAGIC = b"GAMEBOX\0"
NAME_LEN = 40           # 含结尾 NUL，所以显示名最长 39 字节
ENTRY_FMT = "<%dsIII" % NAME_LEN
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)
HEADER_FMT = "<8sI"
HEADER_SIZE = struct.calcsize(HEADER_FMT)

assert ENTRY_SIZE == 52, ENTRY_SIZE

SYSTEM_NES = 1
SYSTEM_GB = 2
SYSTEM_GBC = 3
SYSTEM_SNES = 4
SYSTEM_NAMES = {
    SYSTEM_NES: "NES",
    SYSTEM_GB: "GB",
    SYSTEM_GBC: "GBC",
    SYSTEM_SNES: "SNES",
}

EXTENSIONS = (".nes", ".gb", ".gbc", ".sfc", ".smc")

# ROM 站惯例的标记：(U) (E) (Japan, USA) [!] [!p] (PRG0) 等等。
# 显示名里不需要，剥掉。
TAG_RE = re.compile(r"\s*[\(\[][^\)\]]*[\)\]]")
ORDER_RE = re.compile(r"^\d{2}_")


def display_name(filename):
    """`Super Mario Bros. (Japan, USA).nes` -> `Super Mario Bros.`"""
    stem = os.path.splitext(os.path.basename(filename))[0]
    name = ORDER_RE.sub("", TAG_RE.sub("", stem)).strip()
    if not name:                        # 整个名字都是标记的极端情况
        name = stem.strip()
    # 截断留一个字节给 NUL，但不能从 UTF-8 多字节字符中间切断，否则设备端
    # 会得到坏序列。菜单的中文子集按完整码点查字形。
    encoded = name.encode("utf-8", "replace")
    while len(encoded) > NAME_LEN - 1:
        name = name[:-1]
        encoded = name.encode("utf-8", "replace")
    return encoded


def ines_ok(data):
    """粗查是不是 iNES 文件。坏文件在这里挡掉，比烧进去再在板子上崩好。"""
    return len(data) > 16 and data[:4] == b"NES\x1a"


def gameboy_system(data):
    """校验 GB 头校验和，并按 CGB 标志区分 GB / GBC。"""
    if len(data) < 0x150 or len(data) % 0x4000 != 0:
        return None
    check = 0
    for value in data[0x134:0x14D]:
        check = (check - value - 1) & 0xFF
    if check != data[0x14D]:
        return None
    return SYSTEM_GBC if data[0x143] in (0x80, 0xC0) else SYSTEM_GB


def snes_strip_copier_header(data):
    """老式拷贝机会在文件头加 512 字节。剥掉再打包，省得板子上再判一次。"""
    if len(data) % 0x400 == 512:
        return data[512:]
    return data


def snes_ok(data):
    """SNES 没有 magic，只能查内部头：checksum ^ complement == 0xFFFF，
    且标题是可打印 ASCII。LoROM(0x7FC0) / HiROM(0xFFC0) 各试一次。
    和 main/rom_store.c 的 snes_header_ok() 是同一套判据，改一处要改两处。"""
    if len(data) < 0x20000:
        return None
    for base in (0x7FC0, 0xFFC0):
        if base + 0x20 > len(data):
            continue
        title = data[base:base + 21]
        if any(c != 0 and not (0x20 <= c <= 0x7E) for c in title):
            continue
        comp = int.from_bytes(data[base + 0x1C:base + 0x1E], "little")
        check = int.from_bytes(data[base + 0x1E:base + 0x20], "little")
        if check ^ comp == 0xFFFF:
            return SYSTEM_SNES
    return None


def roms_partition_size():
    """从 partitions.csv 读 roms 分区的容量（字节）。读不到就返回 None 不拦。

    故意不写死：分区大小改过一次（8 MB -> 14 MB），而这里的常量没跟着改，
    结果是构建在打包这步失败、报的还是过期的「超过 8 MB」。宁可现读。
    大小列支持 partitions.csv 用的十六进制，也支持 IDF 允许的 1M / 24K 写法。
    """
    csv_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            os.pardir, "partitions.csv")
    try:
        with open(csv_path, encoding="utf-8") as fh:
            for line in fh:
                line = line.split("#", 1)[0]
                cols = [c.strip() for c in line.split(",")]
                if len(cols) < 5 or cols[0] != "roms":
                    continue
                size = cols[4].upper()
                scale = {"K": 1024, "M": 1024 * 1024}.get(size[-1:], 1)
                if scale != 1:
                    size = size[:-1]
                return int(size, 0) * scale
    except (OSError, ValueError):
        pass
    return None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rom_dir, out_path = sys.argv[1], sys.argv[2]

    paths = sorted(
        os.path.join(rom_dir, f)
        for f in os.listdir(rom_dir)
        if f.lower().endswith(EXTENSIONS)
    )
    if not paths:
        sys.exit("在 %s 里没找到 %s 文件" % (rom_dir, "/".join(EXTENSIONS)))

    roms = []
    for p in paths:
        with open(p, "rb") as fh:
            data = fh.read()
        ext = os.path.splitext(p)[1].lower()
        system = SYSTEM_NES if ext == ".nes" and ines_ok(data) else None
        if ext in (".gb", ".gbc"):
            system = gameboy_system(data)
        if ext in (".sfc", ".smc"):
            data = snes_strip_copier_header(data)
            system = snes_ok(data)
        if system is None:
            print("  跳过（ROM 头无效）: %s" % os.path.basename(p))
            continue
        roms.append((display_name(p), data, system))

    if not roms:
        sys.exit("没有一个文件通过 ROM 头校验")

    # 数据区从目录表之后开始，且各 ROM 4 字节对齐。
    # 先算好每个 ROM 的偏移，再一次写出去。
    cursor = HEADER_SIZE + ENTRY_SIZE * len(roms)
    entries, blobs = [], []
    for name, data, system in roms:
        pad = (-cursor) % 4
        if pad:
            blobs.append(b"\0" * pad)
            cursor += pad
        entries.append(struct.pack(ENTRY_FMT, name, system, cursor, len(data)))
        blobs.append(data)
        cursor += len(data)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as fh:
        fh.write(struct.pack(HEADER_FMT, MAGIC, len(roms)))
        for e in entries:
            fh.write(e)
        for b in blobs:
            fh.write(b)

    total = os.path.getsize(out_path)
    print("打包 %d 个 ROM -> %s (%.0f KB)" % (len(roms), out_path, total / 1024))
    for name, data, system in roms:
        print("  %-4s %-36s %6.0f KB" % (
            SYSTEM_NAMES[system], name.decode("utf-8", "replace"),
            len(data) / 1024))

    # 超了 esptool 也会报错，但在这里说清楚更好定位。
    # 容量从 partitions.csv 现读而不是写死：以前这里硬编码 8 MB，
    # 分区扩到 14 MB 之后脚本仍按 8 MB 拦，白炸一次构建。
    limit = roms_partition_size()
    if limit and total > limit:
        sys.exit("镜像 %.1f MB 超过 roms 分区的 %.0f MB —— "
                 "减少游戏或把 partitions.csv 里的分区改大"
                 % (total / 1048576, limit / 1048576))


if __name__ == "__main__":
    main()
