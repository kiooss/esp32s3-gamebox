#!/usr/bin/env python3
"""从 GNU Unifont .hex(.gz) 生成菜单用的 16x16 中文字形表（GB2312 全集）。

字形来自 GNU Unifont 17.0.04；Unifont 采用 SIL OFL 1.1，或 GPL v2+
（带字体嵌入例外）。

用法：
    python3 tools/gen_menu_font.py unifont_all-17.0.04.hex.gz main/menu_font.c

源文件不入库（解开 8 MB），需要时从
https://ftp.gnu.org/gnu/unifont/unifont-17.0.04/unifont_all-17.0.04.hex.gz 取。
只在换字符集时才需要重跑本脚本——生成的 menu_font.c 是入库的。
"""

import gzip
from pathlib import Path
import sys


# 以前这里是一张手写的「菜单实际出现的汉字」表，只固化用到的那 106 个。
# 问题是游戏名由 roms/ 里的文件名决定：每加一个中文名游戏就要回来补字，
# 忘了也不报错，只是菜单上显示成 `?`——隐性负担，迟早踩。
#
# 改成一次收全 GB2312 的 6763 个汉字（一级 3755 + 二级 3008）。代价是固件
# 多约 225 KB（每字形 34 字节：uint16 码点 + 32 字节点阵），app 分区当时
# 还剩 548 KiB，装得下。这个项目里 flash 是最不紧张的资源，拿它换掉一类
# 「会忘、忘了还不报错」的维护负担很划算。
def gb2312_codepoints():
    """GB2312 的 6763 个汉字。按区位 0xB0A1~0xF7FE 遍历解码得到，
    比维护一张字表可靠——字表抄错了不会有人发现。"""
    out = set()
    for high in range(0xB0, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                out.add(ord(bytes([high, low]).decode("gb2312")))
            except UnicodeDecodeError:
                pass                    # 区位表里的空洞
    return out


# 游戏名偶尔会撞上 GB2312 之外的字（繁体、日文假名）。往这里加单个字即可，
# 比整段换成 GBK（21886 字、约 740 KB）划算得多。
EXTRA_TEXT = ""


def read_hex(path):
    wanted = gb2312_codepoints() | {ord(ch) for ch in EXTRA_TEXT if ord(ch) > 0x7F}
    found = {}
    opener = gzip.open if str(path).endswith(".gz") else open
    with opener(path, "rt", encoding="ascii") as fh:
        for line in fh:
            code_hex, sep, bitmap_hex = line.strip().partition(":")
            if not sep:
                continue
            codepoint = int(code_hex, 16)
            if codepoint not in wanted:
                continue
            bitmap = bytes.fromhex(bitmap_hex)
            if len(bitmap) != 32:
                raise SystemExit(
                    "U+%04X 不是 16x16 字形（%d 字节）" %
                    (codepoint, len(bitmap)))
            found[codepoint] = bitmap

    missing = wanted - found.keys()
    if missing:
        raise SystemExit("缺少字形: " + " ".join("U+%04X" % cp
                                                for cp in sorted(missing)))
    return found


def render(glyphs):
    lines = [
        "/* 此文件由 tools/gen_menu_font.py 生成，不要手改。",
        " * 字形来自 GNU Unifont 17.0.04：SIL OFL 1.1，或 GPL v2+ 字体嵌入例外。 */",
        "#include <stddef.h>",
        '#include "menu_font.h"',
        "",
        "typedef struct {",
        "    uint16_t codepoint;",
        "    uint8_t bitmap[32];     /* 16 行，每行高位在左 */",
        "} menu_glyph_t;",
        "",
        "static const menu_glyph_t GLYPHS[] = {",
    ]
    for codepoint, bitmap in sorted(glyphs.items()):
        values = ", ".join("0x%02X" % b for b in bitmap)
        lines.append("    { 0x%04X, { %s } }, /* %s */" %
                     (codepoint, values, chr(codepoint)))
    lines += [
        "};",
        "",
        "const uint8_t *menu_font_glyph(uint32_t codepoint)",
        "{",
        "    int lo = 0;",
        "    int hi = (int)(sizeof(GLYPHS) / sizeof(GLYPHS[0])) - 1;",
        "    while (lo <= hi) {",
        "        int mid = lo + (hi - lo) / 2;",
        "        uint32_t value = GLYPHS[mid].codepoint;",
        "        if (value == codepoint) return GLYPHS[mid].bitmap;",
        "        if (value < codepoint) lo = mid + 1;",
        "        else hi = mid - 1;",
        "    }",
        "    return NULL;",
        "}",
        "",
    ]
    return "\n".join(lines)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    glyphs = read_hex(Path(sys.argv[1]))
    output = Path(sys.argv[2])
    output.write_text(render(glyphs), encoding="utf-8")
    print("生成 %d 个中文点阵字形 -> %s (%.0f KB 源码，固件里约 %.0f KB)"
          % (len(glyphs), output, output.stat().st_size / 1024,
             len(glyphs) * 34 / 1024))


if __name__ == "__main__":
    main()
