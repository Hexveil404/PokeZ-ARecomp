"""ZA SDK - a scene (scene.Scene) written back as a .trscn flatbuffer, front to back: each table is preceded by its
vtable (soffset > 0), and everything a table points at (strings, vectors, child tables) comes after it, as uoffsets
must point forward. Layouts, as the game's own files have them (checked on all 2058 world scenes of 2.0.0):
  root   [soffset][flag u8 @4]...[name @8][layer @12][objects @16][scene refs @20]   (size 24)
  object [soffset][type @4][data @8][children @12][id u64 @16]                       (size 24, 8-aligned)
A component's data (a nested flatbuffer) is copied byte for byte into a [ubyte] vector whose contents start 16-aligned.
"""
import struct


class _W:
    def __init__(s):
        s.b = bytearray()
        s.todo = []  # (slot, kind, value): pointed-at things still to place

    def pad(s, n, rem=0):
        while len(s.b) % n != rem: s.b.append(0)

    def table(s, size, fields, align=4):
        """fields: [(vtable index, offset in the table, kind, value)]; -> the table's position"""
        nslots = max(i for i, _, _, _ in fields) + 1 if fields else 0
        vt = bytearray(struct.pack('<HH', 4 + 2 * nslots, size) + b'\0' * (2 * nslots))
        for i, off, _, _ in fields: struct.pack_into('<H', vt, 4 + 2 * i, off)
        # the table must start aligned; the vtable sits right before it (its length is even)
        s.pad(2)
        while (len(s.b) + len(vt)) % align: s.b += b'\0\0'
        v = len(s.b)
        s.b += vt
        t = len(s.b)
        s.b += b'\0' * size
        struct.pack_into('<i', s.b, t, t - v)
        for _, off, kind, val in fields:
            p = t + off
            if kind == 'u8': s.b[p] = val
            elif kind == 'u64': struct.pack_into('<Q', s.b, p, val)
            else: s.todo.append((p, kind, val))
        return t

    def place(s, p, kind, val):
        if kind == 'str':
            s.pad(4)
            at = len(s.b)
            e = val.encode('utf-8') if isinstance(val, str) else bytes(val)
            s.b += struct.pack('<I', len(e)) + e + b'\0'
        elif kind == 'blob':
            s.pad(16, 12)  # (the contents 16-aligned)
            at = len(s.b)
            s.b += struct.pack('<I', len(val)) + bytes(val)
        elif kind == 'strvec':
            s.pad(4)
            at = len(s.b)
            s.b += struct.pack('<I', len(val)) + b'\0' * (4 * len(val))
            for k, x in enumerate(val): s.todo.append((at + 4 + 4 * k, 'str', x))
        elif kind == 'objvec':
            s.pad(4)
            at = len(s.b)
            s.b += struct.pack('<I', len(val)) + b'\0' * (4 * len(val))
            for k, o in enumerate(val): s.todo.append((at + 4 + 4 * k, 'obj', o))
        elif kind == 'obj':
            at = s.obj(val)
        struct.pack_into('<I', s.b, p, at - p)

    def obj(s, o):
        f = [(0, 4, 'str', o.type)]
        if o.data is not None: f.append((1, 8, 'blob', o.data))
        if o.children or getattr(o, 'kids_field', True): f.append((2, 12, 'objvec', o.children))
        if o.id is not None: f.append((3, 16, 'u64', o.id))
        return s.table(24, f, align=8)

    def drain(s):
        # breadth by kind of thing, nearest first: a todo placed may add more
        while s.todo:
            p, kind, val = s.todo.pop(0)
            s.place(p, kind, val)


def write(scene):
    w = _W()
    w.b += b'\0' * 4  # (the root's offset)
    f = [(0, 8, 'str', scene.name)]
    if scene.layer is not None: f.append((1, 12, 'str', scene.layer))
    f.append((4, 16, 'objvec', scene.objects))
    if scene.refs is not None: f.append((5, 20, 'strvec', scene.refs))
    if scene.flag is not None: f.append((6, 7, 'u8', scene.flag))
    r = w.table(24, f, align=8)
    struct.pack_into('<I', w.b, 0, r)
    w.drain()
    w.pad(16)
    return bytes(w.b)
