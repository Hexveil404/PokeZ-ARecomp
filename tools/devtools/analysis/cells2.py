import sys, struct
sys.path.insert(0, sys.argv[1])
import fcread
fr, dr = fcread.load(sys.argv[2])
P = int(sys.argv[3])
cells = [tuple(map(int, c.split(','))) for c in sys.argv[4:]]
idx = [i for i, d in enumerate(dr) if d['pass'] == P]
nxt = idx[-1] + 1  # the grid before the next pass's first draw is not this pass's; use last draw only as "before"
for gx, gy in cells:
    c = (gy * 48 + gx) * 16
    out = []
    for a, b in zip(idx, idx[1:]):  # draw a changed the cell if the grid before b differs from the grid before a
        A, Bd = dr[a], dr[b]
        cc = A['cg'][c:c+16] != Bd['cg'][c:c+16]; dd = A['dg'][c:c+16] != Bd['dg'][c:c+16]
        if cc or dd:
            out.append('%d(vs %x n%d%s %s%s)' % (A['draw'], A['vs'], A['count'], ' GPU' if A['gpu'] else '', 'C' if cc else '', 'D' if dd else ''))
    print((gx, gy), ' '.join(out))
