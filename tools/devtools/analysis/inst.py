import sys, struct
sys.path.insert(0, sys.argv[1])
import fcread
fr, dr = fcread.load(sys.argv[2])
i = [k for k, d in enumerate(dr) if d['draw'] == int(sys.argv[3])][0]
d = dr[i]
B = {}
for k, s, a, sz, data in d['blobs']: B[(k, s)] = (a, sz, data)
print('draw', d['draw'], 'vs %x' % d['vs'], 'count', d['count'], 'gpu', d['gpu'])
print('kinds', sorted(B))
args = struct.unpack('<5I', B[(13, 0)][2]); print('GPU args count %d instances %d first %d basevtx %d baseinst %d' % (args[0], args[1], args[2], struct.unpack('<i', struct.pack('<I', args[3]))[0], args[4]))
lay = struct.unpack('<50I', B[(14, 0)][2])
na = lay[0]; attrs = lay[1:17]; ns = lay[17]; streams = [(lay[18 + 2 * j], lay[19 + 2 * j]) for j in range(16)]
print('attribs', [(hex(a & 0xff), (a >> 8) & 0x1f, a >> 13) for a in attrs[:na]], 'streams', streams[:ns])
cb9 = B.get((1, 9)); nm = len(cb9[2]) // 64 if cb9 else 0
print('cb9 matrices', nm, 'size', cb9[1] if cb9 else None)
sb15 = B.get((3, 15)); w = struct.unpack('<%dI' % (len(sb15[2]) // 4), sb15[2])
idx = [w[args[4] + k] if args[4] + k < len(w) else None for k in range(args[1])]
print('sb15 %x' % sb15[0], 'matrix index per instance:', idx[:64])
bad = [k for k, m in enumerate(idx) if m is None or m >= nm]
print('instances with index beyond cb9:', len(bad), bad[:20])
