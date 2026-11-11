"""Generate the LCD's Latin/CJK bitmap subset from licensed Unifont source."""
import gzip
import hashlib
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = root / 'fonts/unifont-18.0.01.hex.gz'
assert hashlib.sha256(source.read_bytes()).hexdigest() == '14c96e497466a82e46cf20032ce510e186bdc8658c9158e2290f452cda9bc498'
ranges = [(0x20, 0x2ff), (0x2000, 0x206f), (0x25a0, 0x25a1),
          (0x3000, 0x303f), (0x3400, 0x9fff), (0xff00, 0xffef)]
glyphs = {}
for line in gzip.decompress(source.read_bytes()).decode('ascii').splitlines():
    code, bits = line.split(':')
    code = int(code, 16)
    if any(first <= code <= last for first, last in ranges):
        data = bytes.fromhex(bits)
        if len(data) == 16:
            glyphs[code] = (8, b''.join(bytes((b, 0)) for b in data))
        elif len(data) == 32:
            glyphs[code] = (16, data)
        else:
            raise ValueError('Unsupported glyph size')
codes = [code for first, last in ranges for code in range(first, last + 1)]
assert all(code in glyphs for code in codes)
lines = ['/* Generated GNU Unifont subset; SIL OFL 1.1, see fonts/. */',
         '#include "display_font.h"', 'static const unsigned char widths[] = {']
for start in range(0, len(codes), 64):
    lines.append(','.join(str(glyphs[c][0]) for c in codes[start:start+64]) + ',')
lines += ['};', 'static const unsigned char bitmaps[][32] = {']
for code in codes:
    lines.append('{' + ','.join('0x%02x' % b for b in glyphs[code][1]) + '},')
lines += ['};', 'const unsigned char *display_font_glyph(uint32_t code, unsigned int *width) {']
offset = 0
for first, last in ranges:
    lines.append('if (code >= 0x%x && code <= 0x%x) { unsigned int i = code - 0x%x + %d; *width = widths[i]; return bitmaps[i]; }' % (first, last, first, offset))
    offset += last - first + 1
lines += ['return display_font_glyph(0x25a1, width);', '}']
(root / 'src/display_font.c').write_text('\n'.join(lines) + '\n', encoding='ascii')
print('Generated', len(codes), 'glyphs; source SHA256', hashlib.sha256(source.read_bytes()).hexdigest())
