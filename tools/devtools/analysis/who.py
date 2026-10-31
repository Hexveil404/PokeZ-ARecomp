import sys, struct
sys.path.insert(0, sys.argv[1])
import fcread
fr, dr = fcread.load(sys.argv[2])
lo, hi = int(sys.argv[3], 16), int(sys.argv[4], 16)
for d in dr:
    B = {(k, s): (a, sz) for k, s, a, sz, data in d['blobs']}
    lay = [data for k, s, a, sz, data in d['blobs'] if k == 14]
    for (k, s), (a, sz) in B.items():
        if k in (3, 4, 5, 8) and a < hi and a + sz > lo:
            strides = ''
            if lay:
                L = struct.unpack('<50I', lay[0]); strides = [(L[18 + 2 * j]) for j in range(L[17])]
                strides = 'attrs ' + str([(hex(x & 0xff), (x >> 8) & 31, x >> 13) for x in L[1:1 + L[0]]]) + ' strides ' + str(strides)
            print('draw %4d pass %2d vs %x kind %d slot %d %x+%d count %d %s' % (d['draw'], d['pass'], d['vs'], k, s, a, sz, d['count'], strides))
