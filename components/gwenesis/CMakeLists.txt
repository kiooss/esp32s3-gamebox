# 源码保持 retro-go 的 Gwenesis 快照不变；这里只把旧版 register_component()
# 声明换成 ESP-IDF 5.4 接口，并接到 Gamebox 自己的宿主层。
idf_component_register(
    SRC_DIRS
        "src/bus"
        "src/cpus/M68K"
        "src/cpus/Z80"
        "src/io"
        "src/savestate"
        "src/sound"
        "src/vdp"
    INCLUDE_DIRS
        "."
        "src/bus"
        "src/cpus/M68K"
        "src/cpus/Z80"
        "src/io"
        "src/savestate"
        "src/sound"
        "src/vdp")

# RETRO_GO 让 load_cartridge() 直接引用宿主提供的 ROM，避免再复制 4 MB。
# -O2 与 retro-go 原组件一致，保持对照版本的执行特征。
target_compile_definitions(${COMPONENT_LIB} PRIVATE RETRO_GO=1)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -Wno-unused-const-variable
    -Wno-unused-value
    -Wno-incompatible-pointer-types)
