# The player's game files checked pack by pack against the known good copies of each version (tools/za/pack_hashes.py:
# a 4-byte BLAKE2b digest of every pack in arc/data.trpfs, made by this file's --make from our own clean files - no game
# data). Why: a damaged download of the 2.0.2 update went around (#173 #206 #223 #233 #248 #294 #301): Scraggy's model
# pack (arc/ik_pokemon...pm0559_00_00_base.trmdt.trpak) has a broken Oodle block, and the game crashes the first time it
# loads Scraggy (the lab exit, the sewers, Acris's battle...) - f_7001e569e0 reading a model part that never loaded.
# zaimport checks the packages' own hashes since v0.1.24, but files set up before were never checked: the start screen
# runs this once for each data.trpfs (zaprompts --verify, runtime/ui/launcher.mm launcher_verify_kick) and says which
# packs differ. A few seconds on an SSD (the whole 3-5 GB read once).
import base64, hashlib, struct, sys


def packs(trpfs):
    """[(pack hash, start, end)] of data.trpfs, in its own order (the hash vector's)"""
    u32 = lambda p: struct.unpack_from('<I', trpfs, p)[0]
    u64 = lambda p: struct.unpack_from('<Q', trpfs, p)[0]
    ind = lambda p: p + u32(p)
    if bytes(trpfs[:8]) != b'ONEPACK\0': raise ValueError('not a ONEPACK file')
    root = u64(8)
    r0 = ind(root)
    vt = r0 - struct.unpack_from('<i', trpfs, r0)[0]
    f0, f1 = struct.unpack_from('<HH', trpfs, vt + 4)
    hv, ov = ind(r0 + f0), ind(r0 + f1)
    n = u32(hv)
    offs = [u64(ov + 4 + 8 * i) for i in range(n)]
    ends = sorted(set(offs)) + [root if root > max(offs) else len(trpfs)]
    nxt = {o: ends[k + 1] for k, o in enumerate(ends[:-1])}
    return [(u64(hv + 4 + 8 * i), offs[i], nxt[offs[i]]) for i in range(n)]


def digests(trpfs, progress=None):
    out = bytearray()
    ps = packs(trpfs)
    for k, (_, a, b) in enumerate(ps):
        out += hashlib.blake2b(trpfs[a:b], digest_size=4).digest()
        if progress and k % 500 == 0: progress(k, len(ps))
    return bytes(out), ps


def check(trpfs, names=None, log=print):
    """-> (version or None, [(pack index, pack name)] that differ from the known copy)"""
    import pack_hashes
    known = pack_hashes.VERSIONS.get(len(trpfs))
    if not known:
        log(f'data.trpfs {len(trpfs)} bytes: not a version with a known copy (1.0.0, 2.0.0, 2.0.2) - not checked')
        return None, []
    version, b64 = known
    want = base64.b64decode(b64)
    got, ps = digests(trpfs)
    if len(got) != len(want):
        log(f'{version}: {len(got) // 4} packs, the known copy has {len(want) // 4}')
        return version, [(-1, 'the pack list')]
    bad = [i for i in range(len(ps)) if got[4 * i:4 * i + 4] != want[4 * i:4 * i + 4]]
    return version, [(i, (names or {}).get(ps[i][0], '%016x' % ps[i][0])) for i in bad]


if __name__ == '__main__':  # --make: pack_hashes.py from clean copies: verify_files.py --make out.py <ver>=<romfs> ...
    import os, mmap
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'prompt_textures.py'), encoding='utf-8').read()
    i = src.index('def open_game'); j = src.index('\ndef ', i + 10)
    g = {'os': os, 'struct': struct, 'mmap': mmap}
    exec(src[i:j], g)
    assert sys.argv[1] == '--make'
    lines = ['# Made by tools/za/verify_files.py --make from clean copies of the game: a 4-byte BLAKE2b digest of each',
             '# pack of arc/data.trpfs, in its order, by data.trpfs size. No game data.', 'VERSIONS = {']
    for a in sys.argv[3:]:
        ver, path = a.split('=', 1)
        trpfs, _ = g['open_game'](path)
        d, ps = digests(trpfs)
        print(ver, len(trpfs), len(ps), 'packs')
        b = base64.b64encode(d).decode()
        lines.append(f'    {len(trpfs)}: ({ver!r},')
        lines += [f'        {b[k:k + 100]!r}' for k in range(0, len(b), 100)]
        lines[-1] += '),'
    lines.append('}')
    open(sys.argv[2], 'w', newline='\n').write('\n'.join(lines) + '\n')
