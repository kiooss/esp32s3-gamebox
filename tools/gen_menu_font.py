#!/usr/bin/env python3
"""从 GNU Unifont .hex(.gz) 生成游戏菜单所需的 16x16 中文字形子集。

只固化菜单实际出现的汉字，避免把完整 CJK 字库塞进固件。生成的字形来自
GNU Unifont 17.0.04；Unifont 采用 SIL OFL 1.1，或 GPL v2+（带字体嵌入例外）。

用法：
    python3 tools/gen_menu_font.py unifont_all-17.0.04.hex.gz main/menu_font.c
"""

import gzip
from pathlib import Path
import sys


MENU_TEXT = """
游戏选择上下左右开始翻页
超级马里奥魂斗罗塞尔达传说俄罗斯方块洛克人星之卡比忍者龙剑
恶魔城松鼠大作战唐老鸭梦冒险坦克大战双截龙神龟热血物语炸弹
泡泡雪人兄弟气球敲冰越野摩托沙罗曼蛇赤影战士兵蜂怪历记医生
蝙蝠侠勇者斗恶岛
"""


def read_hex(path):
    wanted = {ord(ch) for ch in MENU_TEXT if ord(ch) > 0x7F}
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
    print("生成 %d 个中文点阵字形 -> %s" % (len(glyphs), output))


if __name__ == "__main__":
    main()
