# Pokémon Z-A's draw distances, scaled (F1 > Draw distance, #190): the world's scene parts carry per-model tables
#   trinity_ModelLodSetting     {name, path regex, [3 distances in m]}  - the game's own LOD steps: the field's models
#                                50 / 100 / 150, Pokémon 50 / 100 / 150, the people (ik_chara) 8 / 15 / 30
#   trinity_ModelCullingSetting {name, path regex, distance in m}       - beyond it a model isn't drawn (65 / 130 here)
# (scene files, .trscn: a flatbuffer whose objects are {type, data: a nested flatbuffer, children, hash}; the tables
# sit in common_resident and the areas' *_graphics_setting / *_lod_setting parts). make() writes those files with the
# distances times the factor, everything else byte for byte, as loose files under out/romfs at their own paths
# (runtime/services/fs.c layers them; the game's data.trpfd is served without them). Nothing of the game is shipped:
# prompt_textures.py --drawdist (zaprompts) runs this on the player's own files.
# usage (from LASRecomp/): python tools/za/draw_dist.py --ooz <ooz.dll> --romfs extracted/romfs --factor 2 --out <dir>
import argparse, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trinity
import lod_mod

TYPES = (b'trinity_ModelLodSetting', b'trinity_ModelCullingSetting')


def scale_scene(d, factor):
    """d (a .trscn) with its LOD / culling distances times factor -> (bytes, distances changed); the size is kept"""
    out = bytearray(d)
    fb = trinity.FB(d)
    root = fb.table(fb.u32(0))
    if len(root) < 5 or root[4] is None: return None, 0
    n, p = fb.vec(root[4])
    changed = 0
    for i in range(n):
        e = p + 4 * i
        t = fb.table(e + fb.u32(e))
        if len(t) < 2 or t[0] is None or t[1] is None: continue
        q = t[0] + fb.u32(t[0])
        typ = bytes(d[q + 4:q + 4 + fb.u32(q)])
        if typ not in TYPES: continue
        vn, vp = fb.vec(t[1])
        sub = trinity.FB(d[vp:vp + vn])
        ct = sub.table(sub.u32(0))
        if len(ct) < 3 or ct[2] is None: continue
        if typ == b'trinity_ModelLodSetting':  # field 2: vector<float> of distances
            dq = ct[2] + sub.u32(ct[2])
            cnt = sub.u32(dq)
            if not 0 < cnt <= 8: continue
            for k in range(cnt):
                o = vp + dq + 4 + 4 * k
                v = struct.unpack_from('<f', d, o)[0]
                if 0 < v < 100000: struct.pack_into('<f', out, o, v * factor); changed += 1
        else:  # field 2: float distance
            o = vp + ct[2]
            v = struct.unpack_from('<f', d, o)[0]
            if 0 < v < 100000: struct.pack_into('<f', out, o, v * factor); changed += 1
    return bytes(out), changed


def scene_path(pack, fh):
    """a scene part's path from its pack's name (its path, the slashes left out) and its FNV-1a 64 hash: the file is
    some tail name_<n>.trscn, the slashes go back into the rest where the hash says
    (world/ik_scene/parts/common_resident_/common_resident_0.trscn)"""
    base = re.sub(r'^arc/', '', re.sub(r'\.trpak$', '', pack))
    for k in range(len(base) - 8, 0, -1):
        if not re.fullmatch(r'[a-z0-9][a-z0-9_]*_\d+\.trscn', base[k:]): continue
        path = lod_mod.slashes(base[:k], '/' + base[k:], fh, most=4)
        if path: return path
    return None


def make(trpfs, trpfd, unpack, out, factor, log=print):
    """out/romfs/<scene part>.trscn for every scene part with the tables; -> (files, distances, without a path)"""
    names = lod_mod.pack_names(trpfd)
    phash = lod_mod.pack_hashes(trpfs)
    want = {i for i, h in enumerate(phash) if 'scene' in names.get(h, '') and
            re.search(r'graphics_setting|lod_setting|common_resident', names.get(h, ''))}
    files = dists = miss = 0
    for pi, fh, d in lod_mod.pack_files(trpfs, unpack, 4 << 20):
        if pi not in want or not any(t in d for t in TYPES): continue
        data, n = scale_scene(d, factor)
        if not n: continue
        path = scene_path(names[phash[pi]], fh)
        if not path:
            miss += 1
            log('no path for', names[phash[pi]])
            continue
        dst = os.path.join(out, 'romfs', path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, 'wb').write(data)
        files += 1
        dists += n
        log(f'{path}: {n} distances x{factor:g}')
    log(f'{files} scene parts, {dists} distances x{factor:g} in {out}, {miss} without a known path')
    return files, dists, miss


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--ooz', required=True)
    ap.add_argument('--romfs', default='extracted/romfs')
    ap.add_argument('--factor', type=float, default=2)
    ap.add_argument('--out', default='mods/DrawDistance')
    args = ap.parse_args()
    import mmap
    with open(os.path.join(args.romfs, 'arc', 'data.trpfs'), 'rb') as f:
        m = memoryview(mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ))
    with open(os.path.join(args.romfs, 'arc', 'data.trpfd'), 'rb') as f:
        fd = f.read()
    make(m, fd, trinity.Oodle(args.ooz).unpack, args.out, args.factor)
