# Dependencies and provenance

- **LVGL 9.6.0**, MIT: https://github.com/lvgl/lvgl/releases/tag/v9.6.0
  - Source archive: https://codeload.github.com/lvgl/lvgl/tar.gz/refs/tags/v9.6.0
  - SHA-256: `b20ee3acc1bba13c62d854f9ebd62e4c51e0b443b1e0225892e86442defa84df`
  - `vendor/lvgl/` retains upstream `src/`, `include/`, root headers, configuration template and `LICENCE.txt`. Upstream implementation is unmodified. Tests/examples/platform assets are omitted.
  - The application enables the software renderer, built-in Montserrat/symbol fonts, and core widgets. Optional third-party library license files remain beside their upstream sources.
  - `licenses/Montserrat-OFL.txt` and `licenses/FontAwesome5-LICENSE.txt` are copied unchanged from the same archive's `scripts/generators/built_in_font/font_license/` directory and accompany the compiled fonts/icons.
- **Noto Sans CJK SC**, SIL Open Font License 1.1: the generated subset in `fonts.c` contains 6854 characters at 20/24 px, 4 bpp, including the offline name-input dictionary. `FONT-LICENSE.txt` preserves the installed font package copyright/license. `tools/make_fonts.py` uses `/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc`, face 2. Generated data is checked in, so Pillow/font packages are not build/runtime dependencies.
- **Touch protocol reference** (not a copied driver): Espressif `esp_lcd_touch_st7123`, Apache-2.0:
  https://github.com/espressif/esp-iot-solution/tree/master/components/display/lcd_touch/esp_lcd_touch_st7123
  - Register pointer is 16-bit big-endian, I2C address is `0x55`, information block starts at `0x0000`, touch frames at `0x0010`, contacts have seven-byte records.
  - The KY50 module's physical chip marking has not been independently inspected. Support is based on matching information registers **and actual finger-coordinate tests on this board**, not an assumption that all Sitronix chips are compatible.
  - New Linux module sources are marked GPL-2.0. They do not write touch firmware or configuration.
- **Board media baseline**: adjacent `ky50_sensor/{capture.c,display.c,sensor_board.h}` and `ky50_lcd/sample_ky50_lcd.c`, and SDK GC2083/SYS/VB/VI/ISP/VPSS/GDC/VO/MIPI libraries. The adapters reuse these files without modifying the original regression programs. See `README.md` for timing and ownership.

- **GC0328 initialization table**, `GC0328-COMMON-01-140717`, amlogic-sh,
  GPL-2.0-or-later per source header (module also declares GPL v2):
  https://github.com/endlessm/linux-meson/blob/master/drivers/amlogic/camera/gc0328.c
  `gc0328_regs.h` retains the active register table; the KY50 I2C/VI adapter and
  board overrides are in `gc0328.c`. Register/address/clock meanings were checked
  against the user-provided GalaxyCore datasheet and application notes.
- **TinyALSA**, SDK static `libtinyalsa.a`, Android/BSD license. Header from
  `cvi_mpi/self_test/audio_ut/audio_pcm/asoundlib.h`; notice retained in
  `licenses/TinyALSA-BSD.txt`. PCM card 1 device 0, stereo 16 kHz S16_LE.
- **Speech asset**, generated offline by `tools/make_prompt.py` using the host's
  eSpeak NG Mandarin voice and ffmpeg, phrase “识别成功，请开门。”. Only the generated
  16 kHz mono PCM is deployed; neither synthesis engine nor ffmpeg is shipped.
- **Kernel compatibility modules** are GPL-2.0 and tied to this exact SDK kernel.
  `ky50_amp` temporarily coordinates ownership of the legacy DAC driver's left
  amplifier GPIO. It must not be used with a different codec object layout.
  `kernel/codec-dac-polarity.patch` is a separate future firmware patch; applying
  it changes that layout and requires removing the compatibility bridge.

- **PVA CV184X**: business archives and public headers copied from `SDK/libpva_face/build/cv184x-delivery`. Model identity is recorded in `pva/models/manifest.json`; the packed file contains the user's three fused UINT8-input models.
- **libsodium 1.0.19**, ISC: cross-compiled static dependency from the same delivery; full notice retained in `pva/licenses/libsodium.txt` and packaged `licenses/libsodium.txt`.
- **BMRuntime / bmlib**: matching CV184X SDK libraries already provided by the board `/system/lib`; dynamically linked and not redistributed inside this application package.
- **SQLite 3.53.1**, public domain: `vendor/sqlite/sqlite3.c` and `sqlite3.h` extracted from the local SDK `oss/oss_release_tarball/musl/sqlite.tar.gz`. Original public-domain notice remains in both files. Cross-compiled with threading and full disk synchronization; extension loading disabled.
- **ibus-table pinyin dictionary**, LGPL-2.1-or-later: local `/usr/share/ibus-table/data/pinyin_table.txt.bz2`, preserved in `assets/ime/` with the installed package `COPYRIGHT`. `tools/make_pinyin.py` creates an external, replaceable `pinyin.tsv`, filtered to GB2312 and ordered for names. No ibus program code is linked; the small application lookup implementation is separate. Original compressed data, regeneration script, attribution and full `licenses/ibus-pinyin-LGPL-2.1.txt` are distributed with the package.
