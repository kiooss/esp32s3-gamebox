#pragma once

#include <stdint.h>

/* 返回 16x16 行优先点阵（每行 2 字节，高位在左）；字库没有该字符时返回 NULL。 */
const uint8_t *menu_font_glyph(uint32_t codepoint);
