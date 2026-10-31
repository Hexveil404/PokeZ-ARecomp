import sys, struct, math
sys.path.insert(0, sys.argv[1])
import fcread
fr, dr = fcread.load(sys.argv[2])
d = [x for x in dr if x['draw'] == int(sys.argv[3])][0]
B = {(k, s): (a, sz, data) for k, s, a, sz, data in d['blobs']}
vd = B[(5, 0)][2]; idata = B[(8, 1)][2]
ids = struct.unpack('<%dH' % (len(idata) // 2), idata)
res = []
for stride in range(12, 68, 4):
    for off in range(0, stride - 8, 4):
        if max(ids) * stride + off + 12 > len(vd): continue
        big = tri = 0
        for t in range(0, min(len(ids), 3000) - 2, 3):
            P = [struct.unpack_from('<3f', vd, v * stride + off) for v in ids[t:t + 3]]
            if not all(math.isfinite(c) for p in P for c in p): big += 1; tri += 1; continue
            e = max(math.dist(P[0], P[1]), math.dist(P[1], P[2]), math.dist(P[0], P[2]))
            tri += 1; big += e > 20
        res.append((big / tri, stride, off))
for r in sorted(res)[:6]: print('stride %d off %d: %.0f%% triangles with edges > 20' % (r[1], r[2], r[0] * 100))
