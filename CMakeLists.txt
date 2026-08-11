cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3-gamebox)

# ---- ROM 分区镜像 ----
#
# roms/ 下的 NES / GB / GBC 游戏打包成一个镜像，烧进 partitions.csv
# 里那个 roms 分区。
# 格式和理由见 tools/pack_roms.py 顶部。
#
# 挂在 app 的依赖上，所以 `idf.py build` 会顺带生成镜像。烧它要单独一条命令：
#   idf.py flash-roms        （下面注册的 target）
# 或者手工：
#   esptool.py write_flash 0x110000 build/roms.bin
#
# 没有做成 `idf.py flash` 的一部分 —— ROM 镜像 6 MB 多，烧一次要几分钟，
# 每次改代码都重烧是白等。加了游戏才需要跑一次 flash-roms。
set(ROMS_IMAGE "${CMAKE_BINARY_DIR}/roms.bin")
# 三种扩展名都必须成为依赖；否则只替换 GB/GBC ROM 时，已有的 roms.bin
# 会被误判为最新，flash-roms 烧进去的仍是旧镜像。
file(GLOB ROM_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/roms/*.nes"
    "${CMAKE_SOURCE_DIR}/roms/*.gb"
    "${CMAKE_SOURCE_DIR}/roms/*.gbc")

add_custom_command(
    OUTPUT "${ROMS_IMAGE}"
    COMMAND ${CMAKE_COMMAND} -E echo "打包 ROM 镜像..."
    COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/pack_roms.py"
                    "${CMAKE_SOURCE_DIR}/roms" "${ROMS_IMAGE}"
    DEPENDS ${ROM_FILES} "${CMAKE_SOURCE_DIR}/tools/pack_roms.py"
    COMMENT "生成 ROM 分区镜像"
    VERBATIM)

add_custom_target(roms_image ALL DEPENDS "${ROMS_IMAGE}")

# roms 分区在 partitions.csv 里的偏移。改了那个文件记得同步这里。
set(ROMS_OFFSET 0x110000)

add_custom_target(flash-roms
    COMMAND ${CMAKE_COMMAND} -E echo
            "烧 ROM 分区到 ${ROMS_OFFSET}（8 MB 分区，约数分钟）"
    COMMAND python3 -m esptool --chip esp32s3 ${ESPTOOL_WRITE_FLASH_ARGS}
            write_flash ${ROMS_OFFSET} "${ROMS_IMAGE}"
    DEPENDS "${ROMS_IMAGE}"
    VERBATIM
    USES_TERMINAL)
