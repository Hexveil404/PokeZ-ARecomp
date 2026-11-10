"""F1 > Draw distance's generator (tools/za/draw_dist.py) on a synthetic scene part, no game data: only the
trinity_ModelLodSetting distance vectors and the trinity_ModelCullingSetting distances are scaled - every other byte,
other components' floats included, stays as it was, and the size is kept; a scene part's path comes back from its
pack's name and its hash (world/ik_scene/parts/common_resident_/common_resident_0.trscn)."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'za'))
import draw_dist  # noqa: E402
import lod_mod  # noqa: E402


class FB:
    """a tiny flatbuffer writer, front to back: a table is [vtable][soffset][fields]; strings, vectors and child
    tables go after their parent (uoffsets point forward), as the game's files do"""
    def __init__(s):
        s.b = bytearray(4)
        s.later = []

    def align(s, n=4):
        while len(s.b) % n: s.b.append(0)

    def put_table(s, fields):
        """fields: list of None | ('u32', v) | ('f32', v) | ('str', text) | ('fvec', [floats]) | ('bytes', b) |
        ('tvec', [tables' fields]); -> the table's position"""
        s.align()
        n = len(fields)
        vt = len(s.b)
        size = 4 + 4 * n
        s.b += struct.pack('<HH', 4 + 2 * n, size) + b'\0' * (2 * n)
        s.align()
        t = len(s.b)
        s.b += b'\0' * (4 + 4 * n)
        struct.pack_into('<i', s.b, t, t - vt)
        for i, f in enumerate(fields):
            if f is None: continue
            pos = t + 4 + 4 * i
            struct.pack_into('<H', s.b, vt + 4 + 2 * i, 4 + 4 * i)
            kind, v = f
            if kind == 'u32': struct.pack_into('<I', s.b, pos, v)
            elif kind == 'f32': struct.pack_into('<f', s.b, pos, v)
            else: s.later.append((pos, kind, v))
        return t

    def flush(s):
        while s.later:
            pos, kind, v = s.later.pop(0)
            s.align()
            at = len(s.b)
            if kind == 'str':
                e = v.encode()
                s.b += struct.pack('<I', len(e)) + e + b'\0'
            elif kind == 'fvec':
                s.b += struct.pack('<I', len(v)) + b''.join(struct.pack('<f', x) for x in v)
            elif kind == 'bytes':
                s.b += struct.pack('<I', len(v)) + v
            elif kind == 'tvec':
                s.b += struct.pack('<I', len(v)) + b'\0' * (4 * len(v))
                slots = [at + 4 + 4 * k for k in range(len(v))]
                for slot, fields in zip(slots, v):
                    t = s.put_table(fields)
                    struct.pack_into('<I', s.b, slot, t - slot)
            struct.pack_into('<I', s.b, pos, at - pos)

    def finish(s, root_fields):
        r = s.put_table(root_fields)
        struct.pack_into('<I', s.b, 0, r)
        s.flush()
        s.align(8)
        return bytes(s.b)


def component(fields):
    return FB().finish(fields)


def obj(typ, data):
    return [('str', typ), ('bytes', data), ('tvec', []), ('u32', 0x1234)]


lod = component([('str', 'chara'), ('str', '^ik_chara/model_.*?$'), ('fvec', [8.0, 15.0, 30.0]), ('fvec', [])])
cull = component([('str', 'ar001'), ('str', '^field/model/t1/t1_ar/t1_ar001/.*'), ('f32', 130.0), None, ('fvec', [])])
other = component([('str', 'chara'), ('str', '^ik_chara/.*?$'), ('fvec', [8.0, 15.0, 30.0, 65.0]), ('fvec', [])])
scene = FB().finish([('tvec', []), ('tvec', []), None, None,
                     ('tvec', [obj('trinity_ModelLodSetting', lod), obj('trinity_JointPrioritySetting', other),
                               obj('trinity_ModelCullingSetting', cull)]),
                     ('str', 'common_resident')])

out, n = draw_dist.scale_scene(scene, 2.0)
assert n == 4, n  # (3 LOD distances + 1 culling distance)
assert len(out) == len(scene)


def floats(b):
    return {o: struct.unpack_from('<f', b, o)[0] for o in range(0, len(b) - 3, 4)}


fa, fb = floats(scene), floats(out)
changed = {o for o in fa if fa[o] != fb[o]}
assert sorted(fb[o] / fa[o] for o in changed) == [2.0] * 4, [(fa[o], fb[o]) for o in changed]
assert sorted(fa[o] for o in changed) == [8.0, 15.0, 30.0, 130.0]
diff = [i for i in range(len(scene)) if scene[i] != out[i]]
assert all(any(o <= i < o + 4 for o in changed) for i in diff)  # (nothing else moved)
# the other component's identical-looking floats (8 / 15 / 30 / 65) are untouched
assert sum(1 for o in fb if fb[o] == 65.0) == 1 and sum(1 for o in fb if fb[o] == 8.0) == 1
# a scene with none of the tables: nothing to write
assert draw_dist.scale_scene(FB().finish([('tvec', []), ('tvec', []), None, None,
                                          ('tvec', [obj('trinity_JointPrioritySetting', other)])]), 2.0)[1] == 0

path = 'world/ik_scene/parts/common_resident_/common_resident_0.trscn'
assert draw_dist.scene_path('arc/worldik_scenepartscommon_resident_common_resident_0.trscn.trpak', lod_mod.fnv(path)) == path
path = 'world/ik_scene/parts/t4_graphics_setting_/t4_graphics_setting_0.trscn'
assert draw_dist.scene_path('arc/worldik_scenepartst4_graphics_setting_t4_graphics_setting_0.trscn.trpak',
                            lod_mod.fnv(path)) == path
assert draw_dist.scene_path('arc/worldik_scenepartscommon_resident_common_resident_0.trscn.trpak', 1) is None
print('PASS: draw distance - LOD vectors and culling distances scaled, every other byte kept, paths from hashes')
