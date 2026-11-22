# Graphics part of the Pico Engine by Daniel Kammer (GPL-3.0-or-later): 16 bit graphics buffers,
# drawing primitives, colours and bitmap fonts. Origin: github.com/fcipaq/snes9x_esp32,
# components/engine, commit 3bfb279. The files are copied from there, see README.md.
idf_component_register(SRCS "graphics/colors.cpp"
                            "graphics/gbuffers.cpp"
                            "graphics/primitives.cpp"
                            "fonts/fonts.cpp"
                       INCLUDE_DIRS ".")
