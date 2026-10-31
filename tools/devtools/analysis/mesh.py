import sys, struct, math
sys.path.insert(0, sys.argv[1])
import fcread
fr, dr = fcread.load(sys.argv[2])
d = [x for x in dr if x['draw'] == int(sys.argv[3])][0]
B = {(k, s): (a, sz, data) for k, s, a, sz, data in d['blobs']}
lay = struct.unpack('<50I', B[(14, 0)][2])
na = lay[0]; attrs = lay[1:1 + na]; streams = [(lay[18 + 2 * j], lay[19 + 2 * j]) for j in range(lay[17])]
print('attribs (fmt, stream, off):', [(hex(a & 0xff), (a >> 8) & 0x1f, a >> 13) for a in attrs], 'streams (stride, div):', streams)
ia, isz, idata = B[(8, 1)]
ids = struct.unpack('<%dH' % (len(idata) // 2), idata)
print('indices', len(ids), 'min', min(ids), 'max', max(ids), 'first 30', ids[:30])
a0 = attrs[0]; st_ = (a0 >> 8) & 0x1f; off = a0 >> 13; stride = streams[st_][0]
va, vsz, vd = B[(5, st_)]
print('stream addr %x size %d stride %d -> %d vertices' % (va, vsz, stride, vsz // stride))
def pos(v):
    o = v * stride + off
    return struct.unpack_from('<3f', vd, o) if o + 12 <= len(vd) else None
P = {v: pos(v) for v in set(ids)}
good = [p for p in P.values() if p]
lo = [min(p[k] for p in good) for k in range(3)]; hi = [max(p[k] for p in good) for k in range(3)]
print('bbox', lo, hi)
# outliers: far from median
import statistics
med = [statistics.median(p[k] for p in good) for k in range(3)]
far = sorted(((math.dist(p, med), v, p) for v, p in P.items() if p), reverse=True)[:12]
for dist, v, p in far: print('  v %5d dist %10.2f pos %s' % (v, dist, p))
print('--- per vertex (index range):')
for v in range(min(ids), max(ids) + 1, 1):
    p = pos(v)
    tag = 'FAR' if p and abs(p[0]) > 300 else ''
    if v % 4 == 0 or tag: pass
    print('  %5d %s %s' % (v, '(%.2f, %.2f, %.2f)' % p if p else None, tag))
