"""ZA SDK - the world's scenes as a list anyone can edit, and edits turned into a mod.

  index  --romfs <romfs.bin or folder> --out world.tsv
      every trinity_SceneObject with a model in the world's scenes, one line each:
      scene path \t key \t name \t model \t px py pz qx qy qz qw sx sy sz
      (key: the object's place in its scene's tree, "12/3" = 4th child of the 13th top object - stable for that game
      version, which edits refer to)
  apply  --romfs <...> --edits edits.tsv --out <mod folder>
      edits.tsv, one per line (any order; # comments):
        move   \t scene \t key \t px py pz qx qy qz qw sx sy sz     - a new transform
        delete \t scene \t key                                       - the object and its components removed
        copy   \t scene \t key \t px py pz ... sx sy sz [\t scene2]  - a copy (components and collision too) at that
                                                                       transform, in scene2 (default: the same scene)
      -> <mod folder>/romfs/<scene path> for each scene touched: a mod (runtime/services/fs.c layers it).
The F1-free graphical editor on the start screen (runtime/ui/scene_editor.mm) reads world.tsv and writes edits.tsv;
zaprompts --scene-index / --scene-apply run these on the player's own files.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'za'))
import lod_mod  # noqa: E402
import scene as scn  # noqa: E402
import scene_writer  # noqa: E402
import trins  # noqa: E402


def world_scenes(trpfs, trpfd, unpack, log=print):
    """(path, Scene) for every world scene with objects whose path is known"""
    names = lod_mod.pack_names(trpfd)
    phash = lod_mod.pack_hashes(trpfs)
    want = {i for i, h in enumerate(phash) if '/worldik_scene' in names.get(h, '')}
    for pi, fh, d in lod_mod.pack_files(trpfs, unpack, 64 << 20):
        if pi not in want or b'trinity_SceneObject' not in d: continue
        try:
            sc = scn.Scene(d)
        except Exception as e:  # (not a scene this reader knows: left alone)
            log('skip', hex(fh), e)
            continue
        p = scn.scene_path(sc.name, fh)
        if p: yield p, sc


def keyed(sc):
    """(key, object) for every object of the scene, depth first"""
    def rec(objs, prefix):
        for i, o in enumerate(objs):
            k = f'{prefix}{i}'
            yield k, o
            yield from rec(o.children, k + '/')
    yield from rec(sc.objects, '')


def instancer_path(o):
    """a scene object's instance list (.trins) path, '' if it has no trinity_ModelInstancerComponent"""
    for c in o.children:
        if c.type == 'trinity_ModelInstancerComponent' and c.data:
            fb = scn.trinity.FB(c.data)
            t = fb.table(fb.u32(0))
            if t and t[0] is not None:
                q = t[0] + fb.u32(t[0])
                return bytes(c.data[q + 4:q + 4 + fb.u32(q)]).decode('utf-8', 'replace')
    return ''


def files_by_path(trpfs, unpack, paths):
    """{path: bytes} of these game files (by their FNV-1a 64 hash, wherever their pack is)"""
    want = {lod_mod.fnv(p): p for p in paths}
    out = {}
    if not want: return out
    for _, fh, d in lod_mod.pack_files(trpfs, unpack, 64 << 20):
        if fh in want: out[want[fh]] = bytes(d)
    return out


def fmt(tr):
    return ' '.join(f'{v:.6g}' for v in tr)


def index(trpfs, trpfd, unpack, out, log=print):
    n = scenes = 0
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# ZA SDK world index: scene path, key, name, model, transform (px py pz qx qy qz qw sx sy sz)\n')
        inst = []  # (scene path, key, name, model, .trins path)
        for path, sc in world_scenes(trpfs, trpfd, unpack, log):
            scenes += 1
            for k, o in keyed(sc):
                if o.type != 'trinity_SceneObject': continue
                mdl, tr = o.model(), o.transform()
                if not mdl or tr is None: continue
                ip = instancer_path(o)
                if ip:  # (drawn where its instances are: below)
                    inst.append((path, k, o.name(), mdl, ip))
                    continue
                f.write(f'{path}\t{k}\t{o.name()}\t{mdl}\t{fmt(tr)}\n')
                n += 1
        lists = files_by_path(trpfs, unpack, {i[4] for i in inst})
        for path, k, name, mdl, ip in inst:
            if ip not in lists: continue
            _, _, mats = trins.read(lists[ip])
            for i, mt in enumerate(mats):  # (key <object>#<instance>: the instance's place in its list)
                f.write(f'{path}\t{k}#{i}\t{name}#{i}\t{mdl}\t{fmt(trins.to_trs(mt))}\n')
                n += 1
    log(f'{n} objects in {scenes} scenes -> {out}')
    return n


