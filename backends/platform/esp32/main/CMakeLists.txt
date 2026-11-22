idf_component_register(SRCS "main.cpp" "esp-graphics.cpp" "esp-mixer.cpp" "posixesp-fs-factory.cpp"
                            "mmc.c" "esp-mutex.cpp"
                            "tablet_lcd.c" "tablet_hw.c" "tablet_audio.c" "tablet_settings.c" "tablet_ui.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_lcd esp_driver_ppa esp_driver_ledc esp_driver_i2s esp_driver_gpio
                                     esp_driver_sdmmc esp_adc esp_timer esp_mm fatfs sdmmc vfs
                                     blkcache scummvm ui_gfx
                       EMBED_FILES "loading.png")

target_compile_options(${COMPONENT_LIB} PRIVATE -DHAVE_CONFIG_H)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=reorder" APPEND)
