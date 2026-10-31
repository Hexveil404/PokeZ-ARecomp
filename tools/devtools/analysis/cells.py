"""Which draws changed grid cells, exactly (ZAFC3: each record holds the targets just before and just after its draw).
usage: python -I cells.py <dir of fcread.py> fullcap_N.bin <pass> gx,gy [gx,gy ...]"""
import sys
sys.path.insert(0, sys.argv[1])
import fcread
fr, dr = fcread.load(sys.argv[2])
P = int(sys.argv[3])
cells = [tuple(map(int, c.split(','))) for c in sys.argv[4:]]
idx = [i for i, d in enumerate(dr) if d['pass'] == P]
for gx, gy in cells:
    c = (gy * 48 + gx) * 16
    out = []
    for i in idx:
        d = dr[i]
        cc = d['cb'][c:c + 16] != d['cg'][c:c + 16]
        dd = d['db'][c:c + 16] != d['dg'][c:c + 16]
        if cc or dd:
            out.append('%d(vs %x n%d%s %s%s)' % (d['draw'], d['vs'], d['count'], ' GPU' if d['gpu'] else '', 'C' if cc else '', 'D' if dd else ''))
    print((gx, gy), ' '.join(out))
