"""ZA SDK - Pokémon Z-A's world scenes (.trscn), read and edited.

A scene is a flatbuffer: root {0: name, 4: objects, 5: [...], 6: flag}; an object {0: type, 1: data - a nested
flatbuffer with the component's own fields, 2: children (objects), 3: id}. The world is made of trinity_SceneObject
objects (data {0: name, 1: transform [px py pz qx qy qz qw sx sy sz], ...}) whose children are components:
trinity_ModelComponent (data {0: model path .trmdl, 1: name, 5..15: LOD distances, ...}), trinity_CollisionComponent,
trinity_ModelInstancerComponent, ...

Two kinds of edits:
- in place (Scene.set_transform): the 10 floats of an object's transform are rewritten in the file's own bytes -
  move, rotate, scale, and "hide" (scale 0) need nothing else;
- structural (Scene.remove / Scene.duplicate, then Scene.build): the object tree is written again (scene_writer),
  the nested data blobs copied as they are.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'za'))
import trinity  # noqa: E402
import lod_mod  # noqa: E402


class Obj:
    __slots__ = ('type', 'data', 'children', 'id', 'pos', 'parent', 'kids_field')

    def __init__(s, type, data, children, id, pos=None, parent=None):
        s.type, s.data, s.children, s.id, s.pos, s.parent = type, data, children, id, pos, parent
        s.kids_field = True

    # ---- a trinity_SceneObject's own fields (its data blob)
    def _fb(s):
        fb = trinity.FB(s.data)
        return fb, fb.table(fb.u32(0))

    def name(s):
        fb, t = s._fb()
        if not t or t[0] is None: return ''
        q = t[0] + fb.u32(t[0])
        return bytes(s.data[q + 4:q + 4 + fb.u32(q)]).decode('utf-8', 'replace')

    def transform_at(s):
        """offset of the 10 transform floats inside data (None: not a SceneObject with one)"""
        if s.type != 'trinity_SceneObject': return None
        fb, t = s._fb()
        if len(t) < 2 or t[1] is None: return None
        q = t[1] + fb.u32(t[1])
        if fb.u32(q) != 10: return None
        return q + 4

    def transform(s):
        o = s.transform_at()
        return None if o is None else list(struct.unpack_from('<10f', s.data, o))

    def model(s):
        """the first trinity_ModelComponent child's model path ('' if none)"""
        for c in s.children:
            if c.type == 'trinity_ModelComponent':
                fb = trinity.FB(c.data)
                t = fb.table(fb.u32(0))
                if t and t[0] is not None:
                    q = t[0] + fb.u32(t[0])
                    return bytes(c.data[q + 4:q + 4 + fb.u32(q)]).decode('utf-8', 'replace')
        return ''


class Scene:
    def __init__(s, data):
        s.raw = bytearray(data)
        fb = trinity.FB(s.raw)
        s.root = fb.table(fb.u32(0))
        g = lambda i: s.root[i] if len(s.root) > i else None
        s.name = s._str(fb, g(0))
        s.layer = s._str(fb, g(1)) if g(1) is not None else None  # ("---- Default ----")
        s.flag = s.raw[g(6)] if g(6) is not None else None
        s.refs = None  # (the scenes this one pulls in: relative paths)
        if g(5) is not None:
            n, p = fb.vec(g(5))
            s.refs = [s._str(fb, p + 4 * k) for k in range(n)]
        s.objects = s._objs(fb, g(4), None)

    def _str(s, fb, slot):
        q = slot + fb.u32(slot)
        return bytes(s.raw[q + 4:q + 4 + fb.u32(q)]).decode('utf-8', 'surrogateescape')

    def _objs(s, fb, field, parent):
        out = []
        if field is None: return out
        n, p = fb.vec(field)
        for i in range(n):
            e = p + 4 * i
            t = fb.table(e + fb.u32(e))
            g = lambda k: t[k] if len(t) > k else None
            typ = s._str(fb, g(0))
            data, vp = None, None
            if g(1) is not None:
                vn, vp = fb.vec(g(1))
                data = bytearray(s.raw[vp:vp + vn])
            oid = struct.unpack_from('<Q', s.raw, g(3))[0] if g(3) is not None else None
            o = Obj(typ, data, [], oid, vp, parent)
            o.kids_field = g(2) is not None
            o.children = s._objs(fb, g(2), o)
            out.append(o)
        return out

    def walk(s, objs=None):
        for o in (s.objects if objs is None else objs):
            yield o
            yield from s.walk(o.children)

    # ---- in-place edits (the file keeps its layout)
    def set_transform(s, o, tr):
        at = o.transform_at()
        assert at is not None, 'no transform'
        struct.pack_into('<10f', o.data, at, *tr)
        struct.pack_into('<10f', s.raw, o.pos + at, *tr)  # (the same bytes in the file)

    def bytes_in_place(s):
        return bytes(s.raw)

    # ---- structural edits
    def remove(s, o):
        (o.parent.children if o.parent else s.objects).remove(o)

    def duplicate(s, o, tr=None, name_suffix=None):
        """a deep copy of o (and its components) next to it; tr: its transform"""
        def copy(x, parent):
            y = Obj(x.type, None if x.data is None else bytearray(x.data), [], x.id, None, parent)
            y.kids_field = x.kids_field
            y.children = [copy(c, y) for c in x.children]
            return y
        y = copy(o, o.parent)
        lst = o.parent.children if o.parent else s.objects
        lst.insert(lst.index(o) + 1, y)
        if tr is not None:
            at = y.transform_at()
            struct.pack_into('<10f', y.data, at, *tr)
        return y

    def build(s):
        """the scene written again from the object tree (scene_writer)"""
        import scene_writer
        return scene_writer.write(s)


def scene_path(scene_name, fh, packs=()):
    """a scene file's path from its own name (root field 0) and its FNV-1a 64 hash: world/ik_scene/<group>/<name>_/
    <name>_0.trscn; the group (field/unity_output, parts, ...) from a pack name when given, else the usual ones"""
    groups = ['field/unity_output', 'parts', 'field', 'field/area', 'system', 'effect']
    for g in groups:
        for folder in (scene_name + '_', scene_name):
            for i in range(4):
                p = f'world/ik_scene/{g}/{folder}/{scene_name}_{i}.trscn'
                if lod_mod.fnv(p) == fh: return p
    return None
