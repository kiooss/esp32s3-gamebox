cmake_minimum_required(VERSION 3.16)

# 项目版本只在这里维护。ESP-IDF 会把它写进 app descriptor，运行时的串口和
# 开机界面都从 descriptor 读取，避免出现几份手写版本号各自漂移。
set(PROJECT_VER "1.0.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3-gamebox)

# ---- WORDS 英式发音包 ----
#
# 游戏已经全部改从 TF 卡装载，原来给 flash ROM 库预留的约 14 MB 分区改给单词
# 发音。生成器用 macOS 的 Daniel（en_GB）逐词合成，再压成 24 kHz IMA ADPCM。
# 格式和板上解码器分别见 tools/build_word_audio.py / main/word_audio.c。
#
# 语音包不挂在 app 的默认依赖上：第一次生成 524 条要几分钟，教材没变时无需
# 重跑。生成并烧录用：
#   idf.py flash-word-audio
# 或者手工：
#   python3 tools/build_word_audio.py --source main/word_study_data.c \
#       --output build/word_audio_uk.bin
#   esptool.py write_flash 0x210000 build/word_audio_uk.bin
#
# 普通 `idf.py flash` 不写这个 data 分区，避免每次改 UI 都重复烧几 MiB。
set(WORD_AUDIO_IMAGE "${CMAKE_BINARY_DIR}/word_audio_uk.bin")

add_custom_command(
    OUTPUT "${WORD_AUDIO_IMAGE}"
    COMMAND ${CMAKE_COMMAND} -E echo "生成 Daniel 英式单词发音包..."
    COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/build_word_audio.py"
                    --source "${CMAKE_SOURCE_DIR}/main/word_study_data.c"
                    --output "${WORD_AUDIO_IMAGE}"
                    --voice Daniel --rate 155
    DEPENDS "${CMAKE_SOURCE_DIR}/main/word_study_data.c"
            "${CMAKE_SOURCE_DIR}/tools/build_word_audio.py"
    COMMENT "生成 WORDS 英式发音包"
    VERBATIM)

add_custom_target(word-audio DEPENDS "${WORD_AUDIO_IMAGE}")

# word_audio 分区在 partitions.csv 里的偏移。改了那个文件记得同步这里。
set(WORD_AUDIO_OFFSET 0x210000)

# ${ESPTOOL_WRITE_FLASH_ARGS} 在这个手写 target 里其实展开成空——那个变量是
# esptool_py 组件给它自己生成的 flash target 用的，这个 add_custom_target
# 够不到。结果这条命令一直没带 -b，退回 esptool 保守的默认波特率，烧 11+ MB
# 镜像要十几分钟。改成显式 -b 460800，和 `idf.py flash` 烧 app 用的一致。
add_custom_target(flash-word-audio
    COMMAND ${CMAKE_COMMAND} -E echo
            "烧英式发音包到 ${WORD_AUDIO_OFFSET}（约数分钟）"
    COMMAND python3 -m esptool --chip esp32s3 -b 460800
            write_flash ${WORD_AUDIO_OFFSET} "${WORD_AUDIO_IMAGE}"
    DEPENDS "${WORD_AUDIO_IMAGE}"
    VERBATIM
    USES_TERMINAL)
