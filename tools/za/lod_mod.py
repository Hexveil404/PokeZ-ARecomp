# Makes mods/MaxLOD (runtime/services/fs.c layers it over the game's files): Pokémon Z-A's models always at their
# full detail. A model (.trmdl, flatbuffers) lists its meshes - the full one and its simpler LODs (<name>_lod1.trmsh,
# _lod2, ...) - and its LOD levels pick among them by distance; here every mesh slot is pointed at the full mesh
# (the list's entries are offsets: the file keeps its size and everything else). Costs GPU time and memory with many
# characters on screen; drop the folder (or rename it _MaxLOD) to go back.
#
# A loose file needs its path: the game's file list keeps only their FNV-1a 64 hashes, so each model's path is found
# from its pack's name (arc/<folder ...>.trpak) and the model's own name, checked against the hash.
# usage (from LASRecomp/): python tools/za/lod_mod.py --ooz <ooz.dll> [--romfs extracted/romfs] [--out mods/MaxLOD]
# Also a module: make() on the game's data.trpfs / data.trpfd bytes - prompt_textures.py --maxlod (zaprompts, F1 >
# Full-detail models: runtime/ui/launcher.mm launcher_maxlod_kick) runs it on the player's romfs.bin.
import argparse, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trinity

M64 = (1 << 64) - 1


def fnv(s):
    h = 0xCBF29CE484222645
    for c in s.encode(): h = ((h ^ c) * 0x100000001B3) & M64
    return h


def pack_names(d):
    """pack hash -> name (arc/....trpak), from data.trpfd's bytes"""
    fb = trinity.FB(d)
    t = fb.table(fb.u32(0))
    n, p = fb.vec(t[1])
    out = {}
    for i in range(n):
        e = p + 4 * i
        s = e + fb.u32(e)
        name = d[s + 4:s + 4 + fb.u32(s)].decode()
        out[fnv(name)] = name
    return out


def pack_hashes(m):
    fb = trinity.FB(m)
    root = fb.u64(8)
    n, hp = fb.vec(fb.table(root + fb.u32(root))[0])
    return [fb.u64(hp + 8 * i) for i in range(n)]


def full_lod(d, levels=0):
    """the model with its mesh slots on more detailed meshes (--levels), or None if it has no LODs"""
    fb = trinity.FB(d)
    root = fb.u32(0)
    t = fb.table(root)
    if len(t) < 2 or t[1] is None: return None
    n, p = fb.vec(t[1])
    if n < 2: return None
    names = []
    for i in range(n):
        e = p + 4 * i
        mt = e + fb.u32(e)
        f = fb.table(mt)
        if not f or f[0] is None: return None
        s = f[0] + fb.u32(f[0])
        names.append((mt, d[s + 4:s + 4 + fb.u32(s)].decode('latin1')))
    level = {mt: int(m.group(1)) if (m := re.search(r'_lod(\d+)\.trmsh$', nm)) else 0 for mt, nm in names}
    by_level = {lv: mt for mt, lv in level.items()}
    if 0 not in by_level or len(by_level) < 2: return None
    out = bytearray(d)
    for i in range(n):
        e = p + 4 * i
        mt = e + fb.u32(e)
        want = max(level[mt] - levels, 0) if levels else 0  # (the nearest more detailed version there is)
        while want not in by_level: want -= 1
        if by_level[want] <= e: return None  # (an offset must point forward)
        struct.pack_into('<I', out, e, by_level[want] - e)
    return bytes(out), names


def slashes(folder, tail, h, most=4):
    """folder (its slashes left out) + tail, with up to `most` slashes put back so that its FNV-1a 64 is h"""
    from itertools import combinations
    cuts = [i for i in range(1, len(folder)) if folder[i - 1] != '_' and folder[i] != '_']
    for r in range(0, most + 1):
        for cs in combinations(cuts, r):
            s, prev = [], 0
            for c in cs:
                s.append(folder[prev:c])
                prev = c
            s.append(folder[prev:])
            p = '/'.join(s) + tail
            if fnv(p) == h: return p
    return None


