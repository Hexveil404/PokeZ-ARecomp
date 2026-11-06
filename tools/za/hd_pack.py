# Pokémon Z-A HD texture pack, straight from your own game files (no playing through to dump them): every colour
# texture (albedo, emissive, UI art) upscaled 2x by Real-ESRGAN and stored as BC7 with mipmaps in
# textures/hd/<hash>.dds, which runtime/nvn/vulkan/textures.c draws in the game's texture's place (setting tex_hd /
# F1 > Textures: Original / HD). Normal maps and masks (roughness, AO, ...) keep the game's own: an upscaler's
# "detail" in those reads as lighting noise.
#
# Stages (all three by default; each skips what an earlier run did, so the pack can be built in sessions and topped up
# after a game update or a new mod):
#   extract - decode the colour textures' top level to build/hdwork/src/<hash>.png (+ meta.tsv)
#   upscale - Real-ESRGAN 4x (realesrgan-x4plus; realesrgan-ncnn-vulkan, put in tools/hdtools/realesrgan), kept at 2x
#   blend   - with back as much of the original's fine detail as the model smoothed away (grain: asphalt, stone)
#   encode  - texconv (DirectXTex, tools/hdtools/texconv.exe): BC7 (sRGB when the game's was) with every mip level
# Needs Pillow, numpy, texture2ddecoder; ooz as for prompt_textures.py; gcc (builds hd_native.c).
# usage (from LASRecomp/): python tools/za/hd_pack.py --ooz <ooz.dll> [extract|upscale|encode ...] [--max 1024]
#                          [--only <regex on the texture name>] [--limit N]
import argparse, ctypes, os, re, shutil, subprocess, sys
import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trinity

ap = argparse.ArgumentParser()
ap.add_argument('stages', nargs='*', default=['extract', 'upscale', 'blend', 'encode'])
ap.add_argument('--ooz', help='ooz shared library (extract)')
ap.add_argument('--romfs', default='extracted/romfs')
ap.add_argument('--mods', default='mods')
ap.add_argument('--work', default='build/hdwork')
ap.add_argument('--out', default='textures/hd')
ap.add_argument('--tools', default='tools/hdtools')
ap.add_argument('--max', type=int, default=1024, help="textures bigger than this (either side) keep the game's")
ap.add_argument('--min', type=int, default=16)
ap.add_argument('--only', default=None, help='only textures whose name matches this regex')
ap.add_argument('--limit', type=int, default=0, help='extract at most this many (tests)')
ap.add_argument('--detail', type=float, default=1.0, help="how much of the fine detail the upscaler smoothed away to put back")
ap.add_argument('--force', action='store_true', help='blend (and encode) again textures already done')
args = ap.parse_args()
SRC, UP, FIN = (os.path.join(args.work, d) for d in ('src', 'up', 'final'))
META = os.path.join(args.work, 'meta.tsv')
for d in (SRC, UP, FIN, args.out): os.makedirs(d, exist_ok=True)

# ------------------------------------------------------------------ native helpers
HERE = os.path.dirname(os.path.abspath(__file__))
DLL = os.path.join(args.work, 'hd_native.dll' if os.name == 'nt' else 'hd_native.so')
NATIVE_SRC = [os.path.join(HERE, 'hd_native.c'), os.path.join(HERE, '..', '..', 'runtime', 'nvn', 'tex_hash.h')]
if not os.path.exists(DLL) or os.path.getmtime(DLL) < max(os.path.getmtime(p) for p in NATIVE_SRC):
    subprocess.check_call(['gcc', '-O2', '-shared', '-o', DLL, os.path.join(HERE, 'hd_native.c')])
nat = ctypes.CDLL(os.path.abspath(DLL))
# (the file names: the runtime's texture hash - the previous one with LARECOMP_TEX_FASTHASH=0, as the runtime)
pack_hash = nat.hash_full if os.environ.get('LARECOMP_TEX_FASTHASH', '1') == '0' else nat.pack_hash
pack_hash.restype = ctypes.c_uint64
pack_hash.argtypes = [ctypes.c_char_p, ctypes.c_uint64]
nat.deswizzle.argtypes = [ctypes.c_char_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]

# NX texture formats (high byte) -> (block width, height, bytes per block, decoder)
COLOUR = {0x1a: (4, 4, 8, 'bc1'), 0x1b: (4, 4, 16, 'bc2'), 0x1c: (4, 4, 16, 'bc3'), 0x20: (4, 4, 16, 'bc7'),
          0x0b: (1, 1, 4, 'rgba8')}
