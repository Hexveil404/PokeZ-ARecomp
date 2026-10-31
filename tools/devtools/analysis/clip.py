"""Project a draw's positions (a0 * cb10 model * cb5 viewproj, as the 77071daf00-style static mesh shaders do) and
count the triangles that cross the camera plane (w <= 0 at some vertex, w > 0 at another): without near clipping
those spread across the screen.
usage: python -I clip.py <dir of fcread.py> fullcap_N.bin <draw>"""
import sys, struct
sys.path.insert(0, sys.argv[1])
import fcread
fr, dr = fcread.load(sys.argv[2])
d = [x for x in dr if x['draw'] == int(sys.argv[3])][0]
B = {(k, s): data for k, s, a, sz, data in d['blobs']}
lay = struct.unpack('<50I', B[(14, 0)])
a0 = lay[1]; off = a0 >> 13; stride = lay[18]
vd = B[(5, 0)]
idata = B[(8, 1)]
ids = struct.unpack('<%dH' % (len(idata) // 2), idata)
m = struct.unpack_from('<16f', B[(1, 10)], 0)  # rows: world = x*r0 + y*r1 + z*r2 + r3
v = struct.unpack_from('<16f', B[(1, 5)], 0)


def xf(p, M):
    x, y, z, w = p
    return tuple(x * M[c] + y * M[4 + c] + z * M[8 + c] + w * M[12 + c] for c in range(4))


def clip(i):
    p = struct.unpack_from('<3f', vd, i * stride + off)
    return xf(xf((p[0], p[1], p[2], 1.0), m), v)


cross = behind = 0
near = []
for t in range(0, len(ids) - 2, 3):
    c = [clip(i) for i in ids[t:t + 3]]
    ws = [q[3] for q in c]
    if all(w <= 0 for w in ws): behind += 1
    elif any(w <= 0 for w in ws): cross += 1; near.append((t // 3, ['%.2f' % w for w in ws]))
print('draw', d['draw'], 'triangles', len(ids) // 3, 'all behind the camera', behind, 'CROSSING the camera plane', cross)
for n in near[:10]: print('  tri', n[0], 'w', n[1])
nd = []
for i in sorted(set(ids)):
    c = clip(i)
    if c[3] > 0: nd.append((c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3]))
if nd:
    print('NDC x %.2f..%.2f  y %.2f..%.2f  z %.3f..%.3f  w %.2f..%.2f' % (min(p[0] for p in nd), max(p[0] for p in nd), min(p[1] for p in nd), max(p[1] for p in nd), min(p[2] for p in nd), max(p[2] for p in nd), min(p[3] for p in nd), max(p[3] for p in nd)))
