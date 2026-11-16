# Pokémon Z-A content that needs Nintendo's servers, from the player's own files (F1 > Content):
#   --stones   the Ranked Battle Mega Stones (Greninjite, Delphoxite, Chesnaughtite, Baxcalibrite, Sceptilite,
#              Swampertite, Blazikenite) in Quasartico's Mega Shard exchange, from the day it opens, 360 shards each
#   --tradeevo trade evolutions without a trade: plain trades at level 40, trades with an item by holding the item
#              (the evolve icon appears, day or night) - after the "No More Trade Evolutions" mod's rules
# The tables (flatbuffers) are the game's own; what is added goes in front of the old data (item rows, the shop's list:
# the old objects stay where they are, a new root lists them) or at its end (the evolution lists, plain structs), so
# nothing has to be understood beyond the fields named here. make() writes loose files under out/romfs at their own
# paths (runtime/services/fs.c layers them). Nothing of the game is shipped: prompt_textures.py --content (zaprompts,
# runtime/ui/launcher.mm launcher_content_kick) runs this on the player's own files.
import os, struct

STONES = (2577, 2576, 2575, 2648, 753, 752, 664)
PRICE = 360
P_PERSONAL = 'avalon/data/personal_array.bin'
P_PLIB = 'world/ik_data/battle/plib_item_conversion/plib_item_conversion_array.bin'
P_ITEM = 'world/exl/item_data/item_data/item_data.bin'
P_LINEUP = 'world/exl/shop/shop_item_lineup/shop_item_lineup.bin'
TRADE, TRADE_ITEM, LEVEL, HELD_DAY, HELD_NIGHT = 5, 6, 4, 19, 20  # evolution methods