def pack_files(m, unpack, max_size):
    """(pack index, file hash, bytes) of data.trpfs's files (in memory: a folder's file or inside romfs.bin)"""
    fb = trinity.FB(m)
    assert bytes(m[:8]) == b'ONEPACK\0', 'not a ONEPACK file'
    root = fb.u64(8)
    n, op = fb.vec(fb.table(root + fb.u32(root))[1])
    for i in range(n):
        off = fb.u64(op + 8 * i)
        t = fb.table(off + fb.u32(off))
        nh, hp = fb.vec(t[0])
        nf, fp = fb.vec(t[1])
        for j in range(nf):
            e = fp + 4 * j
            ft = fb.table(e + fb.u32(e))
            comp = m[ft[1]] if len(ft) > 1 and ft[1] else 0xFF
            size = fb.u64(ft[3]) if len(ft) > 3 and ft[3] else 0
            dn, dp = fb.vec(ft[4])
            if size > max_size: continue
            try:
                yield i, fb.u64(hp + 8 * j), unpack(comp, size, m[dp:dp + dn])
            except ValueError:
                continue


def make(trpfs, trpfd, unpack, out, levels=0, log=print):
    """out/romfs/<model path>.trmdl for every model with LODs; -> (made, without a known path)"""
    names = pack_names(trpfd)
    phash = pack_hashes(trpfs)
    done = miss = 0
    for pi, fh, d in pack_files(trpfs, unpack, 1 << 20):
        if len(d) > 100000 or b'.trmsh' not in d or b'_lod' not in d: continue
        r = full_lod(d, levels)
        if not r: continue
        data, meshes = r
        model = re.sub(r'\.trmsh$', '', [nm for _, nm in meshes if '_lod' not in nm][0])
        pack = names.get(phash[pi], '')
        # the folder: the pack's name without arc/ and .trpak, its parts joined by / (or the pack name's own slashes)
        base = re.sub(r'^arc/', '', re.sub(r'\.trpak$', '', pack))
        cands = []
        for b in (base, base.replace('_', '/')):
            parts = b.split('/')
            for k in range(len(parts), 0, -1):
                folder = '/'.join(parts[:k])
                cands += [f'{folder}/{model}.trmdl', f'{folder}/{model}/{model}.trmdl', f'{folder}/mdl/{model}.trmdl']
        m = re.match(r'(pm\d{4})_', model)
        if m: cands.insert(0, f'ik_pokemon/data/{m.group(1)}/{model}/{model}.trmdl')
        path = next((c for c in cands if fnv(c) == fh), None)
        if not path:
            # a pack is named after one of its files' path with the slashes left out: the folder is that name without
            # the file at its end; the slashes go back where the hash says
            mm = re.match(r'(.*)' + re.escape(model) + r'\.[a-z]+$', base)
            if mm: path = slashes(mm.group(1), f'/{model}.trmdl', fh)
            if not path:  # (named after another file of the folder: any tail like name.ext, fewer slashes tried)
                for k in range(len(base) - 3, max(len(base) - 40, 0), -1):
                    if not re.fullmatch(r'[a-z0-9_]+\.[a-z]+', base[k:]) or not re.match(r'[a-z0-9]', base[k]): continue
                    path = slashes(base[:k], f'/{model}.trmdl', fh, most=3)
                    if path: break
        if not path:
            miss += 1
            if miss <= 15: log('no path for', model, 'pack', pack)
            continue
        dst = os.path.join(out, 'romfs', path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, 'wb').write(data)
        done += 1
    log(f'{done} models at full detail in {out}, {miss} without a known path')
    return done, miss


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--ooz', required=True)
    ap.add_argument('--romfs', default='extracted/romfs')
    ap.add_argument('--out', default='mods/MaxLOD')
    ap.add_argument('--levels', type=int, default=0, help='0: always the full mesh; N: each LOD N levels more detailed')
    args = ap.parse_args()
    import mmap
    with open(os.path.join(args.romfs, 'arc', 'data.trpfs'), 'rb') as f:
        m = memoryview(mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ))
    with open(os.path.join(args.romfs, 'arc', 'data.trpfd'), 'rb') as f:
        fd = f.read()
    make(m, fd, trinity.Oodle(args.ooz).unpack, args.out, args.levels)
