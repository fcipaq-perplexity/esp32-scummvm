
ScummVM for the ESP32-P4 tablet boards
======================================

This is a fork of [ScummVM](https://github.com/scummvm/scummvm) with an ESP32-P4 backend in
/backends/platform/esp32. This branch (`esp_tablet_1280`) runs it on the ESP32-P4 boards with a
720x1280 NV3052C RGB panel from [esp_extended_screen](https://github.com/fcipaq-perplexity/esp_extended_screen),
used in landscape (1280x720):

| Board definition | Board | Console | Used hardware |
|---|---|---|---|
| `p4_rgb1280_nv3052c_v1_2` | board v1.2 | USB Serial/JTAG of the P4 (CDC-ACM) | display, touch (GT911), SD card, I2S audio, battery, menu button, power off |
| `p4_rgb1280_nv3052c` | dev board (bring-up) | UART | display, SD card |

The board definitions (ESP Board Manager), the modified `esp_lcd` driver and the `ui_gfx` graphics
library are copied from esp_extended_screen into `components/`. The `esp_lcd` override only works
with ESP-IDF v6.0.x.

Building
--------

You need ESP-IDF **v6.0.3** (other v6.0.x releases should work, v6.1 does not because of the
`esp_lcd` override) and the Board Manager helper:

```bash
. $IDF_PATH/export.sh
pip install esp-bmgr-assist
cd backends/platform/esp32
idf.py set-target esp32p4
idf.py bmgr -b p4_rgb1280_nv3052c_v1_2      # or p4_rgb1280_nv3052c for the dev board
idf.py build flash monitor
```

When you switch boards, delete `sdkconfig` (and preferably run `idf.py fullclean`) before
`idf.py bmgr -b ...`: options that already exist in `sdkconfig` are not overwritten by the board
defaults. Building takes a while as a lot of ScummVM code is compiled and linked in (the binary is
more than 10 MiB, flash must be 16 MiB).

The panel needs the LCD bounce buffers (`bounce_buffer_size_px: 28800` in the board's
`board_devices.yaml`); see `components/p4_rgb1280_nv3052c_v1_2/README.md`.

Preparing the SD card
---------------------

To run games, you need a micro-SD-card (formatted in FAT) with both the ScummVM support files as
well as any games you want to run on it. To build the support files, go to the ScummVM root
directory and run ``make esp32dist``. This should generate an ``esp32dist`` folder containing a
``scummvm`` folder. Copy the ``scummvm`` folder to the root of the micro-SD-card.

The settings of the on-board menu are stored in `settings.dat` in the root of the card.

Display and touch
-----------------

The PPA scales every picture to fit the 1280x720 landscape screen (aspect ratio kept, black bars)
and rotates it by 90 degrees counter-clockwise into the portrait panel. If the picture is upside
down for your housing, enable `CONFIG_TABLET_ROTATE_270` in menuconfig ("ScummVM tablet"); touch
follows automatically.

Touching the screen moves the mouse and clicks. Two fingers bring up the onscreen keyboard.

On-board menu (board v1.2)
--------------------------

The button on GPIO 21 opens and closes the menu. While it is open the game is paused. It shows the
battery level and has:

* **Turn off**: tap twice (within 3 s) to switch the board off.
* **Brightness** and **Volume** sliders.
* **Audio**: speaker or headphones.

The menu closes by itself after 30 s without touch (`CONFIG_TABLET_UI_TIMEOUT_S`). Settings are
saved to `/settings.dat` on the SD card when a slider is released and when the menu closes.

Running the games
-----------------

First, add the game. Press 'Add game...', select the game folder, then select 'Choose'. Press 'OK'
on the next sceen. Finally, with the game selected, press 'Start' to start it.

Saving and loading depends on the engine selected. For instance, ScummVM games use the 'F5' key,
while in The 7th Guest, touching the top black matte above the active video area brings up
a menu.

Enabling more engines
---------------------

To keep the size of the binary in check, not all engines have been enabled. If you want your
favourite engine to be enabled, you can look up the engine name by running the configure
script in the root directory as ``./configure --help``. Then add the engine name in the
``backends/platform/esp32/components/scummvm/CMakeLists.txt`` file.

Issues
------

* There is no USB keyboard support. The onscreen keyboard does not work in the ScummVM load/save
  dialog, so save names cannot be typed there.

* Some games assume they're driven by a computer mouse and require either hovering of the cursor without
  clicking, or clicking with the left mouse button. These are not possible with the current touchscreen
  code.