# name endings of data textures (not colour): normals, masks, lighting
DATA = re.compile(r'_(nrm|normal|n|msk|mask|lym|ao|rgn|mtl|pck|lgt|spc|specular|prb|arm|irradiancevolume|hgt|dpt|'
                  r'depth|flw|flow|noise|nse|vel|dist)(\^\w)?$', re.I)


def decode_top(t):
    """the texture's top level as an RGBA PIL image, or None"""
    bw, bh, bpb, kind = COLOUR[t.fmt >> 8]
    wb, hb = (t.w + bw - 1) // bw, (t.h + bh - 1) // bh
    row = wb * bpb
    lin = ctypes.create_string_buffer(row * hb)
    nat.deswizzle(t.data, len(t.data), lin, row, hb, t.bh)
    lin = lin.raw
    if kind == 'rgba8':
        return Image.frombytes('RGBA', (t.w, t.h), lin)
    import texture2ddecoder as td
    if kind == 'bc2': return None
    raw = getattr(td, 'decode_' + kind)(lin, wb * 4, hb * 4)  # (BGRA)
    im = Image.frombytes('RGBA', (wb * 4, hb * 4), raw, 'raw', 'BGRA')
    if kind == 'bc1':
        # texture2ddecoder's BC1 is always opaque, but a block in 3-colour mode (colour0 <= colour1) draws index 3
        # transparent: the holes of chain-link fences, railings, cut-out plants. Without it their HD version was a
        # solid sheet (the holes dark).
        c = np.frombuffer(lin, np.uint16).reshape(hb, wb, 4)
        idx = np.frombuffer(lin, np.uint32).reshape(hb, wb, 2)[..., 1]
        sel = (idx[..., None] >> (2 * np.arange(16, dtype=np.uint32))) & 3  # (texel i: x = i % 4, y = i // 4)
        clear = (sel == 3) & (c[..., 0] <= c[..., 1])[..., None]
        if clear.any():
            a = np.where(clear, 0, 255).astype(np.uint8).reshape(hb, wb, 4, 4).transpose(0, 2, 1, 3)
            im.putalpha(Image.fromarray(np.ascontiguousarray(a).reshape(hb * 4, wb * 4)))
    return im.crop((0, 0, t.w, t.h))


def looks_like_data(im):
    """normal maps (blue full, red / green around the middle; or blue empty: BC5-style XY) and flat greys"""
    a = np.asarray(im.convert('RGB').resize((32, 32)), np.float32)
    r, g, b = a[..., 0].mean(), a[..., 1].mean(), a[..., 2].mean()
    if b > 200 and abs(r - 128) < 30 and abs(g - 128) < 30: return True
    if b < 20 and 70 < r < 190 and 70 < g < 190: return True
    return False


def extract():
    done = set()
    if os.path.exists(META):
        for line in open(META, encoding='utf-8'): done.add(line.split('\t', 1)[0])
    meta = open(META, 'a', encoding='utf-8')
    oodle = trinity.Oodle(args.ooz)
    n = seen = 0
    for where, t in trinity.all_textures(args.romfs, oodle, args.mods):
        if (t.fmt >> 8) not in COLOUR or t.layers != 1 or t.depth != 1: continue
        if max(t.w, t.h) > args.max or min(t.w, t.h) < args.min or DATA.search(t.name): continue
        if args.only and not re.search(args.only, t.name): continue
        h = '%016x' % pack_hash(t.data, len(t.data))
        seen += 1
        if h in done: continue
        done.add(h)
        im = decode_top(t)
        if im is None or looks_like_data(im): continue
        im.save(os.path.join(SRC, h + '.png'))
        srgb = int((t.fmt & 0xFF) == 6)
        meta.write(f'{h}\t{t.name}\t{t.w}\t{t.h}\t{t.fmt:#x}\t{srgb}\t{where}\n')
        meta.flush()
        n += 1
        if n % 200 == 0: print(f'extract: {n} new ({seen} colour textures seen)', flush=True)
        if args.limit and n >= args.limit: break
    print(f'extract: {n} new textures ({seen} colour textures in all)')


