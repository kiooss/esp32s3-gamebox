# 注意：一旦给 main 显式写了 REQUIRES，它就不再自动依赖全部组件了，
# 所以 display.c / nes_emu.c 用到的驱动都得在这里列全。
idf_component_register(SRCS "main.c" "display.c" "menu_font.c" "nes_emu.c"
                            "gbc_emu.c" "snes_emu.c" "genesis_emu.c" "pce_emu.c"
                            "game_menu.c"
                            "rgb_led.c" "audio_output.c" "ui_sound.c"
                            "input_serial.c" "input_gamepad.c" "input_usb.c"
                            "rom_store.c" "rom_zip.c" "rom_menu.c" "rom_favorites.c" "loading_screen.c"
                            "overclock.c" "sd_card.c" "word_study.c" "word_study_data.c"
                            "word_audio.c"
                       INCLUDE_DIRS "."
                       REQUIRES nofrendo gnuboy snes9x gwenesis pce-go
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
                                esp_app_format # 从 app descriptor 读取统一项目版本
                                nvs_flash       # 单词学习进度，答对/错后跨重启保留
                                esp_rom        # ZIP DEFLATE 使用 ESP32-S3 ROM 内置 miniz
                                esp_hw_support # ROM CRC 与存档文件 CRC
                                fatfs          # TF 卡 ROM、目录缓存和四槽即时存档
                                spi_flash      # 提供 esp_flash.h
                                sdmmc          # TF 卡：sdmmc_cmd.h / 卡信息打印
                                esp_driver_sdspi # TF 卡走 SPI3
                       # 这里登记的 ROM 只是「可选」，不是全都进固件 —— 没被
                       # nes_emu.c 的 ROM_CHOICE 选中的那几个没有 extern 引用，
                       # 会被链接器的 --gc-sections 回收掉。实测多登记 4 个 ROM
                       # （264 KB）固件只大了 500 字节。
                       #
                       # 所以体积上限是「固件本体 + 最大的那一个 ROM」，不是全部
                       # ROM 之和。Genesis 加入后 app 分区已扩到 2 MB，嵌入 ROM
                       # 仍只用作分区损坏时的 NES 回退路径。
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
