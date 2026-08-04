# nofrendo NES 模拟器核心
#
# 源码取自 retro-go（GPL v2，见 COPYING / CREDITS），未做修改。
# 与框架的粘合全在 rg_system.h 这一个垫片文件里。

idf_component_register(
    SRC_DIRS    "." "nes" "mappers"
    INCLUDE_DIRS "."
    REQUIRES    esp_common esp_rom
)

# RETRO_GO 必须是 PUBLIC：main 里 include nofrendo.h 会连带 include nes/utils.h，
# 两边对这个宏的取值必须一致，否则 IRAM_ATTR 的定义会打架。
target_compile_definitions(${COMPONENT_LIB} PUBLIC RETRO_GO)

# -O3 是模拟器性能的关键。那几个 -Wno- 是上游代码本来就有的老式写法，
# 不是我们引入的问题，屏蔽掉以免淹没自己代码的警告。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    -Wno-array-bounds
    -Wno-format
    -Wno-error=format
    -Wno-unused-but-set-variable
    -Wno-char-subscripts
)
