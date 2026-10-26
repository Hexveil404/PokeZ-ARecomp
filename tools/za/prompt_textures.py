# Makes textures/prompts/<style>/<hash>.png: Pokémon Z-A's own button icons (the UI layouts' icon_key_a_00,
# icon_key_zl_00, ... and the title screen's "PRESS (A)") relabelled for the keyboard and the controllers. The system
# font glyphs (keyboard_font.py / xbox_font.py) only cover the button glyphs inside message text; the HUD, menus' key
# guides and the title draw these textures instead. runtime/nvn/vulkan/textures.c swaps them in like an HD pack
# texture, from the set runtime/hle/pl.c picks for the prompts (the same choice as the fonts). Every set follows the
# settings file's bindings (else the defaults): each icon names what the player presses for that input.
#   keyboard     - keys with the bound keys' names (bind_key_*)
#   xbox         - the Xbox pad's button bound to each input (bind_pad_*): A B X Y, LB RB LT RT, Menu View
#   steamdeck    - the Xbox faces with the Deck's own names: A B X Y, L1 R1 L2 R2, L3 R3, Menu View
#   playstation  - the same in PlayStation's: cross circle square triangle, L1 R1 L2 R2, Options Share
#   nintendo     - a Nintendo controller: only inputs moved off their own button get a new label
#
# The icons are BC5 masks: green = the button's shape, red = white with the label cut out. A relabel keeps the shape
# (the keyboard's keys get a key cap) and redraws the label, at 4x the game's size.
# The icons inside message text are glyphs of the game's own icon fonts (or_font, or_font_eng): relabelled the same
# way, in their pack rebuilt, they go to textures/prompts/<style>/romfs.patch, which runtime/services/fs.c lays over
# the game's data.trpfs.
#
# The textures are read from your own game files (extracted/romfs/arc/data.trpfs: Oodle-compressed packs), which needs
# ooz built as a library (https://github.com/powzix/ooz: g++ -O2 -shared -include sys/stat.h -Dmain=ooz_main
# kraken.cpp bitknit.cpp lzna.cpp + a C export `int ooz_decompress(src, src_len, dst, dst_len)` calling
# Kraken_Decompress) - pass it with --ooz. Needs Pillow, numpy and fontTools (and cffsubr, to make the fonts fit).
# usage (from LASRecomp/): python tools/za/prompt_textures.py --ooz <ooz.dll> [--settings larecomp.ini]
import argparse, ctypes, mmap, os, re, struct, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ap = argparse.ArgumentParser()
ap.add_argument('--romfs', action='append', help='game files: a folder or a RomFS image (default extracted/romfs, else '
                                                  'extracted/romfs.bin); more than once: several game versions')
ap.add_argument('--ooz', help='ooz shared library (ooz_decompress export; default: ooz.dll / libooz.so next to this program)')
ap.add_argument('--styles', default='keyboard,xbox,steamdeck,playstation,nintendo', help='the sets to make')
ap.add_argument('--list', metavar='REGEX', help="print the UI archives' textures whose name matches (then make nothing)")
ap.add_argument('--list-dir', help='with --list: save the BC5 ones there as PNG (red = label, green = shape)')
ap.add_argument('--install', metavar='VERSION', help='made into <out>.new, then <out>/.made = VERSION and swapped in '
                                                      'whole (the start screen / runtime: a game never sees half a set)')
ap.add_argument('--settings', default=None, help='settings file with bind_key_* lines (default: none = defaults)')
ap.add_argument('--out', default='textures/prompts')
ap.add_argument('--scale', type=int, default=4)
ap.add_argument('--font', default=None, help='bold TrueType font for the labels')
ap.add_argument('--mods', default='mods', help="mods folder (runtime/services/fs.c): their UI archives' icons too")
args = ap.parse_args()
args.romfs = args.romfs or ['extracted/romfs' if os.path.isdir('extracted/romfs') else 'extracted/romfs.bin']
FINAL = args.out
if args.install:
    import shutil
    args.out = FINAL.rstrip('/\\') + '.new'
    shutil.rmtree(args.out, ignore_errors=True)
S = args.scale
HERE = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, 'frozen', False) else __file__))
if not args.ooz:  # (the release's zaprompts/: ooz in the bundle, or next to the program)
    dirs = [getattr(sys, '_MEIPASS', HERE), HERE]
    args.ooz = next((p for p in (os.path.join(d, n) for d in dirs for n in ('ooz.dll', 'libooz.so', 'libooz.dylib'))
                     if os.path.exists(p)), None)
    if not args.ooz: sys.exit('error: no ooz library: pass --ooz')

# ------------------------------------------------------------------ game files: ONEPACK -> packs -> SARC -> BNTX
ooz = ctypes.CDLL(os.path.abspath(args.ooz))
ooz.ooz_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]


def unpack(comp, size, raw):
    if comp == 0xFF or size == len(raw): return bytes(raw)
    out = ctypes.create_string_buffer(size + 64)
    if ooz.ooz_decompress(bytes(raw), len(raw), out, size) != size: raise ValueError('ooz')
    return out.raw[:size]


class FB:  # flatbuffers, just what the ONEPACK tables need
    def __init__(s, m): s.m = m
    def u32(s, p): return struct.unpack_from('<I', s.m, p)[0]
    def u64(s, p): return struct.unpack_from('<Q', s.m, p)[0]
    def table(s, p):
        vt = p - struct.unpack_from('<i', s.m, p)[0]
        n = (struct.unpack_from('<H', s.m, vt)[0] - 4) // 2
        return [(p + o) if (o := struct.unpack_from('<H', s.m, vt + 4 + 2 * i)[0]) else None for i in range(n)]
    def vec(s, fp):
        v = fp + s.u32(fp)
        return s.u32(v), v + 4


class Entry:  # one file in a pack: where its fields are (positions in data.trpfs)
    def __init__(s, **k): s.__dict__.update(k)


def open_game(romfs):
    """-> (data.trpfs bytes, data.trpfd bytes): from a folder, or straight out of a RomFS image (romfs.bin, what the
    start screen's setup leaves: no copy of the multi-GB pack file)"""
    if os.path.isdir(romfs):
        with open(os.path.join(romfs, 'arc', 'data.trpfs'), 'rb') as f:
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        with open(os.path.join(romfs, 'arc', 'data.trpfd'), 'rb') as f:
            return memoryview(m), f.read()
    with open(romfs, 'rb') as f:
        img = memoryview(mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ))
    h = struct.unpack_from('<10Q', img, 0)  # (tools/za/romfs_extract.py: the same layout)
    dm, fm, data = img[h[3]:h[3] + h[4]], img[h[7]:h[7] + h[8]], h[9]

    def dir_path(d):
        parts = []
        while d:
            parent, = struct.unpack_from('<I', dm, d)
            nl, = struct.unpack_from('<I', dm, d + 0x14)
            parts.append(bytes(dm[d + 0x18:d + 0x18 + nl]).decode())
            d = parent
        return '/'.join(reversed(parts))
    want, o = {}, 0
    while o + 0x20 <= len(fm):
        parent, _, off, size, _, nl = struct.unpack_from('<IIQQII', fm, o)
        name = bytes(fm[o + 0x20:o + 0x20 + nl]).decode()
        if name in ('data.trpfs', 'data.trpfd') and dir_path(parent) == 'arc':
            want[name] = img[data + off:data + off + size]
        o += 0x20 + ((nl + 3) & ~3)
    if len(want) != 2: sys.exit(f'error: {romfs}: no arc/data.trpfs / data.trpfd in it')
    return want['data.trpfs'], bytes(want['data.trpfd'])


