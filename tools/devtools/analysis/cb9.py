import sys, struct, math
sys.path.insert(0, sys.argv[1])
import fcread
fr, dr = fcread.load(sys.argv[2])
d = dr[int(sys.argv[3])]
for k, slot, addr, size, data in d['blobs']:
    if k == 1 and slot in (8,):
        f = struct.unpack_from('<%df' % (len(data)//4), data)
        print('cb8 first 8:', ['%.3f' % x for x in f[:8]])
    if k == 1 and slot == 9:
        n = len(data)//64
        for m in range(n):
            f = struct.unpack_from('<16f', data, m*64)
            sc = [math.sqrt(f[r*4]**2+f[r*4+1]**2+f[r*4+2]**2) for r in range(3)]
            print('%3d' % m, ' '.join('%9.2f' % x for x in f), ' | rowlen', ' '.join('%.2f' % s for s in sc))