def upscale():
    """Real-ESRGAN 4x of each new texture, kept at 2x in up/ (the blend stage mixes it with the original)"""
    exe = os.path.join(args.tools, 'realesrgan', 'realesrgan-ncnn-vulkan' + ('.exe' if os.name == 'nt' else ''))
    todo = [f for f in os.listdir(SRC) if f.endswith('.png') and not os.path.exists(os.path.join(UP, f))]
    print(f'upscale: {len(todo)} textures')
    batch, out4 = os.path.join(args.work, 'batch'), os.path.join(args.work, 'out4')
    for i in range(0, len(todo), 200):  # (in batches: a crash or a stop loses little)
        for d in (batch, out4):
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d)
        part = todo[i:i + 200]
        for f in part:
            try: os.link(os.path.join(SRC, f), os.path.join(batch, f))
            except OSError: shutil.copy(os.path.join(SRC, f), os.path.join(batch, f))
        subprocess.run([exe, '-i', batch, '-o', out4, '-n', 'realesrgan-x4plus', '-s', '4', '-f', 'png'],
                       check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for f in part:
            p4 = os.path.join(out4, f)
            if not os.path.exists(p4): print('upscale: failed', f); continue
            src = Image.open(os.path.join(SRC, f))
            Image.open(p4).convert('RGBA').resize((src.width * 2, src.height * 2), Image.LANCZOS).save(os.path.join(UP, f))
        print(f'upscale: {min(i + 200, len(todo))}/{len(todo)}', flush=True)
    for d in (batch, out4): shutil.rmtree(d, ignore_errors=True)


def blend_one(f):
    """The pack's image: the upscaler's, with back as much of the original's fine detail as it smoothed away (it
    reads grain - asphalt, stone, fabric - as noise). Alpha: the original's, smooth (the model's can ring)."""
    src = Image.open(os.path.join(SRC, f)).convert('RGBA')
    ai = Image.open(os.path.join(UP, f)).convert('RGBA')
    lz = src.resize(ai.size, Image.LANCZOS)
    a, l = np.asarray(ai, np.float32), np.asarray(lz, np.float32)
    hp_l = l - np.asarray(lz.filter(ImageFilter.GaussianBlur(1.5)), np.float32)
    hp_a = a - np.asarray(ai.filter(ImageFilter.GaussianBlur(1.5)), np.float32)
    kept = hp_a[..., :3].std() / max(hp_l[..., :3].std(), 1e-3)
    d = min(max(1.0 - kept, 0.0), 1.0) * args.detail
    out = a + d * hp_l
    out[..., 3] = l[..., 3] if src.getextrema()[3][0] < 255 else 255
    return Image.fromarray(np.clip(out + 0.5, 0, 255).astype(np.uint8), 'RGBA')


def blend():
    todo = [f for f in os.listdir(UP) if f.endswith('.png') and (args.force or not os.path.exists(os.path.join(FIN, f)))]
    print(f'blend: {len(todo)} textures')
    for i, f in enumerate(todo):
        blend_one(f).save(os.path.join(FIN, f))
        if args.force:
            dds = os.path.join(args.out, f[:-4] + '.dds')
            if os.path.exists(dds): os.remove(dds)
        if i % 500 == 499: print(f'blend: {i + 1}/{len(todo)}', flush=True)


def encode():
    srgb = {}
    if os.path.exists(META):
        for line in open(META, encoding='utf-8'):
            p = line.rstrip('\n').split('\t')
            srgb[p[0]] = p[5] == '1'
    texconv = os.path.join(args.tools, 'texconv.exe')
    todo = [f[:-4] for f in os.listdir(FIN) if f.endswith('.png') and not os.path.exists(os.path.join(args.out, f[:-4] + '.dds'))]
    print(f'encode: {len(todo)} textures')
    for s in (False, True):
        group = [h for h in todo if srgb.get(h, False) == s]
        for i in range(0, len(group), 200):
            lst = os.path.join(args.work, 'flist.txt')
            with open(lst, 'w') as f:
                for h in group[i:i + 200]: f.write(os.path.abspath(os.path.join(FIN, h + '.png')) + '\n')
            cmd = [texconv, '-nologo', '-y', '-m', '0', '-f', 'BC7_UNORM_SRGB' if s else 'BC7_UNORM', '-o', args.out,
                   '-flist', lst] + (['-srgb'] if s else [])
            subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL)
            print(f'encode: {"sRGB" if s else "linear"} {min(i + 200, len(group))}/{len(group)}', flush=True)


for st in args.stages: {'extract': extract, 'upscale': upscale, 'blend': blend, 'encode': encode}[st]()
