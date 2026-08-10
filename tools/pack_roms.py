#!/usr/bin/env python3
"""把 roms/*.nes 打包成一个镜像，烧进 flash 的 roms 分区。

镜像格式（小端，和 ESP32 一致）：

    偏移 0    magic  "NESROMS\\0"        8 字节
    偏移 8    count                      uint32，条目数
    偏移 12   目录项 x count，每项 48 字节：
                  name    char[40]       显示名，NUL 结尾
                  offset  uint32         ROM 数据在**镜像**内的绝对偏移
                  size    uint32         ROM 字节数
    之后      各 ROM 数据，4 字节对齐

故意不用文件系统。这个格式能让固件直接 esp_partition_mmap 整个分区，
ROM 指针原样传给 nofrendo 的 rom_loadmem —— 走 flash cache，零拷贝、
不占 RAM。SPIFFS 做不到这点（不能 mmap），得把 ROM 整份读进内存。

用法：
    python3 tools/pack_roms.py roms/ build/roms.bin
"""

import os
import re
import struct
import sys

MAGIC = b"NESROMS\0"
NAME_LEN = 40           # 含结尾 NUL，所以显示名最长 39 字节
ENTRY_FMT = "<%dsII" % NAME_LEN
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)
HEADER_FMT = "<8sI"
HEADER_SIZE = struct.calcsize(HEADER_FMT)

assert ENTRY_SIZE == 48, ENTRY_SIZE

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


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rom_dir, out_path = sys.argv[1], sys.argv[2]

    paths = sorted(
        os.path.join(rom_dir, f)
        for f in os.listdir(rom_dir)
        if f.lower().endswith(".nes")
    )
    if not paths:
        sys.exit("在 %s 里没找到 .nes 文件" % rom_dir)

    roms = []
    for p in paths:
        with open(p, "rb") as fh:
            data = fh.read()
        if not ines_ok(data):
            print("  跳过（不是 iNES）: %s" % os.path.basename(p))
            continue
        roms.append((display_name(p), data))

    if not roms:
        sys.exit("没有合法的 iNES 文件")

    # 数据区从目录表之后开始，且各 ROM 4 字节对齐。
    # 先算好每个 ROM 的偏移，再一次写出去。
    cursor = HEADER_SIZE + ENTRY_SIZE * len(roms)
    entries, blobs = [], []
    for name, data in roms:
        pad = (-cursor) % 4
        if pad:
            blobs.append(b"\0" * pad)
            cursor += pad
        entries.append(struct.pack(ENTRY_FMT, name, cursor, len(data)))
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
    for name, data in roms:
        print("  %-40s %6.0f KB" % (name.decode("utf-8", "replace"),
                                    len(data) / 1024))

    # 分区是 8 MB（见 partitions.csv）。超了 esptool 会报错，
    # 但在这里说清楚更好定位。
    limit = 8 * 1024 * 1024
    if total > limit:
        sys.exit("镜像 %.1f MB 超过 roms 分区的 8 MB —— "
                 "减少游戏或把 partitions.csv 里的分区改大" % (total / 1048576))


if __name__ == "__main__":
    main()
