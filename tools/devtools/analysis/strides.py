import sys, struct, math
sys.path.insert(0, sys.argv[1])
import fcread
fr, dr = fcread.load(sys.argv[2])
d = [x for x in dr if x['draw'] == int(sys.argv[3])][0]
B = {(k, s): (a, sz, data) for k, s, a, sz, data in d['blobs']}
vd = B[(5, 0)][2]; idata = B[(8, 1)][2]
ids = struct.unpack('<%dH' % (len(idata) // 2), idata)
print('indices min', min(ids), 'max', max(ids), 'stream bytes', len(vd))
for stride in map(int, sys.argv[4:]):
    ps = [struct.unpack_from('<3f', vd, v * stride) for v in sorted(set(ids)) if v * stride + 12 <= len(vd)]
    ok = [p for p in ps if all(math.isfinite(c) for c in p)]
    lo = [min(p[k] for p in ok) for k in range(3)]; hi = [max(p[k] for p in ok) for k in range(3)]
    tri = 0; big = 0
    for t in range(0, len(ids) - 2, 3):
        a, b, c = ids[t:t + 3]
        if max(a, b, c) * stride + 12 > len(vd): continue
        P = [struct.unpack_from('<3f', vd, v * stride) for v in (a, b, c)]
        e = max(math.dist(P[0], P[1]), math.dist(P[1], P[2]), math.dist(P[0], P[2]))
        tri += 1; big += e > 20
    print('stride %2d: verts %d (%d past end) bbox %s..%s  triangles %d, edges >20 units: %d' % (stride, len(ps), len(set(ids)) - len(ps), ['%.1f' % x for x in lo], ['%.1f' % x for x in hi], tri, big))
