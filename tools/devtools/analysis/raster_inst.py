"""Software raster of a GPU-instanced draw (vs 74490e3f00 style: per-instance matrix index from storage buffer 15 at
the base instance, matrix rows in cb9, then cb5 view-projection), compared with the cells the GPU really changed
(ZAFC3 before/after grids). usage: python -I raster_inst.py <dir of fcread.py> fullcap_N.bin <draw>"""
import sys, struct
sys.path.insert(0, sys.argv[1])
import fcread
fr, dr = fcread.load(sys.argv[2])
d = [x for x in dr if x['draw'] == int(sys.argv[3])][0]
B = {(k, s): (a, sz, data) for k, s, a, sz, data in d['blobs']}
lay = struct.unpack('<50I', B[(14, 0)][2])
off = lay[1] >> 13; stride = lay[18]
vd = B[(5, 0)][2]
args = struct.unpack('<5I', B[(13, 0)][2])
cnt, ninst, first, bv, binst = args
bv = struct.unpack('<i', struct.pack('<I', bv))[0]
isrc = B.get((8, 101)) or B.get((8, 102)) or B[(8, 1)]
ids = struct.unpack('<%dH' % (len(isrc[2]) // 2), isrc[2])[:cnt]
sb = B[(3, 15)][2]
lst = struct.unpack('<%dI' % (len(sb) // 4), sb)
cb9 = B[(1, 9)][2]
v = struct.unpack_from('<16f', B[(1, 5)][2], 0)
W, H = 48, 27
print('draw', d['draw'], 'args', args, 'vertex stride', stride, 'pos offset', off, 'stream bytes', len(vd))


def xf(p, M): return tuple(p[0] * M[c] + p[1] * M[4 + c] + p[2] * M[8 + c] + p[3] * M[12 + c] for c in range(4))


def near_clip(poly, e=0.01):
    out = []
    for a, b in zip(poly, poly[1:] + poly[:1]):
        ia, ib = a[3] > e, b[3] > e
        if ia: out.append(a)
        if ia != ib:
            t = (e - a[3]) / (b[3] - a[3])
            out.append(tuple(a[k] + (b[k] - a[k]) * t for k in range(4)))
    return out


cov = set()
for inst in range(max(ninst, 1)):
    mi = lst[binst + inst]
    if (mi + 1) * 64 > len(cb9): print('  instance', inst, 'matrix', mi, 'PAST cb9'); continue
    m = struct.unpack_from('<16f', cb9, mi * 64)
    for t in range(0, len(ids) - 2, 3):
        poly = []
        for i in ids[t:t + 3]:
            p = struct.unpack_from('<3f', vd, (i + bv) * stride + off)
            poly.append(xf(xf((p[0], p[1], p[2], 1.0), m), v))
        poly = near_clip(poly)
        if len(poly) < 3: continue
        pts = [((q[0] / q[3] * 0.5 + 0.5) * W, (q[1] / q[3] * 0.5 + 0.5) * H) for q in poly]
        for k in range(1, len(pts) - 1):
            a, b, c = pts[0], pts[k], pts[k + 1]
            x0, x1 = max(0, int(min(a[0], b[0], c[0]))), min(W - 1, int(max(a[0], b[0], c[0])))
            y0, y1 = max(0, int(min(a[1], b[1], c[1]))), min(H - 1, int(max(a[1], b[1], c[1])))
            for gy in range(y0, y1 + 1):
                for gx in range(x0, x1 + 1):
                    px, py = gx + 0.5, gy + 0.5
                    w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])
                    w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0])
                    w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0])
                    if (w0 >= 0 and w1 >= 0 and w2 >= 0) or (w0 <= 0 and w1 <= 0 and w2 <= 0):
                        cov.add((gx, gy)); cov.add((gx, H - 1 - gy))
gpu = set()
for gy in range(H):
    for gx in range(W):
        c = (gy * W + gx) * 16
        if d['cb'][c:c + 16] != d['cg'][c:c + 16] or d['db'][c:c + 16] != d['dg'][c:c + 16]: gpu.add((gx, gy))
extra = gpu - cov
print('GPU changed', len(gpu), 'cells; the captured data reaches', len(cov), '; GPU-only', len(extra))
for gy in range(H):
    print(''.join('#' if (gx, gy) in extra else ('o' if (gx, gy) in gpu else ('.' if (gx, gy) in cov else ' ')) for gx in range(W)))
