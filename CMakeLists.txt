cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3-gamebox)

# ---- ROM 分区镜像 ----
#
# roms/ 下的 NES / GB / GBC / SNES / Genesis 游戏打包成一个镜像，烧进 partitions.csv
# 里那个 roms 分区。
# 格式和理由见 tools/pack_roms.py 顶部。
#
# 挂在 app 的依赖上，所以 `idf.py build` 会顺带生成镜像。烧它要单独一条命令：
#   idf.py flash-roms        （下面注册的 target）
# 或者手工：
#   esptool.py write_flash 0x210000 build/roms.bin
#
# 没有做成 `idf.py flash` 的一部分 —— ROM 镜像有好几 MiB，烧一次仍要较久，
# 每次改代码都重烧是白等。加了游戏才需要跑一次 flash-roms。
set(ROMS_IMAGE "${CMAKE_BINARY_DIR}/roms.bin")
# 所有扩展名都必须成为依赖；否则只替换 GB/GBC ROM 时，已有的 roms.bin
# 会被误判为最新，flash-roms 烧进去的仍是旧镜像。
# GLOB_RECURSE：roms/ 已按平台分子目录（nes/ gb/ gbc/ snes/ md/），非递归的
# GLOB 会一个都匹配不到，于是永远不重新打包——正是上面那句警告的情形。
# 这里会连 removed-*/ 一起收进依赖，和 pack_roms.py 的跳过规则不一致，但无害：
# 依赖只决定「要不要重打包」，装什么由 pack_roms.py 说了算，最多多打包一次。
file(GLOB_RECURSE ROM_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/roms/*.nes"
    "${CMAKE_SOURCE_DIR}/roms/*.gb"
    "${CMAKE_SOURCE_DIR}/roms/*.gbc"
    "${CMAKE_SOURCE_DIR}/roms/*.sfc"
    "${CMAKE_SOURCE_DIR}/roms/*.smc"
    "${CMAKE_SOURCE_DIR}/roms/*.md"
    "${CMAKE_SOURCE_DIR}/roms/*.bin"
    "${CMAKE_SOURCE_DIR}/roms/*.zip")

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
set(ROMS_OFFSET 0x210000)

# ${ESPTOOL_WRITE_FLASH_ARGS} 在这个手写 target 里其实展开成空——那个变量是
# esptool_py 组件给它自己生成的 flash target 用的，这个 add_custom_target
# 够不到。结果这条命令一直没带 -b，退回 esptool 保守的默认波特率，烧 11+ MB
# 镜像要十几分钟。改成显式 -b 460800，和 `idf.py flash` 烧 app 用的一致。
add_custom_target(flash-roms
    COMMAND ${CMAKE_COMMAND} -E echo
            "烧 ROM 分区到 ${ROMS_OFFSET}（13 MB 分区，约数分钟）"
    COMMAND python3 -m esptool --chip esp32s3 -b 460800
            write_flash ${ROMS_OFFSET} "${ROMS_IMAGE}"
    DEPENDS "${ROMS_IMAGE}"
    VERBATIM
    USES_TERMINAL)