def game_files(m):
    fb = FB(m)
    assert bytes(m[:8]) == b'ONEPACK\0', 'not a ONEPACK file'
    root = fb.u64(8)
    rt = fb.table(root + fb.u32(root))
    _, hp = fb.vec(rt[0])
    n, op = fb.vec(rt[1])
    for i in range(n):
        off = fb.u64(op + 8 * i)
        t = fb.table(off + fb.u32(off))
        nf, fp = fb.vec(t[1])
        for j in range(nf):
            e = fp + 4 * j
            ft = fb.table(e + fb.u32(e))
            comp = m[ft[1]] if len(ft) > 1 and ft[1] else 0xFF
            size = fb.u64(ft[3]) if len(ft) > 3 and ft[3] else 0
            dn, dp = fb.vec(ft[4])
            magic = bytes(m[dp:dp + 4])
            if size < 64 << 20 and magic != b'BNTX' and (comp != 0xFF or magic == b'SARC'):
                yield Entry(comp=comp, size=size, raw=m[dp:dp + dn], m=m, pack=i, pack_hash=fb.u64(hp + 8 * i),
                            pack_off=off, slot=(fb.u64(op + 8 * i + 8) if i + 1 < n else len(m)) - off, index=j)


def sarc_files(d):
    sfat = struct.unpack_from('<H', d, 4)[0]
    doff = struct.unpack_from('<I', d, 0x0C)[0]
    n = struct.unpack_from('<H', d, sfat + 6)[0]
    for i in range(n):
        _, _, s, e = struct.unpack_from('<IIII', d, sfat + 0xC + 0x10 * i)
        yield d[doff + s:doff + e]


def bntx_textures(d):
    cnt = struct.unpack_from('<I', d, 0x24)[0]
    infos = struct.unpack_from('<Q', d, 0x28)[0]
    for i in range(cnt):
        t = struct.unpack_from('<Q', d, infos + 8 * i)[0]
        fmt = struct.unpack_from('<I', d, t + 0x1C)[0]
        w, h = struct.unpack_from('<II', d, t + 0x24)
        bh = struct.unpack_from('<I', d, t + 0x34)[0]
        size = struct.unpack_from('<I', d, t + 0x50)[0]
        no = struct.unpack_from('<Q', d, t + 0x60)[0]
        m0 = struct.unpack_from('<Q', d, struct.unpack_from('<Q', d, t + 0x70)[0])[0]
        name = d[no + 2:no + 2 + struct.unpack_from('<H', d, no)[0]].decode('latin1')
        yield name, w, h, fmt, bh, d[m0:m0 + size]


M64 = (1 << 64) - 1


def hash_full(p):  # runtime/nvn/vulkan/textures.c hash_full: the name a texture's bytes go by
    n = len(p)
    h = [0x9E3779B97F4A7C15 ^ n, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x27D4EB2F165667C5]
    i = 0
    while i + 32 <= n:
        for k, v in enumerate(struct.unpack_from('<4Q', p, i)):
            x = ((h[k] ^ v) * 0x100000001B3) & M64
            h[k] = x ^ (x >> 29)
        i += 32
    r = h[0] ^ ((h[1] * 0x9E3779B97F4A7C15) & M64) ^ ((h[2] * 0xC2B2AE3D27D4EB4F) & M64) ^ ((h[3] * 0x165667B19E3779F9) & M64)
    while i < n:
        r = ((r ^ p[i]) * 0x100000001B3) & M64
        i += 1
    return r ^ (r >> 31)


