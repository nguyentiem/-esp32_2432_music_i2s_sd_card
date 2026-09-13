# Vendored dependencies

- `minimp3/minimp3.h`: https://github.com/lieff/minimp3,
  commit `ea99364f61c14656440e8d77e9c233ccf3124633`, CC0-1.0; license alongside source.
  Built with `MINIMP3_ONLY_MP3` and `MINIMP3_NO_SIMD` on ESP32.
- `main/font.h` is generated from DejaVu Sans (14 px), containing ASCII, Latin
  extensions and Vietnamese NFC glyphs. License in `fonts/LICENSE`.
  Regenerate with `python3 tools/make_font.py` (Pillow and system DejaVu Sans).

Neither dependency requires Arduino, PlatformIO, LVGL or ESP-ADF.
