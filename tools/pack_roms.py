#!/usr/bin/env python3
"""把 roms/ 下的 NES / GB / GBC / SNES / Genesis 游戏打包进 roms 分区。

裸 ROM 和 .zip 都收：zip 里恰好有一个可识别 ROM 时自动取出来（显示名用
zip 内部的文件名），否则打一行提示跳过。

镜像格式（小端，和 ESP32 一致）：

    偏移 0    magic  "GBOXDFL\\0"        8 字节
    偏移 8    count                      uint32，条目数
    偏移 12   目录项 x count，每项 64 字节：
                  name    char[40]       显示名，NUL 结尾
                  system  uint32         1=NES, 2=GB, 3=GBC, 4=SNES, 5=Genesis
                  codec   uint32         0=原样，1=raw DEFLATE
                  offset  uint32         存储数据在**镜像**内的绝对偏移
                  stored  uint32         压缩后的存储字节数
                  raw     uint32         解压后的 ROM 字节数
                  crc32   uint32         原始 ROM 的 CRC32
    之后      各 ROM 独立压缩的数据，4 字节对齐

故意不用 ZIP 文件系统：这里已有一张可随机访问的目录表，再套 ZIP 中央目录只会
重复。每个游戏独立压缩，菜单无需解压整包；选中后才把一个 ROM 解到 PSRAM。
如果某个文件压缩后没有变小，则保留原样，仍可直接 mmap 零拷贝。

用法：
    python3 tools/pack_roms.py roms/ build/roms.bin
"""

import os
import re
import struct
import sys
import zipfile
import zlib

MAGIC = b"GBOXDFL\0"
NAME_LEN = 40           # 含结尾 NUL，所以显示名最长 39 字节
ENTRY_FMT = "<%dsIIIIII" % NAME_LEN
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)
HEADER_FMT = "<8sI"
HEADER_SIZE = struct.calcsize(HEADER_FMT)

assert ENTRY_SIZE == 64, ENTRY_SIZE

CODEC_RAW = 0
CODEC_DEFLATE = 1

SYSTEM_NES = 1
SYSTEM_GB = 2
SYSTEM_GBC = 3
SYSTEM_SNES = 4
SYSTEM_GENESIS = 5
SYSTEM_NAMES = {
    SYSTEM_NES: "NES",
    SYSTEM_GB: "GB",
    SYSTEM_GBC: "GBC",
    SYSTEM_SNES: "SNES",
    SYSTEM_GENESIS: "MD",
}

EXTENSIONS = (".nes", ".gb", ".gbc", ".sfc", ".smc", ".md", ".bin")

# .zip 会被拆开取出里面的 ROM；其余的只认得出来、提示一句让人先解压。
# 之所以要认这些扩展名而不是直接无视：扫描阶段按扩展名过滤时，不认识的
# 文件是**一点提示都没有**地消失的，人只会看到菜单里少一个游戏，根本
# 想不到是扩展名的问题。宁可多打一行。
ARCHIVE_EXTENSIONS = (".zip", ".7z", ".rar", ".gz", ".tar", ".tgz")

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


def genesis_ok(data):
    """只接收标准线性卡带镜像；0x100 的 SEGA 头比扩展名可靠。"""
    return len(data) >= 0x200 and data[0x100:0x104] == b"SEGA"


def compress_rom(data):
    """返回 (codec, payload)。

    wbits=-15 生成没有 zlib/ZIP 外壳的 raw DEFLATE，板上可以直接交给
    ESP-IDF 自带的 tinfl。只在确实变小时采用，避免小 ROM 被压缩头反向撑大。
    """
    compressor = zlib.compressobj(level=9, wbits=-15)
    packed = compressor.compress(data) + compressor.flush()
    return ((CODEC_DEFLATE, packed) if len(packed) < len(data)
            else (CODEC_RAW, data))


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


# roms/ 按平台分了子目录（nes/ gb/ gbc/ snes/ md/），所以要递归扫。但「不要的
# 游戏」一直是挪进 removed-YYYYMMDD/ 而不是真删——递归之后那些会被重新收进来，
# 所以这类目录整棵跳过。前缀 `_` 和 `.` 一并跳，留作临时存放的通用写法。
SKIP_DIR_RE = re.compile(r"^(removed|_|\.)")


def find_roms(rom_dir):
    paths = []
    for dirpath, dirnames, filenames in os.walk(rom_dir):
        dirnames[:] = [d for d in dirnames if not SKIP_DIR_RE.match(d)]
        paths += [os.path.join(dirpath, f) for f in filenames
                  if f.lower().endswith(EXTENSIONS + ARCHIVE_EXTENSIONS)]
    # 排序键用文件名而不是完整路径：分不分子目录、怎么分，菜单里的顺序都不变。
    return sorted(paths, key=lambda p: (os.path.basename(p), p))


# macOS 压缩时会塞进伴生文件，它们的扩展名和真 ROM 一模一样
# （__MACOSX/._Foo.sfc），不滤掉就会被算成第二个候选、整个 zip 被判定
# 「有 2 个 ROM」而跳过。
def is_macos_junk(name):
    return name.startswith("__MACOSX/") or os.path.basename(name).startswith("._")


