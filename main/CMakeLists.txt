# 注意：一旦给 main 显式写了 REQUIRES，它就不再自动依赖全部组件了，
# 所以 display.c / nes_emu.c 用到的驱动都得在这里列全。
idf_component_register(SRCS "main.c" "display.c" "nes_emu.c"
                            "input_serial.c" "input_gamepad.c"
                            "rom_store.c" "rom_menu.c"
                       INCLUDE_DIRS "."
                       REQUIRES nofrendo
                                esp_partition  # roms 分区 mmap
                                esp_lcd
                                esp_driver_spi
                                esp_driver_gpio
                                esp_driver_ledc
                                esp_driver_uart
                                esp_adc         # 摇杆走 ADC1
                                esp_timer
                                spi_flash      # 提供 esp_flash.h
                       # 这里登记的 ROM 只是「可选」，不是全都进固件 —— 没被
                       # nes_emu.c 的 ROM_CHOICE 选中的那几个没有 extern 引用，
                       # 会被链接器的 --gc-sections 回收掉。实测多登记 4 个 ROM
                       # （264 KB）固件只大了 500 字节。
                       #
                       # 所以体积上限是「固件本体 + 最大的那一个 ROM」，不是全部
                       # ROM 之和。app 分区 1 MB，固件本体约 400 KB，单个 ROM 撑到
                       # 500 KB 都没问题（NES 最大的卡也就 512 KB）。
                       EMBED_FILES "roms/smb.nes"
                                   "roms/tetris.nes"
                                   "roms/contra.nes"
                                   "roms/pacman.nes"
                                   "roms/drmario.nes"
                                   "roms/SimpleParallaxDemo.nes"
                                   "roms/full_palette.nes"
                                   "roms/flowing_palette.nes")
