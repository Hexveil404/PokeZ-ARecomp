import sys, struct
sys.path.insert(0, sys.argv[1])
import fcread
fr, dr = fcread.load(sys.argv[2])
import pickle
for i in map(int, sys.argv[3:]):
    d = dr[i]
    print('== draw', d['draw'], 'pass', d['pass'], 'vs %x fs %x' % (d['vs'], d['fs']), 'count', d['count'], 'indexed', d['indexed'], 'gpu', d['gpu'])
    for k, slot, addr, size, data in d['blobs']:
        print('  kind %2d slot %2d addr %x size %d len %d' % (k, slot, addr, size, len(data)))
        if k == 1 and slot in (5, 10):
            f = struct.unpack_from('<16f', data, 0)
            for r in range(4): print('     ', ' '.join('%10.3f' % x for x in f[r*4:r*4+4]))
