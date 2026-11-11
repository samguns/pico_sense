# Pico LCD bitmap subset

The generated `src/display_font.c` contains 28,802 Latin, punctuation and CJK
glyphs from GNU Unifont 18.0.01. The subset is distributed under SIL OFL 1.1;
upstream `COPYING` and `OFL-1.1.txt` are retained. Original contributors include
Roman Czyborra, Paul Hardy and the Unifont contributors identified upstream.
This is a display subset, not a modified release of the full Unifont font.

Source: https://ftp.gnu.org/gnu/unifont/unifont-18.0.01/unifont_all-18.0.01.hex.gz

The downloaded archive is stored as `unifont-18.0.01.hex.gz`, SHA256
`14c96e497466a82e46cf20032ce510e186bdc8658c9158e2290f452cda9bc498`.
Run `python scripts/generate_display_font.py` to regenerate the C data. The
generator validates the pinned checksum and glyph sizes. Bitmaps stay in flash;
the framebuffer, 15 MHz SPI DMA and two staging buffers remain unchanged.
