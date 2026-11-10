"""ZA SDK - instance lists (.trins): where a trinity_ModelInstancerComponent draws its model (most of Lumiose's
buildings: one ins_root_<model> scene object per model, its instances in <scene folder>/ins_root_<model>_..._X00_Z00.trins).

Layout (a flatbuffer): root {0: name "SrtInstanceBuffer", 1: u32 (1), 2: u32 instance count, 4: table {0: [ubyte]
the instances: count x 4x4 float matrices, column-major - the translation in floats 12..14}}.
"""
import math
import struct


def read(d):
    """-> (name, version, [16-float matrices])"""
    rp = struct.unpack_from('<I', d, 0)[0]
    vt = rp - struct.unpack_from('<i', d, rp)[0]
    vl = struct.unpack_from('<H', d, vt)[0]
    offs = [struct.unpack_from('<H', d, vt + 4 + 2 * i)[0] for i in range((vl - 4) // 2)]
    f = lambda i: rp + offs[i] if i < len(offs) and offs[i] else None
    q = f(0) + struct.unpack_from('<I', d, f(0))[0]
    name = bytes(d[q + 4:q + 4 + struct.unpack_from('<I', d, q)[0]]).decode()
    ver = struct.unpack_from('<I', d, f(1))[0] if f(1) else 0
    count = struct.unpack_from('<I', d, f(2))[0] if f(2) else 0
    it = f(4) + struct.unpack_from('<I', d, f(4))[0]  # (the inner table)
    ivt = it - struct.unpack_from('<i', d, it)[0]
    io = struct.unpack_from('<H', d, ivt + 4)[0]
    bp = it + io + struct.unpack_from('<I', d, it + io)[0]
    n = struct.unpack_from('<I', d, bp)[0]
    assert n == count * 64, (n, count)
    mats = [list(struct.unpack_from('<16f', d, bp + 4 + 64 * k)) for k in range(count)]
    return name, ver, mats


def write(name, ver, mats):
    """the same layout as the game's files: root table, inner table, the matrices 16-aligned, then the name"""
    b = bytearray(struct.pack('<I', 0))
    # root vtable (fields 0, 1, 2, -, 4) and table
    while (len(b) + 14) % 4: b += b'\0'
    vt = len(b)
    b += struct.pack('<HH5H', 14, 20, 4, 8, 12, 0, 16)
    while len(b) % 4: b += b'\0'
    rt = len(b)
    b += struct.pack('<iIIII', rt - vt, 0, ver, len(mats), 0)
    struct.pack_into('<I', b, 0, rt)
    # inner table {0: blob}
    ivt = len(b)
    b += struct.pack('<HHH', 6, 8, 4)
    while len(b) % 4: b += b'\0'
    it = len(b)
    b += struct.pack('<iI', it - ivt, 0)
    struct.pack_into('<I', b, rt + 16, it - (rt + 16))
    while len(b) % 16 != 12: b += b'\0'  # (the matrices 16-aligned)
    bp = len(b)
    b += struct.pack('<I', 64 * len(mats)) + b''.join(struct.pack('<16f', *m) for m in mats)
    struct.pack_into('<I', b, it + 4, bp - (it + 4))
    while len(b) % 4: b += b'\0'
    sp = len(b)
    e = name.encode()
    b += struct.pack('<I', len(e)) + e + b'\0'
    struct.pack_into('<I', b, rt + 4, sp - (rt + 4))
    while len(b) % 16: b += b'\0'
    return bytes(b)


# ---- a matrix as position / rotation (quaternion) / scale, the scene objects' transform form (and back)
def to_trs(m):
    sx = math.sqrt(m[0] ** 2 + m[1] ** 2 + m[2] ** 2)
    sy = math.sqrt(m[4] ** 2 + m[5] ** 2 + m[6] ** 2)
    sz = math.sqrt(m[8] ** 2 + m[9] ** 2 + m[10] ** 2)
    if sx < 1e-9 or sy < 1e-9 or sz < 1e-9: return [m[12], m[13], m[14], 0, 0, 0, 1, 0, 0, 0]
    r = [m[0] / sx, m[1] / sx, m[2] / sx, m[4] / sy, m[5] / sy, m[6] / sy, m[8] / sz, m[9] / sz, m[10] / sz]
    # column-major rotation r[col*3+row] -> quaternion
    m00, m10, m20, m01, m11, m21, m02, m12, m22 = r
    tr = m00 + m11 + m22
    if tr > 0:
        s = math.sqrt(tr + 1) * 2; w = s / 4; x = (m21 - m12) / s; y = (m02 - m20) / s; z = (m10 - m01) / s
    elif m00 > m11 and m00 > m22:
        s = math.sqrt(1 + m00 - m11 - m22) * 2; w = (m21 - m12) / s; x = s / 4; y = (m01 + m10) / s; z = (m02 + m20) / s
    elif m11 > m22:
        s = math.sqrt(1 + m11 - m00 - m22) * 2; w = (m02 - m20) / s; x = (m01 + m10) / s; y = s / 4; z = (m12 + m21) / s
    else:
        s = math.sqrt(1 + m22 - m00 - m11) * 2; w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = s / 4
    return [m[12], m[13], m[14], x, y, z, w, sx, sy, sz]


def from_trs(t):
    px, py, pz, x, y, z, w, sx, sy, sz = t
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1
    x, y, z, w = x / n, y / n, z / n, w / n
    r = [1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y),  # column 0
         2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x),  # column 1
         2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)]  # column 2
    return [r[0] * sx, r[1] * sx, r[2] * sx, 0, r[3] * sy, r[4] * sy, r[5] * sy, 0,
            r[6] * sz, r[7] * sz, r[8] * sz, 0, px, py, pz, 1]
