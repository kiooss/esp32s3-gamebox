# 注意：一旦给 main 显式写了 REQUIRES，它就不再自动依赖全部组件了，
# 所以 display.c / nes_emu.c 用到的驱动都得在这里列全。
idf_component_register(SRCS "main.c" "display.c" "nes_emu.c"
                            "input_serial.c" "input_gamepad.c"
                       INCLUDE_DIRS "."
                       REQUIRES nofrendo
                                esp_lcd
                                esp_driver_spi
                                esp_driver_gpio
                                esp_driver_ledc
                                esp_driver_uart
                                esp_adc         # 摇杆走 ADC1
                                esp_timer
                                spi_flash      # 提供 esp_flash.h
                       EMBED_FILES "roms/smb.nes"
                                   "roms/SimpleParallaxDemo.nes"
                                   "roms/full_palette.nes"
                                   "roms/flowing_palette.nes")
