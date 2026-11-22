# ui_gfx

The graphics part of the Pico Engine (github.com/fcipaq/snes9x_esp32, `components/engine`, commit
3bfb279, GPL-3.0-or-later, Daniel Kammer): 16 bit RGB565 graphics buffers, drawing primitives,
colour conversion and bitmap fonts. The on-board UI (`main/app_ui.cpp`) draws with it.

The files are copied unchanged. From the root of this repository:

```bash
SRC=~/snes9x_esp32/components/engine          # your checkout
DST=components/ui_gfx
mkdir -p $DST/graphics $DST/fonts/fontfiles
cp $SRC/typedefs.h $DST/
cp $SRC/graphics/{colors,gbuffers,primitives}.{cpp,h} $DST/graphics/
cp $SRC/fonts/fonts.{cpp,h} $DST/fonts/
cp $SRC/fonts/fontfiles/f13x16.h $DST/fonts/fontfiles/
```

Not copied yet: `graphics/blitter.{cpp,h}` (add them and the `.cpp` to `CMakeLists.txt` when images or
blending are needed) and the other fonts. Do not copy `control`, `power`, `adc`, `utils` and
`hwcfg.h`: they belong to the handheld.

## Fonts

Fonts are C arrays of type `font_t` (see `fonts/fontfiles/`). To use another font, add its header to
`fonts/fontfiles/` and change the include and the `UI_FONT` line in `main/app_ui.cpp`.
