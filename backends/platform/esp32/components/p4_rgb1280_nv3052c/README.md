# p4_rgb1280_nv3052c

Board Manager board definition: ESP32-P4 with a 720x1280 NV3052C RGB565 panel (NV3052CGRB + HSD5.46 init sequence). It is derived from `p4_rgb720_nv3052c`: same pins, panel supply and SPI init, but a different resolution, timing, pixel clock and init table.

Select it with:

```bash
idf.py bmgr -x
idf.py fullclean
idf.py bmgr -b p4_rgb1280_nv3052c
idf.py build flash monitor
```

After changing `board_devices.yaml`, run `idf.py bmgr -b p4_rgb1280_nv3052c` again so that the generated config structs are updated. A stale `sdkconfig` keeps old values of options that already exist in it, so delete it when switching boards.

## Status

- display_lcd: 720x1280, 40 MHz pixel clock (PCLK active on the falling edge), 16-bit RGB565, DE/PCLK only (no HSYNC/VSYNC). Timing: HSYNC 10 / back porch 36 / front porch 46, VSYNC 5 / 5 / 16
- lcd_brightness: LEDC on GPIO 9 (100 kHz, 8-bit)
- panel_power: LDO ch.4 @ 3.3 V with a 200 ms settle time. The application initialises it by name before the LCD
- lcd_spi_init: bit-banged 9-bit SPI init on GPIO 7 (SCK) / 8 (MOSI), no hardware reset (see below). Depends on panel_power

## Split frame scanout

The LCD controller of the ESP32-P4 cannot count 1280 lines in one frame (the vertical size register is too small). The project therefore overrides the `esp_lcd` component (`components/esp_lcd`) with a driver that scans out every frame in two halves of 640 lines: the interrupt handlers reprogram the vertical timing and restart the DMA between the halves.

This board requires:

- `CONFIG_LCD_RGB_SPLIT_FRAME=y`, set by `sdkconfig.defaults.board`. The split position is always `v_res / 2`.
- Bounce buffers are required: `bounce_buffer_size_px` 28800 (40 lines) is known to work, 14400 (20 lines, saves 57.6 KiB of internal RAM) is being tested. Do not set it to 0. The ISR copies the frame from the two PSRAM framebuffers (about 1.8 MB each) into them.
- `CONFIG_LCD_RGB_SPLIT_FIFO_OFFSET`, an empirical DMA read offset in bytes. 110 was tuned for this panel.
- `CONFIG_LCD_RGB_ISR_IRAM_SAFE=y`, so the interrupt handlers keep running while the flash cache is disabled.

## Reset line (GPIO 54)

GPIO 54 is shared: it is the chip enable of the ESP32-C6 Wi-Fi co-processor and the reset input of the panel. In the extended screen firmware ESP-Hosted pulsed it when Wi-Fi started. ScummVM has no Wi-Fi, so `lcd_spi_init` pulses it itself right before the init sequence (`lcd_rst_gpio: 54`), like on board v1.2.

## ScummVM port

This is the bring-up board (UART console). Only the display and the SD card (4-bit SDMMC on GPIO 39-44, IO domain powered by LDO channel 4) are used; it has no touch, audio, battery or menu button in this definition.

## TODO

- touch (GT911): needs I2C SDA/SCL, INT and RST pins
- audio: needs codec part number, I2C address/pins, I2S pins and PA enable pin
- sdkconfig.defaults.board: PSRAM mode and speed (chip revision v1.0 is set)
