idf_component_register(
    SRCS "cpu.c" "gnuboy.c" "hw.c" "lcd.c" "sound.c"
    INCLUDE_DIRS ".")

# 这套核心的 CPU/LCD 内循环每帧要执行几十万次；和 nofrendo 一样，-O3 是
# 能否稳定跑满约 59.7 fps 的功能配置，不是单纯的编译偏好。
target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