class FB:
    def __init__(s, m): s.m = m
    def u16(s, p): return struct.unpack_from('<H', s.m, p)[0]
    def u32(s, p): return struct.unpack_from('<I', s.m, p)[0]
    def ind(s, p): return p + s.u32(p)
    def vtable(s, t): return t - struct.unpack_from('<i', s.m, t)[0]
    def fields(s, t):
        """{field: (offset in the table, bytes to the next field)}"""
        v = s.vtable(t)
        offs = [s.u16(v + 4 + 2 * i) for i in range((s.u16(v) - 4) // 2)]
        have = sorted(o for o in offs if o) + [s.u16(v + 2)]
        return {i: (o, have[have.index(o) + 1] - o) for i, o in enumerate(offs) if o}
    def rows(s):
        """the root table's first field: a vector of tables -> their positions"""
        r = s.u32(0)
        v = s.ind(r + s.fields(r)[0][0])
        return [s.ind(v + 4 + 4 * i) for i in range(s.u32(v))]
    def string(s, p):
        q = s.ind(p)
        return bytes(s.m[q + 4:q + 4 + s.u32(q)]).decode('latin1')


class Front:
    """New objects in front of a flatbuffer's data: the file becomes [root][these][the old data from byte 4], so the
    old objects' offsets to each other hold and the new ones point forward at them."""
    def __init__(s, old):
        s.old, s.buf, s.fix = old, bytearray(), []
    def here(s): return len(s.buf)
    def align(s, n=4):
        while len(s.buf) % n: s.buf.append(0)
    def u32(s, v): s.buf += struct.pack('<I', v)
    def raw(s, b): s.buf += b
    def ref_old(s, x): s.fix.append((len(s.buf), 'o', x)); s.u32(0)     # offset to an old object (at x in the old file)
    def ref_new(s, label): s.fix.append((len(s.buf), 'n', label)); s.u32(0)  # offset to a new object, placed later
    def vt_old(s, x): s.fix.append((len(s.buf), 'v', x)); s.u32(0)      # a table's start: its vtable is the old one at x
    def vt_new(s, label): s.fix.append((len(s.buf), 'w', label)); s.u32(0)
    def done(s, labels, root):
        s.align(8)
        L = len(s.buf)
        for q, kind, x in s.fix:
            at = 4 + q
            if kind == 'o': v = x + L - at
            elif kind == 'n': v = 4 + labels[x] - at
            elif kind == 'v': v = (at - (x + L)) & 0xFFFFFFFF
            else: v = (at - (4 + labels[x])) & 0xFFFFFFFF
            if kind in 'on' and not 0 < v < 1 << 31: raise ValueError('offset back')
            struct.pack_into('<I', s.buf, q, v)
        return struct.pack('<I', 4 + labels[root]) + bytes(s.buf) + bytes(s.old[4:])


def new_root(fb, fr, labels, rows):
    """a root table and its vector of rows: rows is the old positions, or labels of new tables"""
    r = fb.u32(0)
    labels['root'] = fr.here()
    fr.vt_old(fb.vtable(r))
    body = bytearray(fb.m[r + 4:r + fb.u16(fb.vtable(r) + 2)])
    o = fb.fields(r)[0][0]
    fr.raw(body[:o - 4]); fr.ref_new('rows'); fr.raw(body[o:])
    fr.align()
    labels['rows'] = fr.here()
    fr.u32(len(rows))
    for x in rows:
        fr.ref_new(x) if isinstance(x, str) else fr.ref_old(x)


def patch_items(d, stones=STONES, price=PRICE):
    """item_data with a Mega Shard price (field 8) on the stones that have none -> (bytes, stones priced)"""
    fb = FB(d)
    rows = fb.rows()
    ids = {fb.u32(t + fb.fields(t)[0][0]): t for t in rows}
    # a stone that has a price: its vtable is the one a priced stone needs
    like = next(t for t in rows if 8 in fb.fields(t) and fb.string(t + fb.fields(t)[2][0]).endswith('NAITO'))
    lf = fb.fields(like)
    todo = [i for i in stones if i in ids and 8 not in fb.fields(ids[i])]
    for i in todo:
        if not set(fb.fields(ids[i])) <= set(lf): raise ValueError('item %d: fields' % i)
    if not todo: return bytes(d), 0
    fr, labels = Front(d), {}
    new_root(fb, fr, labels, ['item%d' % fb.u32(t + fb.fields(t)[0][0]) if fb.u32(t + fb.fields(t)[0][0]) in todo else t
                              for t in rows])
    size = fb.u16(fb.vtable(like) + 2)
    for i in todo:
        t, f = ids[i], fb.fields(ids[i])
        fr.align()
        labels['item%d' % i] = fr.here()
        start = fr.here()
        fr.vt_old(fb.vtable(like))
        fr.raw(bytes(size - 4))
        for k, (o, n) in lf.items():
            if k == 8: struct.pack_into('<I' if n >= 4 else '<H', fr.buf, start + o, price)
            elif k in (2, 3):  # the name and the label: strings, the old ones
                fr.fix.append((start + o, 'o', fb.ind(t + f[k][0])))
            elif k in f:
                m = min(n, f[k][1])
                fr.buf[start + o:start + o + m] = fb.m[t + f[k][0]:t + f[k][0] + m]
    return fr.done(labels, 'root'), len(todo)


def patch_lineup(d, stones=STONES, shop='megashop_01_lineup1'):
    """shop_item_lineup with the stones at the end of the Mega Shard exchange's list, on sale as its first stones are
    -> (bytes, stones added)"""
    fb = FB(d)
    rows = fb.rows()
    row = next(t for t in rows if fb.string(t + fb.fields(t)[0][0]) == shop)
    rf = fb.fields(row)
    v = fb.ind(row + rf[1][0])
    ents = [fb.ind(v + 4 + 4 * i) for i in range(fb.u32(v))]
    item = lambda e: fb.u32(e + fb.fields(e)[0][0])
    order = lambda e: fb.u32(e + fb.fields(e)[1][0])
    have = {item(e) for e in ents}
    todo = [i for i in stones if i not in have]
    if not todo: return bytes(d), 0
    # the conditions of a stone sold from the start: the cheapest stone's (the first in the shop's order)
    stone = [e for e in ents if 650 <= item(e) <= 770 or item(e) >= 2559]
    like = min(stone, key=order)
    lf = fb.fields(like)
    if sorted(lf) != [0, 1, 2] or any(n != 4 for _, n in lf.values()): raise ValueError('lineup entry')
    fr, labels = Front(d), {}
    new_root(fb, fr, labels, ['shop' if t == row else t for t in rows])
    fr.align()
    labels['shop'] = fr.here()
    start = fr.here()
    fr.vt_old(fb.vtable(row))
    fr.raw(bytes(fb.u16(fb.vtable(row) + 2) - 4))
    for k, (o, n) in rf.items():
        if k == 0: fr.fix.append((start + o, 'o', fb.ind(row + o)))
        elif k == 1: fr.fix.append((start + o, 'n', 'ents'))
        else: fr.buf[start + o:start + o + n] = fb.m[row + o:row + o + n]
    fr.align()
    labels['ents'] = fr.here()
    fr.u32(len(ents) + len(todo))
    for e in ents: fr.ref_old(e)
    for i in todo: fr.ref_new('ent%d' % i)
    last = max(order(e) for e in ents)
    for n, i in enumerate(todo):
        fr.align()
        labels['ent%d' % i] = fr.here()
        start = fr.here()
        fr.vt_old(fb.vtable(like))
        fr.raw(bytes(fb.u16(fb.vtable(like) + 2) - 4))
        struct.pack_into('<I', fr.buf, start + lf[0][0], i)
        struct.pack_into('<I', fr.buf, start + lf[1][0], last + 1 + n)
        fr.fix.append((start + lf[2][0], 'o', fb.ind(like + lf[2][0])))
    return fr.done(labels, 'root'), len(todo)


def plib_rows(fb):
    out = []
    for t in fb.rows():
        f = fb.fields(t)
        out.append((t, fb.u32(t + f[0][0]) if 0 in f else 0, fb.u32(t + f[1][0]) if 1 in f else 0))
    return out  # (table, item, held-item number)


def evolutions(fb):
    """(row, its field's place, [(level, method, argument, r, r, r, species, form)]) of every Pokémon with any"""
    for t in fb.rows():
        f = fb.fields(t)
        if 21 not in f: continue
        v = fb.ind(t + f[21][0])
        yield t, t + f[21][0], [struct.unpack_from('<8H', fb.m, v + 4 + 16 * i) for i in range(fb.u32(v))]


def patch_trade(personal, plib):
    """-> (personal bytes, plib bytes, evolutions added): a level 40 evolution next to each trade, held-item ones (day
    and night) next to each trade with an item; the items get held-item numbers where they have none"""
    fb, pb = FB(personal), FB(plib)
    prow = plib_rows(pb)
    num = {i: p for _, i, p in prow if i}
    free = {p: t for t, i, p in prow if not i}
    # the numbers the game's own code has for these items (its table leaves them without an item)
    known = {221: 11, 233: 13, 646: 104, 647: 105}
    items = sorted({e[2] for _, _, ev in evolutions(fb) for e in ev if e[1] == TRADE_ITEM})
    add = {}
    for i in items:
        if i in num: continue
        if i in known and known[i] in free: add[i] = known[i]
        else: add[i] = i  # as the game's own row for item 1691: the item's number
    num.update(add)
    out = bytearray(personal)
    added = 0
    for t, at, ev in evolutions(fb):
        new = []
        for lv, m, arg, r3, r4, r5, sp, form in ev:
            if m == TRADE and not any(e[1] == LEVEL and e[6:] == (sp, form) for e in ev):
                new.append((40, LEVEL, 0, 0, 0, 0, sp, form))
            if m == TRADE_ITEM:
                for hm in (HELD_DAY, HELD_NIGHT):
                    if not any(e[1] == hm and e[6:] == (sp, form) for e in ev):
                        new.append((0, hm, num[arg], 0, 0, 0, sp, form))
        if not new: continue
        while len(out) % 4: out.append(0)
        struct.pack_into('<I', out, at, len(out) - at)
        out += struct.pack('<I', len(ev) + len(new))
        for e in ev + new: out += struct.pack('<8H', *e)
        added += len(new)
    # plib: the rows with the items filled in / new rows, in front
    if add:
        like = next(t for t, i, p in prow if i and p and sorted(pb.fields(t)) == [0, 1])
        lf = pb.fields(like)
        fr, labels = Front(plib), {}
        fill = {free[p]: i for i, p in add.items() if p in free}
        extra = [(i, p) for i, p in add.items() if p not in free]
        new_root(pb, fr, labels, ['p%d' % pb.u32(t + pb.fields(t)[1][0]) if t in fill else t for t, _, _ in prow] +
                 ['p%d' % p for _, p in extra])
        for i, p in [(i, p) for i, p in add.items()]:
            fr.align()
            labels['p%d' % p] = fr.here()
            start = fr.here()
            fr.vt_old(pb.vtable(like))
            fr.raw(bytes(pb.u16(pb.vtable(like) + 2) - 4))
            struct.pack_into('<I', fr.buf, start + lf[0][0], i)
            struct.pack_into('<I', fr.buf, start + lf[1][0], p)
        plib = fr.done(labels, 'root')
    return bytes(out), bytes(plib), added


M64 = (1 << 64) - 1


def fnv(s):
    h = 0xCBF29CE484222645
    for c in s.encode(): h = ((h ^ c) * 0x100000001B3) & M64
    return h


def game_files(trpfs, trpfd, unpack, paths):
    """{path: bytes} of the game's files at these paths: data.trpfd says which pack each is in, data.trpfs has it"""
    fd = FB(trpfd)
    t = fd.fields(fd.u32(0))
    vec = lambda k: (fd.u32(fd.ind(fd.u32(0) + t[k][0])), fd.ind(fd.u32(0) + t[k][0]) + 4)
    (nh, hp), (nn, np_), (nm, mp) = vec(0), vec(1), vec(2)
    u64 = lambda m, p: struct.unpack_from('<Q', m, p)[0]
    want = {fnv(p): p for p in paths}
    packs = {}  # pack name's hash -> the paths' hashes in it
    for i in range(nh):
        h = u64(trpfd, hp + 8 * i)
        if h not in want: continue
        e = fd.ind(mp + 4 * i)
        f = fd.fields(e)
        pi = u64(trpfd, e + f[0][0]) if 0 in f else 0
        s = fd.ind(np_ + 4 * pi)
        packs.setdefault(fnv(bytes(trpfd[s + 4:s + 4 + fd.u32(s)]).decode()), set()).add(h)
    fs = FB(trpfs)
    root = u64(trpfs, 8)
    rt = fs.fields(fs.ind(root))
    r0 = fs.ind(root)
    hv, ov = fs.ind(r0 + rt[0][0]), fs.ind(r0 + rt[1][0])
    out = {}
    for i in range(fs.u32(hv)):
        hs = packs.get(u64(trpfs, hv + 4 + 8 * i))
        if not hs: continue
        off = u64(trpfs, ov + 4 + 8 * i)
        pk = fs.ind(off)
        pf = fs.fields(pk)
        fh, fv = fs.ind(pk + pf[0][0]), fs.ind(pk + pf[1][0])
        for j in range(fs.u32(fv)):
            h = u64(trpfs, fh + 4 + 8 * j)
            if h not in hs: continue
            e = fs.ind(fv + 4 + 4 * j)
            f = fs.fields(e)
            comp = trpfs[e + f[1][0]] if 1 in f else 0xFF
            size = u64(trpfs, e + f[3][0]) if 3 in f else 0
            dv = fs.ind(e + f[4][0])
            out[want[h]] = unpack(comp, size, trpfs[dv + 4:dv + 4 + fs.u32(dv)])
    return out


def make(read, out, stones=True, tradeevo=True, log=print):
    """read(path) -> the game's file; out/romfs/<path> for each table changed -> files written"""
    files = {}
    if stones:
        files[P_ITEM], n = patch_items(read(P_ITEM))
        files[P_LINEUP], m = patch_lineup(read(P_LINEUP))
        log(f'ranked Mega Stones: {n} priced at {PRICE} Mega Shards, {m} added to the exchange')
    if tradeevo:
        files[P_PERSONAL], files[P_PLIB], n = patch_trade(read(P_PERSONAL), read(P_PLIB))
        log(f'trade evolutions: {n} evolutions added')
    for path, data in files.items():
        dst = os.path.join(out, 'romfs', path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f: f.write(data)
    return len(files)
