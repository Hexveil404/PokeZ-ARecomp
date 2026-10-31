"""Software-raster a draw (positions a0 * cb10 * cb5, near-clipped at w = 0.01) onto the 48x27 capture grid and
compare with the cells the GPU changed for it (cells2 logic). Cells the GPU painted that the draw's own data can't
reach mean the GPU drew something else than what the capture holds.
usage: python -I raster.py <dir of fcread.py> fullcap_N.bin <draw>"""
import sys, struct
sys.path.insert(0, sys.argv[1])
import fcread
fr, dr = fcread.load(sys.argv[2])
idx = [k for k, x in enumerate(dr) if x['draw'] == int(sys.argv[3])][0]
d = dr[idx]
B = {(k, s): data for k, s, a, sz, data in d['blobs']}
lay = struct.unpack('<50I', B[(14, 0)])
off = lay[1] >> 13; stride = lay[18]
vd = B[(5, 0)]
idata = B[(8, 1)]
ids = struct.unpack('<%dH' % (len(idata) // 2), idata)
m = struct.unpack_from('<16f', B[(1, 10)], 0)
v = struct.unpack_from('<16f', B[(1, 5)], 0)
W, H = 48, 27


def xf(p, M): return tuple(p[0] * M[c] + p[1] * M[4 + c] + p[2] * M[8 + c] + p[3] * M[12 + c] for c in range(4))


def clip(i):
    p = struct.unpack_from('<3f', vd, i * stride + off)
    return xf(xf((p[0], p[1], p[2], 1.0), m), v)


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
for t in range(0, len(ids) - 2, 3):
    poly = near_clip([clip(i) for i in ids[t:t + 3]])
    if len(poly) < 3: continue
    pts = [((p[0] / p[3] * 0.5 + 0.5) * W, (p[1] / p[3] * 0.5 + 0.5) * H) for p in poly]
    for k in range(1, len(pts) - 1):
        a, b, c = pts[0], pts[k], pts[k + 1]
        area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
        if area == 0: continue
        x0, x1 = max(0, int(min(a[0], b[0], c[0]))), min(W - 1, int(max(a[0], b[0], c[0])))
        y0, y1 = max(0, int(min(a[1], b[1], c[1]))), min(H - 1, int(max(a[1], b[1], c[1])))
        for gy in range(y0, y1 + 1):
            for gx in range(x0, x1 + 1):
                px, py = gx + 0.5, gy + 0.5
                w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])
                w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0])
                w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0])
                if (w0 >= 0 and w1 >= 0 and w2 >= 0) or (w0 <= 0 and w1 <= 0 and w2 <= 0):
                    cov.add((gx, gy)); cov.add((gx, H - 1 - gy))  # (either y convention)
nxt = dr[idx + 1]
gpu = set()
for gy in range(H):
    for gx in range(W):
        c = (gy * W + gx) * 16
        if d['cg'][c:c + 16] != nxt['cg'][c:c + 16] or d['dg'][c:c + 16] != nxt['dg'][c:c + 16]: gpu.add((gx, gy))
extra = gpu - cov
print('draw', d['draw'], 'GPU changed', len(gpu), 'cells; the data reaches', len(cov), 'cells; GPU-only cells', len(extra))
for gy in range(H):
    print(''.join('#' if (gx, gy) in extra else ('o' if (gx, gy) in gpu else ('.' if (gx, gy) in cov else ' ')) for gx in range(W)))
