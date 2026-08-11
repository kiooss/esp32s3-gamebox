# 注意：一旦给 main 显式写了 REQUIRES，它就不再自动依赖全部组件了，
# 所以 display.c / nes_emu.c 用到的驱动都得在这里列全。
idf_component_register(SRCS "main.c" "display.c" "menu_font.c" "nes_emu.c"
                            "gbc_emu.c"
                            "rgb_led.c" "audio_output.c"
                            "input_serial.c" "input_gamepad.c" "input_usb.c"
                            "rom_store.c" "rom_menu.c"
                       INCLUDE_DIRS "."
                       REQUIRES nofrendo gnuboy
                                esp_partition  # roms 分区 mmap
                                esp_lcd
                                esp_driver_spi
                                esp_driver_gpio
                                esp_driver_ledc
                                esp_driver_i2s
                                esp_driver_uart
                                usb             # ESP32-S3 原生 USB-OTG Host
                                usb_host_hid    # 通用 HID 手柄的中断报告接收
                                espressif__led_strip  # 板载可寻址 RGB LED
                                esp_adc         # 摇杆走 ADC1
                                esp_timer
                                nvs_flash      # 记住菜单里的声音开关
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

# nofrendo 没有宿主音频回调。包装它每帧唯一一次的 apu_emulate() 调用，
# 让 main/audio_output.c 接走 PCM；不修改 components/nofrendo 上游源码。
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=apu_emulate")