def read_edits(path):
    out = []
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line.strip() or line.startswith('#'): continue
        out.append(line.split('\t'))
    return out


def apply(trpfs, trpfd, unpack, edits_path, out, log=print):
    edits = read_edits(edits_path)
    touched = {e[1] for e in edits} | {e[4] for e in edits if e[0] == 'copy' and len(e) > 4 and e[4]}
    scenes = {p: sc for p, sc in world_scenes(trpfs, trpfd, unpack, log) if p in touched}
    missing = touched - set(scenes)
    if missing: log('scenes not found (another game version?):', ', '.join(sorted(missing)))
    objs = {p: dict(keyed(sc)) for p, sc in scenes.items()}  # (keys of the original trees: before any change)
    done = 0
    # instance edits (key <object>#<n>): on the object's .trins list, written as its own file
    lists, inst_edits = {}, [e for e in edits if '#' in e[2]]
    edits = [e for e in edits if '#' not in e[2]]
    if inst_edits:
        owners = {}
        for e in inst_edits:
            k = e[2].split('#')[0]
            o = objs.get(e[1], {}).get(k)
            ip = instancer_path(o) if o is not None else ''
            if ip: owners[(e[1], k)] = ip
            else: log('no instance list for', e[2], 'in', e[1])
        for ip, d in files_by_path(trpfs, unpack, set(owners.values())).items(): lists[ip] = list(trins.read(d))
        dels = {}
        for e in inst_edits:
            k, i = e[2].split('#')
            i = int(i)
            ip = owners.get((e[1], k))
            if ip not in lists: continue
            mats = lists[ip][2]
            if i >= len(mats):
                log('no instance', e[2])
                continue
            if e[0] == 'move': mats[i] = trins.from_trs([float(x) for x in e[3].split()])
            elif e[0] == 'copy': mats.append(trins.from_trs([float(x) for x in e[3].split()]))
            elif e[0] == 'delete': dels.setdefault(ip, set()).add(i)
            done += 1
        for ip, idx in dels.items():  # (after the moves and copies: the original numbering)
            lists[ip][2] = [m for j, m in enumerate(lists[ip][2]) if j not in idx]
        for ip, (nm, ver, mats) in lists.items():
            dst = os.path.join(out, 'romfs', ip)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as f: f.write(trins.write(nm, ver, mats))
    copies, deletes = [], []
    for e in edits:
        op, sp, key = e[0], e[1], e[2]
        o = objs.get(sp, {}).get(key)
        if o is None:
            log('no object', key, 'in', sp)
            continue
        if op == 'move':
            sc = scenes[sp]
            tr = [float(x) for x in e[3].split()]
            at = o.transform_at()
            import struct
            struct.pack_into('<10f', o.data, at, *tr)
            done += 1
        elif op == 'copy':
            copies.append((o, sp, [float(x) for x in e[3].split()], e[4] if len(e) > 4 and e[4] else sp))
        elif op == 'delete':
            deletes.append((o, sp))
    for o, sp, tr, dst in copies:  # (copies before deletes: a copy of a deleted object still works)
        y = scenes[sp].duplicate(o, tr)
        if dst != sp:  # (into another scene: off this one's tree, onto the other's top level)
            scenes[sp].remove(y)
            y.parent = None
            scenes[dst].objects.append(y)
        done += 1
    for o, sp in deletes:
        scenes[sp].remove(o)
        done += 1
    for p in {e[1] for e in edits} | {c[3] for c in copies}:  # (scene edits: instance ones are in lists)
        if p not in scenes: continue
        dst = os.path.join(out, 'romfs', p)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f: f.write(scene_writer.write(scenes[p]))
    log(f'{done} of {len(edits) + len(inst_edits)} edits applied ({len(lists)} instance lists) -> {out}')
    return done


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['index', 'apply'])
    ap.add_argument('--romfs', required=True)
    ap.add_argument('--ooz', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--edits')
    a = ap.parse_args()
    import mmap
    import trinity
    with open(os.path.join(a.romfs, 'arc', 'data.trpfs'), 'rb') as f:
        m = memoryview(mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ))
    with open(os.path.join(a.romfs, 'arc', 'data.trpfd'), 'rb') as f:
        fd = f.read()
    u = trinity.Oodle(a.ooz).unpack
    if a.cmd == 'index': index(m, fd, u, a.out)
    else: apply(m, fd, u, a.edits, a.out)
