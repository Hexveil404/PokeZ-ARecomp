import struct, sys
G = 48 * 27 * 16


def load(path):
    f = open(path, 'rb')
    assert f.read(5) == b'ZAFC3'
    f.read(3)
    frame, = struct.unpack('<Q', f.read(8))
    draws = []
    seen = {}
    global dispatches
    dispatches = []  # (ZAFC3 + dispatches) 'DISB' / 'DISA' records: dict(after, code, groups, seq, blobs)
    seq = 0
    while True:
        h = f.read(8)
        if len(h) < 8: break
        magic, ln = struct.unpack('<II', h)
        body = f.read(ln)
        seq += 1
        if magic in (0x42534944, 0x41534944):
            code, gx, gy, gz, nb = struct.unpack_from('<QIIII', body, 0)
            off = 24
            bl = []
            try:
                for i in range(nb):
                    kind, slot, addr, size, l, hh = struct.unpack_from('<IIQQIQ', body, off); off += 36
                    if l == 0xFFFFFFFF:
                        bl.append((kind, slot, addr, size, seen.get((kind, addr, hh), b''))); continue
                    data = body[off:off + l]; off += l
                    seen[(kind, addr, hh)] = data
                    bl.append((kind, slot, addr, size, data))
            except struct.error:
                pass
            dispatches.append(dict(after=magic == 0x41534944, code=code, groups=(gx, gy, gz), seq=seq, before_draw=len(draws), blobs=bl))
            continue
        d = {}
        d['draw'], d['pass'] = struct.unpack_from('<II', body, 0)
        d['vs'], d['fs'] = struct.unpack_from('<QQ', body, 8)
        d['indexed'], d['mode'], d['count'] = struct.unpack_from('<IIQ', body, 24)
        d['inst'], d['gpu'], d['cfmt'], d['dfmt'], gw, gh = struct.unpack_from('<6I', body, 40)
        off = 64
        d['cg'] = body[off:off + G]; off += G
        d['dg'] = body[off:off + G]; off += G
        d['cb'] = body[off:off + G]; off += G  # (just before the draw)
        d['db'] = body[off:off + G]; off += G
        nb, = struct.unpack_from('<I', body, off); off += 4
        d['blobs'] = []
        try:
          for i in range(nb):
            kind, slot, addr, size, l = struct.unpack_from('<IIQQI', body, off); off += 28
            h = 0
            if kind not in (6, 7):  # (texture tables carry no hash)
                h, = struct.unpack_from('<Q', body, off); off += 8
            if l == 0xFFFFFFFF:
                d['blobs'].append((kind, slot, addr, size, seen.get((kind, addr, h), b'')))
                continue
            data = body[off:off + l]; off += l
            seen[(kind, addr, h)] = data
            d['blobs'].append((kind, slot, addr, size, data))
        except struct.error:  # (the record still being written)
            break
        d['seq'] = seq
        draws.append(d)
    return frame, draws


def touched(draws, gx, gy):
    c = (gy * 48 + gx) * 16
    out = []
    for i, d in enumerate(draws):
        first = i == 0 or draws[i - 1]['pass'] != d['pass']
        pc = b'\0' * 16 if first else draws[i - 1]['cg'][c:c + 16]
        pd = b'\0' * 16 if first else draws[i - 1]['dg'][c:c + 16]
        m = (d['cg'][c:c + 16] != pc) | ((d['dg'][c:c + 16] != pd) << 1)
        if m: out.append((i, m))
    return out


def vtex(d, kind=6):
    out = []
    for b in d['blobs']:
        if b[0] != kind: continue
        data = b[4]
        for k in range(len(data) // 56):
            t = struct.unpack_from('<IIIIQQQIIII', data, k * 56)
            out.append(dict(index=t[0], slot=t[1], tic=t[2], fmt=t[3], handle=t[4], obj=t[5], addr=t[6], w=t[7], h=t[8], lv=t[9]))
    return out


if __name__ == '__main__':
    path, gx, gy = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    frame, draws = load(path)
    print('frame', frame, 'draws', len(draws))
    for i, m in touched(draws, gx, gy):
        d = draws[i]
        vt = vtex(d)
        print('draw %4d pass %3d %-12s vs %x fs %x count %d x%d%s | VS textures: %s' % (
            d['draw'], d['pass'], {1: 'colour', 2: 'depth', 3: 'colour+depth'}[m], d['vs'], d['fs'], d['count'], d['inst'],
            ' GPU' if d['gpu'] else '', ', '.join('%dx%d fmt %#x tic %d' % (t['w'], t['h'], t['fmt'], t['tic']) for t in vt) or '-'))