def deswizzle(src, wb, hb, bpb, bh_log2):  # Tegra block linear (64-byte x 8-row GOBs) -> rows of blocks
    gob_h, row = 8 << bh_log2, wb * bpb
    gobs_x = (row + 63) // 64
    out = bytearray(row * hb)
    for y in range(hb):
        by, byo = divmod(y, gob_h)
        gy, gyo = divmod(byo, 8)
        for x in range(row):
            gx, gxo = divmod(x, 64)
            o = (by * gobs_x + gx) * gob_h * 64 + gy * 512 + (gxo // 32) * 256 + (gyo // 2) * 64 + \
                ((gxo % 32) // 16) * 32 + (gyo % 2) * 16 + gxo % 16
            out[y * row + x] = src[o]
    return out


def bc4(b):
    a0, a1 = b[0], b[1]
    bits = int.from_bytes(b[2:8], 'little')
    pal = [a0, a1] + ([((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)] if a0 > a1 else
                      [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255])
    return [pal[(bits >> 3 * i) & 7] for i in range(16)]


def bc1_rg(b):  # a BC1 colour block (BC3's second half: always 4 colours) -> red, green of its 16 texels
    c0, c1, bits = int.from_bytes(b[0:2], 'little'), int.from_bytes(b[2:4], 'little'), int.from_bytes(b[4:8], 'little')
    rg = [((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63) for c in (c0, c1)]
    pal = rg + [tuple((2 * rg[0][k] + rg[1][k]) // 3 for k in (0, 1)), tuple((rg[0][k] + 2 * rg[1][k]) // 3 for k in (0, 1))]
    return [pal[(bits >> 2 * i) & 3] for i in range(16)]


def decode_bc5(data, w, h, bh, fmt=0x1e01):  # -> float32 HxWx2 (label, shape) 0..1; BC5, or BC3 (fmt 0x1c01)
    wb, hb = (w + 3) // 4, (h + 3) // 4
    lin = deswizzle(data, wb, hb, 16, bh)
    img = np.zeros((hb * 4, wb * 4, 2), np.float32)
    for y in range(hb):
        for x in range(wb):
            o = (y * wb + x) * 16
            if fmt == 0x1c01:  # (BC3: the label in the colour, the shape in the alpha - the texture's channel swizzle)
                img[y * 4:y * 4 + 4, x * 4:x * 4 + 4, 0] = np.array([c[0] for c in bc1_rg(lin[o + 8:o + 16])]).reshape(4, 4)
                img[y * 4:y * 4 + 4, x * 4:x * 4 + 4, 1] = np.array(bc4(lin[o:o + 8])).reshape(4, 4)
                continue
            img[y * 4:y * 4 + 4, x * 4:x * 4 + 4, 0] = np.array(bc4(lin[o:o + 8])).reshape(4, 4)
            img[y * 4:y * 4 + 4, x * 4:x * 4 + 4, 1] = np.array(bc4(lin[o + 8:o + 16])).reshape(4, 4)
    return img[:h, :w] / 255.0


# ------------------------------------------------------------------ labels
DEFAULTS = {'a': 'K', 'b': 'J', 'x': 'L', 'y': 'I', 'lstick': 'F', 'rstick': 'Keypad 0', 'l': 'Q', 'r': 'E',
            'zl': '1', 'zr': '3', 'plus': 'Return', 'minus': 'Backspace', 'left': 'Left', 'up': 'Up', 'right': 'Right',
            'down': 'Down', 'ls_up': 'W', 'ls_down': 'S', 'ls_left': 'A', 'ls_right': 'D', 'rs_up': 'Keypad 8',
            'rs_down': 'Keypad 5', 'rs_left': 'Keypad 4', 'rs_right': 'Keypad 6'}  # runtime/hle/bindings.c
SHORT = {'Return': 'Enter', 'Backspace': 'Bksp', 'Escape': 'Esc', 'Left Shift': 'Shift', 'Right Shift': 'Shift',
         'Left Ctrl': 'Ctrl', 'Right Ctrl': 'Ctrl', 'Left Alt': 'Alt', 'Right Alt': 'Alt', 'CapsLock': 'Caps',
         'Delete': 'Del', 'Insert': 'Ins', 'PageUp': 'PgUp', 'PageDown': 'PgDn', 'Keypad Enter': 'Enter'}
ARROWS = {'Up': 'up', 'Down': 'down', 'Left': 'left', 'Right': 'right'}

# the controller: the button each Switch input is on (bind_pad_*, by position; runtime/hle/bindings.c DEF_PAD)
PAD_DEFAULTS = {'a': 'east', 'b': 'south', 'x': 'north', 'y': 'west', 'lstick': 'lstick', 'rstick': 'rstick',
                'l': 'lshoulder', 'r': 'rshoulder', 'zl': 'ltrigger', 'zr': 'rtrigger', 'plus': 'start', 'minus': 'back',
                'left': 'dpad_left', 'up': 'dpad_up', 'right': 'dpad_right', 'down': 'dpad_down'}
binds, pad_binds = dict(DEFAULTS), dict(PAD_DEFAULTS)
if args.settings:
    for line in open(args.settings, encoding='utf-8', errors='replace'):
        k, _, v = line.partition('=')
        k, v = k.strip(), v.strip()
        if k.startswith('bind_key_') and v and k[9:] in binds: binds[k[9:]] = v
        if k.startswith('bind_pad_') and v and k[9:] in pad_binds: pad_binds[k[9:]] = v


def key(slot):
    v = binds[slot]
    if v in ARROWS: return ('arrow', ARROWS[v])
    if v.startswith('Keypad ') and len(v) == 8: return v[7]
    return SHORT.get(v, v)


def stick_keys(prefix):  # WASD-like: the four keys' names in up, left, down, right order, when they're one letter each
    ks = [key(prefix + d) for d in ('up', 'left', 'down', 'right')]
    return ''.join(ks) if all(isinstance(k, str) and len(k) == 1 for k in ks) else ks[0]


# icon name -> button; '|' = two buttons side by side
ICONS = {'icon_key_a_00^t': 'a', 'icon_key_b_00^t': 'b', 'icon_key_x_00^t': 'x', 'icon_key_y_00^t': 'y',
         'icon_key_l_00^t': 'l', 'icon_key_r_00^t': 'r', 'icon_key_zl_00^t': 'zl', 'icon_key_zr_00^t': 'zr',
         'icon_key_l_ol_00^t': 'l', 'icon_key_r_ol_00^t': 'r', 'icon_key_plus_00^t': 'plus', 'icon_key_minus_00^t': 'minus',
         'icon_key_y_hold_00^t': 'y', 'icon_key_zl_keep_00^t': 'zl', 'icon_key_zr_keep_00^t': 'zr',
         'icon_key_lr_00^t': 'l|r', 'icon_key_zlzr_00^t': 'zl|zr',
         'icon_key_up_00^t': 'up', 'icon_key_down_00^t': 'down', 'icon_key_left_00^t': 'left', 'icon_key_right_00^t': 'right',
         'icon_key_tenkey_leftright_00^t': 'left|right', 'icon_key_tenkey_updown_00^t': 'up|down',
         'icon_key_stick_l_00^t': 'ls', 'icon_key_stick_r_00^t': 'rs', 'icon_key_stick_r_01^t': 'rs',
         'icon_key_stick_r_02^t': 'rs', 'icon_key_stick_l_push_00^t': 'lstick', 'icon_key_stick_r_push_00^t': 'rstick',
         'grptext_title_key_guide_00^t': 'a'}
KEEP_SHAPE = ('_hold_', '_keep_', '_ol_', 'title')  # (shapes with a mark of their own: hold / outline)

# The controllers' sets name, for each Switch input, the button it is bound to (bind_pad_*) in that pad's own words:
# a rebind (say the menu, the Switch's X, moved to Xbox Menu) changes the icons with it. runtime/ui/launcher.mm remakes
# them when the bindings change. A d-pad direction or stick press on its own place keeps the game's drawing.
PAD_LABEL = {
    'xbox': {'south': 'A', 'east': 'B', 'west': 'X', 'north': 'Y', 'lshoulder': 'LB', 'rshoulder': 'RB',
             'ltrigger': 'LT', 'rtrigger': 'RT', 'start': ('menu',), 'back': ('view',), 'lstick': 'LS', 'rstick': 'RS',
             'guide': 'XB', 'misc1': 'SH', 'rpaddle1': 'P1', 'lpaddle1': 'P3', 'rpaddle2': 'P2', 'lpaddle2': 'P4'},
    # the Deck: Xbox's faces and Menu / View, its own names for the shoulders, triggers, sticks and back buttons
    'steamdeck': {'south': 'A', 'east': 'B', 'west': 'X', 'north': 'Y', 'lshoulder': 'L1', 'rshoulder': 'R1',
                  'ltrigger': 'L2', 'rtrigger': 'R2', 'start': ('menu',), 'back': ('view',), 'lstick': 'L3', 'rstick': 'R3',
                  'guide': 'ST', 'misc1': 'QA', 'rpaddle1': 'R4', 'lpaddle1': 'L4', 'rpaddle2': 'R5', 'lpaddle2': 'L5'},
    'playstation': {'south': ('cross',), 'east': ('circle',), 'west': ('square',), 'north': ('triangle',),
                    'lshoulder': 'L1', 'rshoulder': 'R1', 'ltrigger': 'L2', 'rtrigger': 'R2', 'start': ('options',),
                    'back': ('share',), 'lstick': 'L3', 'rstick': 'R3', 'guide': 'PS', 'touchpad': 'TP',
                    'misc1': 'M', 'rpaddle1': 'RB', 'lpaddle1': 'LB', 'rpaddle2': 'RF', 'lpaddle2': 'LF'},
    # a Nintendo controller (Pro Controller, Joy-Cons): only a moved input gets a new label
    'nintendo': {'south': 'B', 'east': 'A', 'west': 'Y', 'north': 'X', 'lshoulder': 'L', 'rshoulder': 'R',
                 'ltrigger': 'ZL', 'rtrigger': 'ZR', 'start': '+', 'back': '-', 'lstick': 'LS', 'rstick': 'RS',
                 'guide': 'H', 'misc1': 'C'},
}
DPAD = {'dpad_up': ('arrow', 'up'), 'dpad_down': ('arrow', 'down'), 'dpad_left': ('arrow', 'left'), 'dpad_right': ('arrow', 'right')}


def label(style, button):
    """the new label: a string, ('arrow', dir), a shape (('menu',), ('circle',)...), or None = leave the game's"""
    if style == 'keyboard':
        if button == 'ls': return stick_keys('ls_')
        if button == 'rs': return stick_keys('rs_')
        return key(button)
    if button not in pad_binds: return None  # (the sticks' own icons)
    host = pad_binds[button]
    if host == PAD_DEFAULTS[button] and (button in ('lstick', 'rstick', 'up', 'down', 'left', 'right') or style == 'nintendo'):
        return None
    return DPAD.get(host) or PAD_LABEL[style].get(host, host[:2].upper())


# ------------------------------------------------------------------ drawing
FONTS = [args.font, 'C:/Windows/Fonts/arialbd.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',
         '/usr/share/fonts/TTF/DejaVuSans-Bold.ttf', '/System/Library/Fonts/Supplemental/Arial Bold.ttf']
FONT = next((f for f in FONTS if f and os.path.exists(f)), None)
if not FONT: sys.exit('no bold font found: pass --font')


def text_mask(s, cap_h, max_w):
    """white text on black, cap height ~cap_h px, at most max_w wide"""
    f = ImageFont.truetype(FONT, 200)
    x0, y0, x1, y1 = f.getbbox(s)
    im = Image.new('L', (x1 - x0 + 20, y1 - y0 + 20))
    ImageDraw.Draw(im).text((10 - x0, 10 - y0), s, font=f, fill=255)
    im = im.crop(im.getbbox())
    cap = f.getbbox('H')
    k = cap_h / (cap[3] - cap[1])
    k = min(k, max_w / im.width)
    return im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)


def shape_mask(kind, w, h):  # Xbox Menu (three bars) / View (two windows), PlayStation's shapes, at w x h
    big = 8
    im = Image.new('L', (w * big, h * big))
    d = ImageDraw.Draw(im)
    W, H = w * big, h * big
    t = max(1, round(min(W, H) * 0.13))
    if kind in ('circle', 'cross', 'triangle', 'square'):
        t = max(1, round(min(W, H) * 0.14))
        if kind == 'circle': d.ellipse((t / 2, t / 2, W - t / 2, H - t / 2), outline=255, width=t)
        elif kind == 'square': d.rectangle((W * 0.08, H * 0.08, W * 0.92, H * 0.92), outline=255, width=t)
        elif kind == 'triangle':
            pts = [(W / 2, H * 0.06), (W * 0.97, H * 0.88), (W * 0.03, H * 0.88)]
            d.line(pts + [pts[0], pts[1]], fill=255, width=t, joint='curve')
        else:
            for a, b in (((0.12, 0.12), (0.88, 0.88)), ((0.88, 0.12), (0.12, 0.88))):
                d.line((W * a[0], H * a[1], W * b[0], H * b[1]), fill=255, width=round(t * 1.1))
        return im.resize((w, h), Image.LANCZOS)
    if kind == 'share':  # Share / Create: a flat pill, as the touchpad beside it (an upright one read as a zero)
        d.rounded_rectangle((W * 0.04, H * 0.27, W * 0.96, H * 0.73), radius=H * 0.2, outline=255, width=t)
        d.line((W * 0.32, H * 0.5, W * 0.68, H * 0.5), fill=255, width=t)
        return im.resize((w, h), Image.LANCZOS)
    if kind in ('menu', 'options'):
        for i in range(3):
            y = H * (0.2 + 0.3 * i)
            d.rounded_rectangle((W * 0.1, y - t / 2, W * 0.9, y + t / 2), radius=t / 2, fill=255)
    else:
        d.rounded_rectangle((W * 0.3, H * 0.05, W * 0.95, H * 0.62), radius=t, outline=255, width=t)
        d.rounded_rectangle((W * 0.05, H * 0.38, W * 0.7, H * 0.95), radius=t, fill=0)
        d.rounded_rectangle((W * 0.05, H * 0.38, W * 0.7, H * 0.95), radius=t, outline=255, width=t)
    return im.resize((w, h), Image.LANCZOS)


def arrow_mask(direction, w, h):
    big = 8
    im = Image.new('L', (w * big, h * big))
    W, H = w * big, h * big
    pts = {'up': [(W / 2, 0), (W, H), (0, H)], 'down': [(0, 0), (W, 0), (W / 2, H)],
           'left': [(0, H / 2), (W, 0), (W, H)], 'right': [(0, 0), (W, H / 2), (0, H)]}[direction]
    ImageDraw.Draw(im).polygon(pts, fill=255)
    return im.resize((w, h), Image.LANCZOS)


def components(mask):
    """4-connected components of a bool array -> list of index arrays"""
    seen = np.zeros(mask.shape, bool)
    out = []
    H, W = mask.shape
    for sy, sx in zip(*np.nonzero(mask)):
        if seen[sy, sx]: continue
        stack, pts = [(sy, sx)], []
        seen[sy, sx] = True
        while stack:
            y, x = stack.pop()
            pts.append((y, x))
            for ny, nx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
                if 0 <= ny < H and 0 <= nx < W and mask[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    stack.append((ny, nx))
        out.append(np.array(pts))
    return out


def up(ch):  # one channel 0..1 -> S x larger, edges kept crisp
    im = Image.fromarray((ch * 255).astype(np.uint8)).resize((ch.shape[1] * S, ch.shape[0] * S), Image.BICUBIC)
    a = np.asarray(im, np.float32) / 255.0
    return np.clip((a - 0.5) * 2.5 + 0.5, 0, 1)


def keycap(w, h):  # a key: rounded box filling the cell (S x scale)
    im = Image.new('L', (w * 8, h * 8))
    m = 8 * S * 0.6
    ImageDraw.Draw(im).rounded_rectangle((m, m, w * 8 - m, h * 8 - m), radius=min(w, h) * 8 * 0.22, fill=255)
    return np.asarray(im.resize((w, h), Image.LANCZOS), np.float32) / 255.0


def relabel(img, name, buttons, style):
    """img: HxWx2 (R, G) at the game's size -> RGBA PNG image at S x, or None when nothing changes"""
    labels = [label(style, b) for b in buttons]
    keep_shape = any(k in name for k in KEEP_SHAPE)
    if all(l is None for l in labels) and (style != 'keyboard' or keep_shape): return None
    H, W = img.shape[:2]
    R, G = up(img[:, :, 0]), up(img[:, :, 1])
    shape = img[:, :, 1] > 0.5
    # the old label: holes in the red inside the shape (not touching its edge: an outline ring stays)
    hole = shape & (img[:, :, 0] < 0.5)
    outside = ~shape
    edge = np.zeros_like(outside)
    edge[1:] |= outside[:-1]; edge[:-1] |= outside[1:]; edge[:, 1:] |= outside[:, :-1]; edge[:, :-1] |= outside[:, 1:]
    edge |= outside
    olds = [c for c in components(hole) if not edge[c[:, 0], c[:, 1]].any()]
    # the buttons' cells: the shape's parts (two buttons side by side: left / right halves)
    parts = sorted((c for c in components(shape) if len(c) > 20), key=lambda c: c[:, 1].min())
    if 'title' in name:  # the (A): the part with a label inside (the words' letters are the shape itself)
        inside = np.zeros((H, W), bool)
        for c in olds: inside[c[:, 0], c[:, 1]] = True
        parts = [c for c in parts if inside[c[:, 0].min():c[:, 0].max() + 1, c[:, 1].min():c[:, 1].max() + 1].any()][-1:]
    elif len(buttons) == 1 or len(parts) != len(buttons):
        parts = [np.argwhere(shape)]
    if len(parts) != len(buttons): return None
    outR = R.copy()
    outG = G.copy()
    for part, new, button in zip(parts, labels, buttons):
        y0, x0 = part.min(0)
        y1, x1 = part.max(0) + 1
        mine = [c for c in olds if y0 <= c[:, 0].min() and c[:, 0].max() < y1 and x0 <= c[:, 1].min() and c[:, 1].max() < x1]
        if new is None and style == 'keyboard': new = 'same'
        if new is None: continue
        # label box: the old label's (its centre, height), else the cell's middle
        if mine:
            pts = np.concatenate(mine)
            ly0, lx0 = pts.min(0)
            ly1, lx1 = pts.max(0) + 1
        else:
            ly0, ly1, lx0, lx1 = y0 + (y1 - y0) * 0.3, y0 + (y1 - y0) * 0.7, x0 + (x1 - x0) * 0.3, x1 - (x1 - x0) * 0.3
        cx, cy = (lx0 + lx1) / 2 * S, (ly0 + ly1) / 2 * S
        if style == 'keyboard' and 'title' in name and isinstance(new, str) and len(new) > 1:
            # the title's (A): a key as wide as the name, growing to the right while there's room
            tw = text_mask(new, (y1 - y0) * S * 0.42, 1 << 20).width / S + (y1 - y0) * 0.6
            others = shape.copy()
            others[part[:, 0], part[:, 1]] = False
            rx1 = x0 + (x1 - x0)
            while rx1 < min(W, x0 + tw) and not others[y0:y1, rx1:rx1 + 3].any(): rx1 += 1
            if rx1 > x1:
                outG[y0 * S:y1 * S, x0 * S:rx1 * S] = keycap((rx1 - x0) * S, (y1 - y0) * S)
                outR[y0 * S:y1 * S, x0 * S:rx1 * S] = 1.0
                x1 = rx1
                cx, cy = (x0 + x1) / 2 * S, (y0 + y1) / 2 * S
                mine = []
        if style == 'keyboard' and not keep_shape:  # a key cap over the cell
            cap = keycap((x1 - x0) * S, (y1 - y0) * S)
            outG[y0 * S:y1 * S, x0 * S:x1 * S] = cap
            outR[y0 * S:y1 * S, x0 * S:x1 * S] = 1.0
            cx, cy = (x0 + x1) / 2 * S, (y0 + y1) / 2 * S
        for c in mine:  # old label out
            m = np.zeros((H, W), bool)
            m[c[:, 0], c[:, 1]] = True
            mm = np.asarray(Image.fromarray(m.astype(np.uint8) * 255).resize((W * S, H * S), Image.NEAREST)
                            .filter(ImageFilter.MaxFilter(2 * S + 1)), np.float32) / 255.0
            outR = np.maximum(outR, mm)
        cell_w, cell_h = (x1 - x0) * S, (y1 - y0) * S
        if new == 'same':  # (keyboard d-pad keys: the game's own arrow back on the key)
            for c in mine:
                m = np.zeros((H, W), np.float32)
                m[c[:, 0], c[:, 1]] = 1.0
                outR = np.minimum(outR, 1.0 - up(m))
            continue
        if isinstance(new, tuple) and new[0] == 'arrow':
            s = round(min(cell_w, cell_h) * 0.42)
            lm = arrow_mask(new[1], s, s)
        elif isinstance(new, tuple):
            s = round(min(cell_w, cell_h) * 0.5)
            lm = shape_mask(new[0], s, s)
        else:
            old_h = (ly1 - ly0) * S if mine else cell_h * 0.5
            cap_h = min(max(old_h, cell_h * 0.42), cell_h * 0.56)
            if len(new) > 2: cap_h = min(cap_h, cell_h * 0.38)
            lm = text_mask(new, cap_h, cell_w * (0.78 if style == 'keyboard' else 0.7))
        lw, lh = lm.size
        px, py = round(cx - lw / 2), round(cy - lh / 2)
        a = np.zeros_like(outR)
        a[py:py + lh, px:px + lw] = np.asarray(lm, np.float32)[:a.shape[0] - py, :a.shape[1] - px] / 255.0
        outR = np.minimum(outR, 1.0 - a)
    rgba = np.zeros((H * S, W * S, 4), np.uint8)
    rgba[:, :, 0] = (outR * 255 + 0.5).astype(np.uint8)
    rgba[:, :, 1] = (outG * 255 + 0.5).astype(np.uint8)
    rgba[:, :, 3] = 255
    return Image.fromarray(rgba, 'RGBA')


# ------------------------------------------------------------------ the game's icon fonts
# The button icons inside message text ("press (R) on the main menu") are glyphs of the game's own fonts or_font /
# or_font_eng (U+E31D..: .bfttf files in arc/uifontbinbmp_din_24_28.bfcpx.trpak), drawn by its text engine - not the
# system NintendoExt font. Relabelled the same way, they go back into a copy of their pack that the runtime serves in
# place of the game's (romfs.patch below, runtime/services/fs.c).
FONT_GLYPHS = {0xE31D: 'a', 0xE31E: 'b', 0xE31F: 'x', 0xE320: 'y', 0xE321: 'l', 0xE322: 'r', 0xE323: 'zl',
               0xE324: 'zr', 0xE325: 'up', 0xE326: 'down', 0xE327: 'left', 0xE328: 'right', 0xE329: 'plus',
               0xE32A: 'minus', 0xE32E: 'up', 0xE32F: 'down', 0xE330: 'left', 0xE331: 'right', 0xE335: 'ls',
               0xE336: 'rs', 0xE338: 'lstick', 0xE339: 'rstick', 0xE33C: 'ls', 0xE33D: 'ls', 0xE33E: 'rs', 0xE33F: 'rs'}
BKEY = bytes([0x49, 0x62, 0x18, 0x06])


def bfttf_decode(d):
    n = int.from_bytes(bytes(a ^ b for a, b in zip(d[4:8], BKEY)), 'big')
    return bytes(d[8 + i] ^ BKEY[i % 4] for i in range(n))


def bfttf_encode(magic, t):
    t = t + b'\0' * (-len(t) % 4)
    return magic + bytes(a ^ b for a, b in zip(len(t).to_bytes(4, 'big'), BKEY)) + \
        bytes(t[i] ^ BKEY[i % 4] for i in range(len(t)))


def oodle_stored(d):  # an Oodle stream of uncompressed blocks (what the game's decompressor takes as is)
    out = bytearray()
    for i in range(0, len(d), 0x40000):
        out += bytes([0xCC if i == 0 else 0x4C, 0x0A]) + d[i:i + 0x40000]
    return bytes(out)


def contours(draw):
    from fontTools.pens.recordingPen import RecordingPen
    pen = RecordingPen()
    draw(pen)
    out, cur = [], []
    for op, a in pen.value:
        cur.append((op, a))
        if op in ('closePath', 'endPath'): out.append(cur); cur = []
    if cur: out.append(cur)
    return out


def area(c):  # signed (shoelace over every point: enough for the sign and the size order)
    pts = [p for _, a in c for p in a]
    return sum(pts[i][0] * pts[i - 1][1] - pts[i - 1][0] * pts[i][1] for i in range(len(pts))) / 2


def bounds(cs):
    pts = [p for c in cs for _, a in c for p in a]
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


def replay(c, pen, reverse=False):
    from fontTools.pens.reverseContourPen import ReverseContourPen
    p = ReverseContourPen(pen) if reverse else pen
    for op, a in c: getattr(p, op)(*a)


def rrect(x0, y0, x1, y1, r):  # a rounded box contour
    k = 0.45 * r
    return [('moveTo', ((x0 + r, y0),)), ('lineTo', ((x1 - r, y0),)), ('curveTo', ((x1 - r + k, y0), (x1, y0 + r - k), (x1, y0 + r))),
            ('lineTo', ((x1, y1 - r),)), ('curveTo', ((x1, y1 - r + k), (x1 - r + k, y1), (x1 - r, y1))),
            ('lineTo', ((x0 + r, y1),)), ('curveTo', ((x0 + r - k, y1), (x0, y1 - r + k), (x0, y1 - r))),
            ('lineTo', ((x0, y0 + r),)), ('curveTo', ((x0, y0 + r - k), (x0 + r - k, y0), (x0 + r, y0))), ('closePath', ())]


def poly(pts):
    return [('moveTo', (pts[0],))] + [('lineTo', (p,)) for p in pts[1:]] + [('closePath', ())]


def ellipse(cx, cy, rx, ry, reverse=False):  # four cubic arcs, counter-clockwise (clockwise: reverse)
    k = 0.5523
    sy = -1 if reverse else 1
    return [('moveTo', ((cx + rx, cy),)),
            ('curveTo', ((cx + rx, cy + sy * k * ry), (cx + k * rx, cy + sy * ry), (cx, cy + sy * ry))),
            ('curveTo', ((cx - k * rx, cy + sy * ry), (cx - rx, cy + sy * k * ry), (cx - rx, cy))),
            ('curveTo', ((cx - rx, cy - sy * k * ry), (cx - k * rx, cy - sy * ry), (cx, cy - sy * ry))),
            ('curveTo', ((cx + k * rx, cy - sy * ry), (cx + rx, cy - sy * k * ry), (cx + rx, cy))), ('closePath', ())]


LABEL_FONT = None


def label_contours(new, cx, cy, cap_h, max_w):
    """the label's contours centred on (cx, cy) in font units"""
    global LABEL_FONT
    from fontTools.ttLib import TTFont as _TT
    from fontTools.pens.transformPen import TransformPen
    if isinstance(new, tuple) and new[0] == 'arrow':
        s = cap_h * 0.55
        pts = {'up': [(0, s), (-s, -s * 0.8), (s, -s * 0.8)], 'down': [(0, -s), (s, s * 0.8), (-s, s * 0.8)],
               'left': [(-s, 0), (s * 0.8, -s), (s * 0.8, s)], 'right': [(s, 0), (-s * 0.8, s), (-s * 0.8, -s)]}[new[1]]
        return [poly([(cx + x, cy + y) for x, y in pts])]
    if isinstance(new, tuple) and new[0] in ('circle', 'cross', 'triangle', 'square', 'share'):
        # PlayStation's shapes as rings: an outer contour and the inner one the other way round (one polygon for the
        # cross: overlapping bars would leave its middle filled once the label is cut out of the button)
        s, t = cap_h * 0.62, cap_h * 0.15
        if new[0] == 'circle':
            return [ellipse(cx, cy, s, s), ellipse(cx, cy, s - t, s - t, reverse=True)]
        if new[0] == 'square':
            a = s * 0.86
            return [poly([(cx - a, cy - a), (cx + a, cy - a), (cx + a, cy + a), (cx - a, cy + a)]),
                    poly([(cx - a + t, cy - a + t), (cx - a + t, cy + a - t), (cx + a - t, cy + a - t), (cx + a - t, cy - a + t)])]
        if new[0] == 'triangle':
            outer = [(cx, cy + s), (cx - s * 1.08, cy - s * 0.8), (cx + s * 1.08, cy - s * 0.8)]
            k = t * 2.0  # (the inner one: the outer shrunk by the stroke towards the centre of the triangle)
            g = (cx, cy - s * 0.2)
            inner = [(px + (g[0] - px) * k / s / 1.6, py + (g[1] - py) * k / s / 1.6) for px, py in outer]
            return [poly(outer), poly(inner[::-1])]
        if new[0] == 'share':  # (a flat pill with a bar in it, as the textures)
            w, hh = s * 1.05, s * 0.48
            return [rrect(cx - w, cy - hh, cx + w, cy + hh, hh * 0.8),
                    contours(lambda p: replay(rrect(cx - w + t, cy - hh + t, cx + w - t, cy + hh - t, (hh - t) * 0.8), p, True))[0],
                    rrect(cx - w * 0.38, cy - t / 2, cx + w * 0.38, cy + t / 2, t / 2)]
        # cross: arms of half width a out to r, turned 45 degrees
        a, r = t * 0.62, s
        pl = [(a, a), (a, r), (-a, r), (-a, a), (-r, a), (-r, -a), (-a, -a), (-a, -r), (a, -r), (a, -a), (r, -a), (r, a)]
        c45 = 0.70710678
        return [poly([(cx + (x - y) * c45, cy + (x + y) * c45) for x, y in pl])]
    if isinstance(new, tuple):  # Menu / View (PlayStation's Options: three bars as Menu)
        s, t = cap_h * 0.6, cap_h * 0.16
        if new[0] in ('menu', 'options'):
            return [rrect(cx - s, cy + y - t / 2, cx + s, cy + y + t / 2, t / 2) for y in (-s * 0.7, 0, s * 0.7)]
        # View: a window outline behind a filled one
        a, b = (cx - s * 0.35, cy - s * 0.35, cx + s, cy + s), (cx - s, cy - s, cx + s * 0.35, cy + s * 0.35)
        inner = contours(lambda p: replay(rrect(a[0] + t, a[1] + t, a[2] - t, a[3] - t, t / 2), p, True))[0]
        return [rrect(*a, t), inner, rrect(*b, t)]
    if LABEL_FONT is None: LABEL_FONT = _TT(FONT)
    gs, cmap, hmtx = LABEL_FONT.getGlyphSet(), LABEL_FONT.getBestCmap(), LABEL_FONT['hmtx']
    cap = LABEL_FONT['OS/2'].sCapHeight or LABEL_FONT['head'].unitsPerEm * 0.7
    k = cap_h / cap
    width = sum(hmtx[cmap[ord(ch)]][0] for ch in new if ord(ch) in cmap) * k
    if width > max_w: k *= max_w / width; width = max_w
    out, x = [], cx - width / 2
    y = cy - cap * k / 2
    for ch in new:
        g = cmap.get(ord(ch))
        if not g: continue
        out += contours(lambda p: gs[g].draw(TransformPen(p, (k, 0, 0, k, x, y))))
        x += hmtx[g][0] * k
    return out


def relabel_font(t, style, skip=()):
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    cff = t['CFF '].cff
    top = cff.topDictIndex[0]
    priv = top.Private
    gs, cmap, hmtx = t.getGlyphSet(), t.getBestCmap(), t['hmtx']
    n = 0
    for cp, button in FONT_GLYPHS.items():
        g = cmap.get(cp)
        if not g or cp in skip: continue
        new = label(style, button)
        if new is None and style != 'keyboard': continue
        cs = contours(gs[g].draw)
        if not cs: continue
        outer = max(cs, key=lambda c: abs(area(c)))
        sign = 1 if area(outer) > 0 else -1
        x0, y0, x1, y1 = bounds([outer])
        olds = [c for c in cs if c is not outer]
        h = y1 - y0
        adv, lsb = hmtx[g]
        if new is None:  # keyboard d-pad keys bound to the arrows: the game's own arrow on a key
            new = ('keep',)
        if style == 'keyboard':
            tw = 0
            if isinstance(new, str) and len(new) > 1:
                lab = label_contours(new, 0, 0, h * 0.42, 1e9)
                tw = bounds(lab)[2] - bounds(lab)[0]
            w = max(h, tw + h * 0.45)
            body = rrect(x0, y0, x0 + w, y1, h * 0.22)
            if (area(body) > 0) != (sign > 0): body = contours(lambda p, b=body: replay(b, p, True))[0]
            adv += w - (x1 - x0)
            x1 = x0 + w
        else:
            body = outer
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        if new == ('keep',):
            labs = olds
        else:
            cap_h = h * (0.42 if isinstance(new, str) and len(new) > 2 else 0.5)
            labs = label_contours(new, cx, cy, cap_h, (x1 - x0) * 0.78)
            # holes: the opposite way round to the body (the label's own counters, the body's)
            lab_outer = max(labs, key=lambda c: abs(area(c)))
            if (area(lab_outer) > 0) == (sign > 0): labs = [contours(lambda p, c=c: replay(c, p, True))[0] for c in labs]
        pen = T2CharStringPen(None if adv == priv.defaultWidthX else adv - priv.nominalWidthX, None)
        replay(body, pen)
        for c in labs: replay(c, pen)
        top.CharStrings[g].program = pen.getCharString(private=priv, globalSubrs=cff.GlobalSubrs).program
        hmtx[g] = (round(adv), lsb)
        n += 1
    return n


def font_bytes(t):
    import io
    t['name'].names = [n for n in t['name'].names if n.nameID in (1, 2, 4, 6) and n.platformID == 3]
    try:
        import cffsubr  # (subroutines: the shared label outlines once - the fonts must fit their pack's room)
        cffsubr.subroutinize(t)
    except ImportError:
        pass
    b = io.BytesIO()
    t.save(b)
    return b.getvalue()


def trpfd_pack_size_at(d, pack_hash):
    """position of the pack's size (u64) in data.trpfd (its bytes): its packs table, by the name's FNV-1a hash"""
    fb = FB(d)
    t = fb.table(fb.u32(0))
    n, np_ = fb.vec(t[1])
    _, ip = fb.vec(t[3])
    for i in range(n):
        e = np_ + 4 * i
        s = e + fb.u32(e)
        name = d[s + 4:s + 4 + fb.u32(s)]
        h = 0xCBF29CE484222645
        for c in name: h = ((h ^ c) * 0x100000001B3) & M64
        if h == pack_hash:
            e = ip + 4 * i
            pt = fb.table(e + fb.u32(e))
            return pt[0], fb.u64(pt[0])
    raise ValueError('pack not in data.trpfd')


def build_pack(m, off, files):
    """the pack at `off` laid out anew, compactly, with `files` {entry index: new decompressed bytes} stored
    (uncompressed Oodle blocks). Flatbuffers, every reference forward: root, vtables, root table, file list, hashes,
    file tables, data."""
    fb = FB(m)
    rt = off + fb.u32(off)
    t = fb.table(rt)
    nh, hp = fb.vec(t[0])
    nf, fp = fb.vec(t[1])
    out = bytearray(4)

    def align(a, r=0):
        while len(out) % a != r: out.append(0)

    def vtable_of(tp):
        v = tp - struct.unpack_from('<i', m, tp)[0]
        return bytes(m[v:v + struct.unpack_from('<H', m, v)[0]])

    tables = []
    for j in range(nf):
        e = fp + 4 * j
        tp = e + fb.u32(e)
        ft = fb.table(tp)
        dn, dp = fb.vec(ft[4])
        data = bytes(m[dp:dp + dn])
        tb = bytearray(m[tp:tp + struct.unpack_from('<H', vtable_of(tp), 2)[0]])
        if j in files:
            data = oodle_stored(files[j])
            struct.pack_into('<Q', tb, ft[3] - tp, len(files[j]))
        tables.append((tp % 8, vtable_of(tp), tb, ft[4] - tp, data))
    vts = {}
    for vt in [vtable_of(rt)] + [x[1] for x in tables]:
        if vt not in vts:
            align(2)
            vts[vt] = len(out)
            out += vt
    align(4)
    root = len(out)
    struct.pack_into('<I', out, 0, root)
    rtb = bytearray(m[rt:rt + struct.unpack_from('<H', vtable_of(rt), 2)[0]])
    struct.pack_into('<i', rtb, 0, root - vts[vtable_of(rt)])
    out += rtb
    align(4)
    files_at = len(out)
    struct.pack_into('<I', out, root + (t[1] - rt), files_at - (root + (t[1] - rt)))
    out += struct.pack('<I', nf) + bytes(4 * nf)
    align(8, 4)
    hashes_at = len(out)
    struct.pack_into('<I', out, root + (t[0] - rt), hashes_at - (root + (t[0] - rt)))
    out += struct.pack('<I', nh) + bytes(m[hp:hp + 8 * nh])
    places = []
    for j, (mod, vt, tb, dfield, data) in enumerate(tables):
        align(8, mod)
        at = len(out)
        struct.pack_into('<i', tb, 0, at - vts[vt])
        out += tb
        struct.pack_into('<I', out, files_at + 4 + 4 * j, at - (files_at + 4 + 4 * j))
        places.append(at + dfield)
    for j, (mod, vt, tb, dfield, data) in enumerate(tables):
        align(4)
        struct.pack_into('<I', out, places[j], len(out) - places[j])
        out += struct.pack('<I', len(data)) + data
    return bytes(out)


# ------------------------------------------------------------------ main
found = {}  # hash -> (name, decoded)
bc3 = set()  # (those stored as BC3: written back label -> RGB, shape -> alpha)


listed = set()


def scan_sarc(d):
    if d[:4] != b'SARC': return
    for f in sarc_files(d):
        if f[:4] != b'BNTX': continue
        for name, w, h, fmt, bh, data in bntx_textures(f):
            if args.list and re.search(args.list, name) and name not in listed:  # (finding a game's icons by name)
                listed.add(name)
                print(f'texture {name} {w}x{h} fmt {fmt:#x} {hash_full(data):016x}')
                if args.list_dir:
                    os.makedirs(args.list_dir, exist_ok=True)
                    if fmt in (0x1e01, 0x1c01):
                        im = decode_bc5(data, w, h, bh, fmt)
                        rgb = np.zeros((h, w, 3), np.uint8)
                        rgb[:, :, 0] = (im[:, :, 0] * 255).astype(np.uint8)
                        rgb[:, :, 1] = (im[:, :, 1] * 255).astype(np.uint8)
                        Image.fromarray(rgb).save(os.path.join(args.list_dir, name.replace('^', '_') + '.png'))
            if name not in ICONS or fmt not in (0x1e01, 0x1c01): continue  # (BC5; BC3 in some mods' archives)
            hh = hash_full(data)
            if hh not in found: found[hh] = (name, decode_bc5(data, w, h, bh, fmt))
            if fmt == 0x1c01: bc3.add(hh)


icon_fonts = {}  # game files folder -> [(Entry, magic, decoded TTF) of or_font / or_font_eng]
trpfds = {}  # game files -> data.trpfd's bytes
for romfs in args.romfs:
    print('reading', romfs, flush=True)
    trpfs, trpfds[romfs] = open_game(romfs)
    icon_fonts[romfs] = []
    for ent in game_files(trpfs):
        comp, size, raw = ent.comp, ent.size, ent.raw
        try:
            d = unpack(comp, size, raw)
        except ValueError:
            continue
        if size < 0x20000 and d[:4] == b'6\xf8\x1a\x1e':  # (a small .bfttf: the icon fonts, the UI's bold condensed)
            ttf = bfttf_decode(d)
            if b'or_font' in ttf.replace(b'\0', b''): icon_fonts[romfs].append((ent, d[:4], ttf))
            elif b'UniversNextPro-BoldCond' in ttf.replace(b'\0', b''):
                import io
                from fontTools.ttLib import TTFont
                LABEL_FONT = TTFont(io.BytesIO(ttf))  # (the game's own letters for the font glyphs' labels)
            continue
        scan_sarc(d)
# the mods' UI archives (a translation's title screen has its own "PRESS (A)")
for root, _, names in os.walk(args.mods):
    for nm in names:
        if nm.endswith('.arc'):
            with open(os.path.join(root, nm), 'rb') as f: scan_sarc(f.read())
if args.list: sys.exit(0)
print(len(found), 'icon textures,', sum(len(v) for v in icon_fonts.values()), 'icon fonts')
print('labels:', {b: key(b) for b in ('a', 'b', 'x', 'y', 'l', 'r', 'zl', 'zr', 'plus', 'minus')})


def fnv64(b):  # FNV-1a 64 (runtime/services/fs.c checks the bytes a patch replaces with it)
    h = 0xCBF29CE484222645
    for x in b: h = ((h ^ x) * 0x100000001B3) & M64
    return h


def record(path, off, orig, data):
    p = path.encode()
    return struct.pack('<BH', 0, len(p)) + p + struct.pack('<QQQ', off, len(data), fnv64(orig)) + data


# glyphs left as they are when the relabelled fonts don't fit their pack's room, in this order
OPTIONAL = [(0xE33C, 0xE33D, 0xE33E, 0xE33F), (0xE338, 0xE339), (0xE325, 0xE326, 0xE327, 0xE328)]
for style in args.styles.split(','):
    d = os.path.join(args.out, style)
    os.makedirs(d, exist_ok=True)
    for f in os.listdir(d):
        if re.fullmatch(r'[0-9a-f]{16}\.png', f): os.remove(os.path.join(d, f))
    n = 0
    for hh, (name, img) in sorted(found.items()):
        out = relabel(img, name, ICONS[name].split('|'), style)
        if out is None: continue
        if hh in bc3:
            r, g, _, _ = out.split()
            out = Image.merge('RGBA', (r, r, r, g))
        out.save(os.path.join(d, '%016x.png' % hh))
        n += 1
    print(f'{d}: {n} textures')
    # The icon fonts: their pack rebuilt with them relabelled, in place of the game's in data.trpfs (romfs.patch,
    # runtime/services/fs.c). It has to fit where the pack was - the game takes a pack's size from where the next
    # one starts - so the fonts are stored subroutinized and, when still too big, some glyphs are left alone. One
    # patch file for every game version given (--romfs, more than once): the runtime applies the records whose
    # original bytes are the ones it has.
    import io
    from fontTools.ttLib import TTFont
    patch = os.path.join(d, 'romfs.patch')
    if os.path.exists(patch): os.remove(patch)
    recs = []
    for romfs, fonts in icon_fonts.items():
        if not fonts: continue
        e0 = fonts[0][0]
        for k in range(len(OPTIONAL) + 1):
            skip = {c for grp in OPTIONAL[:k] for c in grp}
            files = {}
            for ent, magic, ttf in fonts:
                t = TTFont(io.BytesIO(ttf))
                if relabel_font(t, style, skip): files[ent.index] = bfttf_encode(magic, font_bytes(t))
            pack = build_pack(e0.m, e0.pack_off, files)
            if len(pack) <= e0.slot: break
        else:
            print(f'{d}: {romfs}: the icon fonts don\'t fit their pack ({len(pack)} > {e0.slot} bytes), left as they are')
            continue
        if skip: print(f'{d}: {romfs}: (to fit, {len(skip)} icon font glyphs left as they are)')
        pack += bytes(e0.slot - len(pack))
        size_at, _ = trpfd_pack_size_at(trpfds[romfs], e0.pack_hash)
        old_size = trpfds[romfs][size_at:size_at + 8]
        recs.append(record('arc/data.trpfs', e0.pack_off, e0.m[e0.pack_off:e0.pack_off + e0.slot], pack))
        recs.append(record('arc/data.trpfd', size_at, old_size, struct.pack('<Q', len(pack))))
    if recs:
        open(patch, 'wb').write(b'ZAROMFS2' + struct.pack('<I', len(recs)) + b''.join(recs))
        print(f'{patch}: icon fonts for {len(recs) // 2} game version(s)')

if args.install:  # the finished sets in place of the old ones, in one move
    with open(os.path.join(args.out, '.made'), 'w') as f: f.write(args.install)
    shutil.rmtree(FINAL, ignore_errors=True)
    os.replace(args.out, FINAL)
    print('installed', FINAL)
