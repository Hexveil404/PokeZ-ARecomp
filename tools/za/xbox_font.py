# Makes the Xbox button glyph fonts from your NintendoExt / Standard font dumps (tools/za/nca_font.py):
#   fonts/nintendo_ext_003_xbox.ttf       controls by position (default: the Switch's A on the east button = Xbox B):
#                                         the circled A/B and X/Y glyphs trade places
#   fonts/nintendo_ext_003_steamdeck.ttf   the same by position, with the Deck's L1 R1 L2 R2
#   fonts/nintendo_ext_003_xboxlabel.ttf  controls by letter (bind_pad_a = south ...): A B X Y as they are
# In both, the shoulder / trigger / menu glyphs get the Xbox pad's names: L R ZL ZR -> LB RB LT RT, + - -> Menu / View
# (outline U+E0A0.., filled U+E0E0..), as keys lettered with the Standard font. runtime/hle/pl.c picks one when the
# prompts are Xbox's. Needs fonttools.
# usage (from LASRecomp/): python tools/za/xbox_font.py
import io
from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.reverseContourPen import ReverseContourPen

KEY = bytes([0x49, 0x62, 0x18, 0x06])
SWAPS = [(0xE0A0, 0xE0A1), (0xE0A2, 0xE0A3), (0xE0E0, 0xE0E1), (0xE0E2, 0xE0E3)]
# (outline, filled) codepoint -> label
RELABEL = {(0xE0A4, 0xE0E4): 'LB', (0xE0A5, 0xE0E5): 'RB', (0xE0A6, 0xE0E6): 'LT', (0xE0A7, 0xE0E7): 'RT',
           (0xE0B3, 0xE0EF): 'MENU', (0xE0B4, 0xE0F0): 'VIEW'}


def bfttf(path):
    d = open(path, 'rb').read()
    size = int.from_bytes(bytes(a ^ b for a, b in zip(d[4:8], KEY)), 'big')
    return TTFont(io.BytesIO(bytes(d[8 + i] ^ KEY[i % 4] for i in range(size))))


def rect(pen, x0, y0, x1, y1, r, cw):
    pts = [(x0 + r, y0), (x1 - r, y0), (x1, y0 + r), (x1, y1 - r), (x1 - r, y1), (x0 + r, y1), (x0, y1 - r), (x0, y0 + r)]
    if cw:
        pts = pts[::-1]
    pen.moveTo(pts[0])
    for p in pts[1:]:
        pen.lineTo(p)
    pen.closePath()


DECK = {'LB': 'L1', 'RB': 'R1', 'LT': 'L2', 'RT': 'R2'}  # the Steam Deck's names for the same four


def build(out, swap, names=None):
    ext = bfttf('fonts/nintendo_ext_003.bfttf')
    std = bfttf('fonts/nintendo_udsg-r_std_003.bfttf')
    ecmap, scmap = ext.getBestCmap(), std.getBestCmap()
    sgs = std.getGlyphSet()
    glyf, hmtx = ext['glyf'], ext['hmtx']
    upm_s = std['head'].unitsPerEm
    for cps, label in RELABEL.items():
        label = (names or {}).get(label, label)
        for filled, cp in zip((False, True), cps):
            name = ecmap.get(cp)
            if not name:
                continue
            old = glyf[name]
            adv, lsb = hmtx[name]
            y0, y1 = (old.yMin, old.yMax) if hasattr(old, 'yMin') else (-100, 800)
            h = y1 - y0
            size = h * (0.55 if len(label) <= 2 else 0.36)
            tscale = size / (upm_s * 0.72)
            tw = sum(std['hmtx'][scmap[ord(ch)]][0] for ch in label if ord(ch) in scmap) * tscale
            pad, stroke = h * 0.16, h * 0.08
            w = max(h * 1.15, tw + 2 * pad)
            x0 = lsb if lsb > 0 else h * 0.05
            pen = TTGlyphPen(None)
            rect(pen, x0, y0, x0 + w, y1, h * 0.32, cw=True)  # the button (outer: clockwise)
            if not filled:
                rect(pen, x0 + stroke, y0 + stroke, x0 + w - stroke, y1 - stroke, h * 0.26, cw=False)
            tx, ty = x0 + (w - tw) / 2, y0 + (h - size) / 2 + size * 0.1
            dst = ReverseContourPen(pen) if filled else pen  # (cut out of a filled button, solid in a hollow one)
            for ch in label:
                g = scmap.get(ord(ch))
                if g:
                    sgs[g].draw(TransformPen(dst, (tscale, 0, 0, tscale, tx, ty)))
                    tx += std['hmtx'][g][0] * tscale
            glyf[name] = pen.glyph()
            hmtx[name] = (int(x0 * 2 + w), int(x0))
    if swap:
        for table in ext['cmap'].tables:
            if table.isUnicode():
                for a, b in SWAPS:
                    if a in table.cmap and b in table.cmap:
                        table.cmap[a], table.cmap[b] = table.cmap[b], table.cmap[a]
    ext['hhea'].advanceWidthMax = max(m[0] for m in hmtx.metrics.values())
    ext.save(out)
    print('wrote', out)


build('fonts/nintendo_ext_003_xbox.ttf', swap=True)
build('fonts/nintendo_ext_003_xboxlabel.ttf', swap=False)
build('fonts/nintendo_ext_003_steamdeck.ttf', swap=True, names=DECK)
