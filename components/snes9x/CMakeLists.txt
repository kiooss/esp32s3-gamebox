# Snes9x 2005 —— SNES 模拟器核心
#
# 源码取自 retro-go 的 retro-core/components/snes9x（上游又来自
# libretro/snes9x2005），未做修改。粘合同样只有 rg_system.h 一个垫片。
#
# ⚠ 授权和 nofrendo/gnuboy 不同：src/LICENSE 是 Snes9x 自己的许可证，
#   明确禁止商业分发。见 README.gamebox.md。

idf_component_register(
    SRC_DIRS     "src"
    INCLUDE_DIRS "."
    REQUIRES     esp_common esp_rom
)

# RETRO_GO 让 src/port.h 去 include rg_system.h（本目录那份垫片）。
# 和 nofrendo 一样必须是 PUBLIC：main 里 include snes9x.h 会连带 port.h。
target_compile_definitions(${COMPONENT_LIB} PUBLIC RETRO_GO)

# 这几个 -D 是上游 retro-go 给 ESP32 定的，不是可选项：
#   RIGHTSHIFT_IS_SAR    —— 承诺有符号右移是算术移位，省掉每次移位的补符号分支
#   FAST_LSB_WORD_ACCESS —— 允许非对齐小端 16 位直读（Xtensa 支持）
#   NO_ZERO_LUT          —— 不生成那张查零表，省 RAM
#
# -O2 而不是 -O3：这份核心比 nofrendo 大一个量级（cpuops.c 73 KB、
# dsp.c 141 KB、gfx.c 101 KB），-O3 的循环展开会把 .text 撑爆 1 MB 的 app 分区。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -DRIGHTSHIFT_IS_SAR
    -DFAST_LSB_WORD_ACCESS
    -DNO_ZERO_LUT
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-array-bounds
    -Wno-implicit-fallthrough
    -Wno-unused-but-set-variable
    -Wno-char-subscripts
)
