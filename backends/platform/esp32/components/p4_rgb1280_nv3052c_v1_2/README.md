# p4_rgb1280_nv3052c_v1_2

Board Manager board definition: ESP32-P4 board v1.2 with a 720x1280 NV3052C RGB565 panel. Copied from
[esp_extended_screen](https://github.com/fcipaq-perplexity/esp_extended_screen) (`components/`) and
adapted for ScummVM (no Wi-Fi). ScummVM uses the panel in landscape (1280x720), the picture is
rotated by the PPA.

Select it with:

```bash
idf.py bmgr -x
idf.py fullclean
idf.py bmgr -b p4_rgb1280_nv3052c_v1_2
idf.py build flash monitor
```

After changing `board_devices.yaml`, run `idf.py bmgr -b p4_rgb1280_nv3052c_v1_2` again so that the generated config structs are updated. A stale `sdkconfig` keeps old values of options that already exist in it, so delete it when switching boards.

## Pinout

| Function | GPIO |
|---|---|
| DE | 3 |
| PCLK | 4 |
| DATA0..DATA4 (B0..B4) | 2, 1, 0, 53, 52 |
| DATA5..DATA10 (G0..G5) | 51, 50, 49, 48, 47, 46 |
| DATA11..DATA15 (R0..R4) | 45, 34, 33, 32, 31 |
| HSYNC / VSYNC / DISP | not connected |
| Panel SPI SCK / MOSI | 5 / 7 (no CS) |
| Reset of the C6 and the panel | 54 (shared, see below) |
| Backlight PWM | 8 |
| Power enable (EN of the main regulator) | 20 |
| Touch I2C SDA / SCL | 28 / 27 |
| Touch INT / RST | 26 / 29 |
| SD card (4-bit SDMMC) CLK / CMD | 43 / 44 |
| SD card D0..D3 | 39, 40, 41, 42 |
| Button BTN_ST (active high, opens the menu) | 21 |
| Power off latch (driven high to switch off) | 30 |
| Battery voltage (ADC, 2:1 divider) / charge status (low = charging) | 23 / 22 |
| I2S DATA / BCLK / LRCLK (NS4168 amplifiers) | 13 / 12 / 10 |
| Speaker / headphone amplifier enable | 9 / 11 |
| Console: USB Serial/JTAG of the P4 | USB D- / D+ (GPIO 24 / 25) |

## Status

- display_lcd: 720x1280, 40 MHz pixel clock (PCLK active on the falling edge), 16-bit RGB565, DE/PCLK only. Timing: HSYNC 10 / back porch 36 / front porch 46, VSYNC 5 / 5 / 16
- lcd_brightness: LEDC on GPIO 8 (100 kHz, 8-bit)
- panel_power: enables LDO channel 4 (LDO_VO4, supply of the IO domain VDD_IO_5, see below), drives GPIO 20 (enable of the main regulator) high to keep the board powered, never low, and parks SCK and MOSI low. The application initialises it by name before the LCD
- lcd_spi_init: resets the panel with GPIO 54 (100 ms low, 200 ms wait), then bit-banged 9-bit SPI init on GPIO 5 (SCK) / 7 (MOSI) with a configurable half clock period (5 us). SCK and MOSI stay driven low afterwards. Depends on panel_power
- lcd_touch: GT911 on the I2C bus i2c_master (SDA 28, SCL 27, RST 29, INT 26), 720x1280. Polled by the ScummVM backend (`main/tablet_lcd.c`). The factory function is in `setup_device.c`

## IO power domain VDD_IO_5

GPIO 39..48 of the ESP32-P4 are powered by the IO domain VDD_IO_5. On this board it is supplied by the chip's LDO channel 4 (LDO_VO4); `panel_power` enables it (`ldo_chan_id: 4`, 3300 mV). Without it these pins only swing about 1.1 V. The domain contains the green data lines G3..G5 (GPIO 48, 47, 46) and the lowest red bit R0 (GPIO 45): the symptom of the missing supply is an image without (most of the) green, and the yellow and green status screens look red and black. The Espressif P4 Function EV board powers its SD card pins (GPIO 39..44) from LDO_VO4 in the same way.

## Bounce buffers and frame scanout

The LCD controller of the ESP32-P4 cannot count 1280 lines in one frame, so the project overrides the `esp_lcd` component (`components/esp_lcd`, only compatible with ESP-IDF v6.0.x) with a driver that scans out every frame in two halves of 640 lines (reprogrammed vertical timing and a VSYNC timer).

**The panel needs bounce buffers (`bounce_buffer_size_px` in `board_devices.yaml`).** Use 28800 (40 lines); 14400 (20 lines) flickers. Do not set it to 0. The driver copies the frame from the PSRAM framebuffers into two internal RAM bounce buffers in its ISR, so `CONFIG_LCD_RGB_ISR_IRAM_SAFE=y` is required; `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y` restarts the GDMA every frame. `sdkconfig.defaults.board` sets both. (`CONFIG_LCD_RGB_SPLIT_FRAME` and `CONFIG_LCD_RGB_SPLIT_FIFO_OFFSET` are left over from an earlier version of the driver; the split is always active in the current one.)

## Reset line (GPIO 54) and start-up order

GPIO 54 is the chip enable of the ESP32-C6 Wi-Fi co-processor and the reset input of the panel, so one pulse resets both. ScummVM does not use Wi-Fi, the C6 just idles.

1. The backend drives GPIO 20 (enable of the main regulator) high first thing in `app_main`, so the board stays on after the power button is released.
2. `panel_power` enables LDO channel 4, parks SCK and MOSI low and resets the GT911.
3. `lcd_spi_init` pulses GPIO 54 (resets the panel and the C6) and sends the init sequence. The pin stays an output driven high.
4. `display_lcd` creates the RGB panel; the SD card (same IO domain as above) is mounted afterwards.

The panel's chip select is always asserted, so noise on SCK or MOSI between its reset and the init would shift the alignment of its 9-bit words. `panel_power` therefore parks both lines low before the reset pulse, and they stay driven low after the init.

## Audio, battery, button

These are not Board Manager devices, the backend drives them directly (`main/tablet_hw.c`, `main/esp-mixer.cpp`): I2S at 44.1 kHz stereo into the NS4168 amplifiers, volume applied in software; battery on ADC GPIO 23 with charge status on GPIO 22; the button on GPIO 21 opens the on-board menu; "Turn off" drives GPIO 30 high.
