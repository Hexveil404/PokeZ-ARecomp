import sys, struct
sys.path.insert(0, sys.argv[1])
import fcread
fr, dr = fcread.load(sys.argv[2])
for i in map(int, sys.argv[3:]):
    d = dr[i]
    for k, s, a, sz, data in d['blobs']:
        if k == 3 and s == 0:
            w = struct.unpack('<%dI' % (len(data)//4), data)
            print('draw', d['draw'], 'sb0 %x' % a, 'n', len(w), 'max', max(w))
            print(' ', w[:200])
