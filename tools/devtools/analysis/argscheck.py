"""For the draws that changed the given cells (exact before/after grids), check their indices against their own vertex
buffer, and for GPU-parameter draws compare the parameters the draw used (kind 13) with what the culling dispatches
left in that slot (the last 'DISA' record covering the address before the draw), and with the slot before them.
usage: python -I argscheck.py <dir of fcread.py> fullcap_N.bin <pass> gx,gy [gx,gy ...]"""
import sys, struct
sys.path.insert(0, sys.argv[1])
import fcread
fr, dr = fcread.load(sys.argv[2])
ds = fcread.dispatches
P = int(sys.argv[3])
cells = [tuple(map(int, c.split(','))) for c in sys.argv[4:]]
hits = {}
for gx, gy in cells:
    c = (gy * 48 + gx) * 16
    for d in dr:
        if d['pass'] != P: continue
        if d['cb'][c:c + 16] != d['cg'][c:c + 16] or d['db'][c:c + 16] != d['dg'][c:c + 16]:
            hits.setdefault(d['draw'], set()).add((gx, gy))


def slot_in(recs, addr, kinds):
    for r in recs:
        for k, s, a, sz, data in r['blobs']:
            if k in kinds and a <= addr and addr + 20 <= a + len(data):
                return r, s, struct.unpack_from('<5I', data, addr - a)
    return None, None, None


for D, cs in sorted(hits.items()):
    d = [x for x in dr if x['draw'] == D][0]
    B = {(k, s): (a, sz, data) for k, s, a, sz, data in d['blobs']}
    lay = B.get((14, 0)); stride = struct.unpack('<50I', lay[2])[18] if lay else 0
    vb = B.get((5, 0)); nv = (vb[1] // stride) if vb and stride else -1
    ib = B.get((8, 101)) or B.get((8, 1))
    ids = struct.unpack('<%dH' % (len(ib[2]) // 2), ib[2]) if ib and ib[2] else ()
    real = [i for i in ids if i != 0xFFFF]
    mx = max(real) if real else -1
    own = next((n for n, i in enumerate(ids) if i != 0xFFFF and nv > 0 and i >= nv), len(ids))
    print('draw %d vs %x gpu %d cells %d | vertices %d, indices %d, max %d, first index past the buffer at position %s' % (
        D, d['vs'], d['gpu'], len(cs), nv, len(ids), mx, own if own < len(ids) else '-'))
    if (13, 0) in B:
        a = B[(13, 0)][0]
        used = struct.unpack('<5I', B[(13, 0)][2])
        before = [r for r in ds if r['seq'] < d['seq']]
        after_rec, sl, after_val = slot_in([r for r in reversed(before) if r['after']], a, (21,))
        bef_rec, sl2, bef_val = slot_in([r for r in before if not r['after']], a, (20,))
        print('   args @%x used by the draw  : %s' % (a, used))
        print('   after the culling dispatch : %s  (%s storage %s)' % (after_val, hex(after_rec['code']) if after_rec else '-', sl))
        print('   before the first dispatch  : %s' % (bef_val,))