def read_zip_rom(path):
    """从 zip 里取出唯一那个 ROM，返回 (内部文件名, 数据)；取不到返回 (None, None)。

    显示名用 zip **内部**的文件名而不是 zip 自己的名字：库里的 zip 常是
    `Contra_ The Alien Wars.zip` 这种——那个下划线是 macOS 把 `:` 转义来的，
    内部名 `Contra - The Alien Wars (USA) (SGB Enhanced).gb` 干净得多，
    而且这样 zip 和裸 ROM 走的是同一条 display_name() 路径。
    """
    shown = os.path.basename(path)
    try:
        with zipfile.ZipFile(path) as zf:
            names = [n for n in zf.namelist()
                     if not n.endswith("/")
                     and not is_macos_junk(n)
                     and n.lower().endswith(EXTENSIONS)]
            if len(names) != 1:
                why = ("里面没有可识别的 ROM" if not names else
                       "里面有 %d 个 ROM，不猜用哪个" % len(names))
                print("  忽略（%s）: %s" % (why, shown))
                return None, None
            return os.path.basename(names[0]), zf.read(names[0])
    except (zipfile.BadZipFile, OSError) as exc:
        print("  忽略（打不开: %s）: %s" % (exc, shown))
        return None, None


def read_rom(path):
    """把一个路径读成 (用于判类型和显示的文件名, 数据)；读不了返回 (None, None)。"""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".zip":
        return read_zip_rom(path)
    if ext in ARCHIVE_EXTENSIONS:
        print("  忽略（%s 压缩包，请先解压）: %s" % (ext, os.path.basename(path)))
        return None, None
    with open(path, "rb") as fh:
        return os.path.basename(path), fh.read()


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rom_dir, out_path = sys.argv[1], sys.argv[2]

    paths = find_roms(rom_dir)
    if not paths:
        sys.exit("在 %s 里没找到 %s 文件" % (rom_dir, "/".join(EXTENSIONS)))

    roms = []
    for p in paths:
        name, data = read_rom(p)
        if data is None:
            continue        # read_rom 已经把原因打出来了
        ext = os.path.splitext(name)[1].lower()
        system = SYSTEM_NES if ext == ".nes" and ines_ok(data) else None
        if ext in (".gb", ".gbc"):
            system = gameboy_system(data)
        if ext in (".sfc", ".smc"):
            data = snes_strip_copier_header(data)
            system = snes_ok(data)
        if ext in (".md", ".bin") and genesis_ok(data):
            system = SYSTEM_GENESIS
        if system is None:
            print("  跳过（ROM 头无效）: %s" % name)
            continue
        codec, payload = compress_rom(data)
        roms.append((display_name(name), data, payload, system, codec))

    if not roms:
        sys.exit("没有一个文件通过 ROM 头校验")

    # 按平台分组，同类游戏在菜单里排在一起；list.sort 是稳定排序，组内
    # 顺序仍是上面按文件名排出来的那个顺序（NES 靠文件名前的两位数字，
    # 其它平台靠字母序），只是把各平台的段落聚拢，不打乱组内已有的顺序。
    roms.sort(key=lambda r: r[3])

    # 数据区从目录表之后开始，且各 ROM 4 字节对齐。
    # 先算好每个 ROM 的偏移，再一次写出去。
    cursor = HEADER_SIZE + ENTRY_SIZE * len(roms)
    entries, blobs = [], []
    for name, data, payload, system, codec in roms:
        pad = (-cursor) % 4
        if pad:
            blobs.append(b"\0" * pad)
            cursor += pad
        entries.append(struct.pack(
            ENTRY_FMT, name, system, codec, cursor, len(payload), len(data),
            zlib.crc32(data) & 0xFFFFFFFF))
        blobs.append(payload)
        cursor += len(payload)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as fh:
        fh.write(struct.pack(HEADER_FMT, MAGIC, len(roms)))
        for e in entries:
            fh.write(e)
        for b in blobs:
            fh.write(b)

    total = os.path.getsize(out_path)
    print("打包 %d 个 ROM -> %s (%.0f KB)" % (len(roms), out_path, total / 1024))
    raw_total = sum(len(data) for _, data, _, _, _ in roms)
    stored_total = sum(len(payload) for _, _, payload, _, _ in roms)
    for name, data, payload, system, codec in roms:
        print("  %-4s %-36s %6.0f -> %6.0f KB  %s" % (
            SYSTEM_NAMES[system], name.decode("utf-8", "replace"),
            len(data) / 1024, len(payload) / 1024,
            "deflate" if codec == CODEC_DEFLATE else "raw"))
    print("ROM 数据 %.2f -> %.2f MiB，节省 %.2f MiB（%.1f%%）" % (
        raw_total / 1048576, stored_total / 1048576,
        (raw_total - stored_total) / 1048576,
        (raw_total - stored_total) * 100 / raw_total))

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
