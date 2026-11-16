"""F1 > Content's generator (tools/za/content_mod.py) on synthetic tables, no game data: the ranked Mega Stones get a
Mega Shard price and a place at the end of the exchange's list with a first-day stone's conditions, every other row
and shop as it was; a trade evolution gets a level 40 one next to it, a trade with an item gets held-item ones (day
and night) under the item's held-item number - the one the table keeps free for it, else the item's own number in a
new row; a second run changes nothing."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'za'))
import content_mod as cm  # noqa: E402


class W:
    """a tiny flatbuffer writer, front to back (tools/test_draw_dist.py's): [vtable][soffset][fields], strings,
    vectors and child tables after their parent"""
    def __init__(s):
        s.b, s.later = bytearray(4), []

    def align(s, n=4):
        while len(s.b) % n: s.b.append(0)

    def table(s, fields):
        s.align()
        n, vt = len(fields), len(s.b)
        s.b += struct.pack('<HH', 4 + 2 * n, 4 + 4 * n) + b'\0' * (2 * n)
        s.align()
        t = len(s.b)
        s.b += b'\0' * (4 + 4 * n)
        struct.pack_into('<i', s.b, t, t - vt)
        for i, f in enumerate(fields):
            if f is None: continue
            pos = t + 4 + 4 * i
            struct.pack_into('<H', s.b, vt + 4 + 2 * i, 4 + 4 * i)
            if f[0] == 'u32': struct.pack_into('<I', s.b, pos, f[1])
            else: s.later.append((pos, f[0], f[1]))
        return t

    def finish(s, rows):
        r = s.table([('tvec', rows)])
        struct.pack_into('<I', s.b, 0, r)
        while s.later:
            pos, kind, v = s.later.pop(0)
            s.align()
            at = len(s.b)
            if kind == 'str':
                s.b += struct.pack('<I', len(v)) + v.encode() + b'\0'
            elif kind == 'evo':  # a vector of 16-byte structs
                s.b += struct.pack('<I', len(v)) + b''.join(struct.pack('<8H', *e) for e in v)
            else:
                s.b += struct.pack('<I', len(v)) + b'\0' * (4 * len(v))
                for k, fields in enumerate(v):
                    slot = at + 4 + 4 * k
                    struct.pack_into('<I', s.b, slot, s.table(fields) - slot)
            struct.pack_into('<I', s.b, pos, at - pos)
        s.align(8)
        return bytes(s.b)


def item(i, name, price=None):
    return [('u32', i), ('u32', 7), ('str', name), ('str', 'item_%04d' % i), ('u32', 100000), ('u32', 7), ('u32', 1),
            ('u32', i % 90), ('u32', price) if price else None]


STONE_NAMES = {i: 'STONE%dNAITO' % i for i in cm.STONES}
items = W().finish([item(4, 'MONSUTAABOORU'), item(657, 'SAANAITONAITO', 240), item(666, 'HERUGANAITO', 180)] +
                   [item(i, STONE_NAMES[i]) for i in sorted(cm.STONES)])
out, n = cm.patch_items(items)
assert n == len(cm.STONES)
fb = cm.FB(out)
got = {}
for t in fb.rows():
    f = fb.fields(t)
    got[fb.u32(t + f[0][0])] = (fb.string(t + f[2][0]), fb.string(t + f[3][0]), fb.u32(t + f[8][0]) if 8 in f else None,
                                fb.u32(t + f[7][0]))
assert [fb.u32(t + fb.fields(t)[0][0]) for t in fb.rows()] == [4, 657, 666] + sorted(cm.STONES)  # (the order kept)
assert got[4] == ('MONSUTAABOORU', 'item_0004', None, 4) and got[657][2] == 240 and got[666][2] == 180
for i in cm.STONES: assert got[i] == (STONE_NAMES[i], 'item_%04d' % i, cm.PRICE, i % 90), got[i]
assert cm.patch_items(out) == (out, 0)

cond = lambda s: [('tvec', [[('str', s)]])]
ent = lambda i, order, c: [('u32', i), ('u32', order), ('tvec', [cond(c)])]
lineup = W().finish([
    [('str', 'a02_stoneshop_01_lineup1'), ('tvec', [ent(82, 1, 'force_condition')])],
    [('str', 'megashop_01_lineup1'), ('tvec', [ent(1125, 1, 'flag_condition'), ent(657, 26, 'phase_condition'),
                                               ent(666, 12, 'force_condition')])]])
out, n = cm.patch_lineup(lineup)
assert n == len(cm.STONES)
fb = cm.FB(out)
shops = {}
for t in fb.rows():
    f = fb.fields(t)
    v = fb.ind(t + f[1][0])
    ents = [fb.ind(v + 4 + 4 * i) for i in range(fb.u32(v))]
    shops[fb.string(t + f[0][0])] = [(fb.u32(e + 4), fb.u32(e + 8), fb.ind(e + 12)) for e in ents]
assert [e[:2] for e in shops['a02_stoneshop_01_lineup1']] == [(82, 1)]
mega = shops['megashop_01_lineup1']
assert [e[:2] for e in mega] == [(1125, 1), (657, 26), (666, 12)] + [(i, 27 + k) for k, i in enumerate(cm.STONES)]
assert all(e[2] == mega[2][2] for e in mega[3:])  # (the conditions of the first stone on sale: item 666's)
assert cm.patch_lineup(out) == (out, 0)

T, TI, LV, HD, HN = cm.TRADE, cm.TRADE_ITEM, cm.LEVEL, cm.HELD_DAY, cm.HELD_NIGHT
mon = lambda sp, form, evo: [('u32', sp | form << 16)] + [None] * 20 + [('evo', evo) if evo else None]
personal = W().finish([
    mon(63, 0, [(16, LV, 0, 0, 0, 0, 64, 0)]),
    mon(64, 0, [(0, T, 0, 0, 0, 0, 65, 0)]),
    mon(65, 0, None),
    mon(95, 0, [(0, TI, 233, 0, 0, 0, 208, 0)]),
    mon(137, 0, [(0, TI, 252, 0, 0, 0, 233, 0)]),
    mon(710, 2, [(0, T, 0, 0, 0, 0, 711, 2)])])
plib = W().finish([[('u32', 110), ('u32', 9)], [('u32', 0), ('u32', 13)], [('u32', 326), ('u32', 49)]])
p2, l2, n = cm.patch_trade(personal, plib)
assert n == 6
evo = {}
fb = cm.FB(p2)
for t, at, ev in cm.evolutions(fb):
    evo[struct.unpack_from('<HH', p2, t + fb.fields(t)[0][0])] = ev
assert evo[(63, 0)] == [(16, LV, 0, 0, 0, 0, 64, 0)]
assert evo[(64, 0)] == [(0, T, 0, 0, 0, 0, 65, 0), (40, LV, 0, 0, 0, 0, 65, 0)]
assert evo[(710, 2)] == [(0, T, 0, 0, 0, 0, 711, 2), (40, LV, 0, 0, 0, 0, 711, 2)]
assert evo[(95, 0)] == [(0, TI, 233, 0, 0, 0, 208, 0), (0, HD, 13, 0, 0, 0, 208, 0), (0, HN, 13, 0, 0, 0, 208, 0)]
assert evo[(137, 0)] == [(0, TI, 252, 0, 0, 0, 233, 0), (0, HD, 252, 0, 0, 0, 233, 0), (0, HN, 252, 0, 0, 0, 233, 0)]
assert len(cm.FB(p2).rows()) == 6 and (65, 0) not in evo
assert [(i, p) for _, i, p in cm.plib_rows(cm.FB(l2))] == [(110, 9), (233, 13), (326, 49), (252, 252)]
assert cm.patch_trade(p2, l2) == (p2, l2, 0)
print('content: ok')
