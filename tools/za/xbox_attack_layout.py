"""Private battle UI preparation used by the shipped zaprompts helper.

Only metadata is shipped in the source. Game data is read from the player's
RomFS and installed as a hidden, opt-in overlay; no source build is needed.
"""
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile

FORMAT_VERSION = 1
MAGIC = b'ZAXBOX01'
MAX_ARC = 16 << 20
MAX_PACK = 64 << 20
PACK = 'arc/uidatabtl_appbtl_app_waza_00.arc.trpak'
VIRTUAL = 'ui/data/btl_app/btl_app_waza_00.arc'
MEMBER = 'blyt/btl_app_waza_00.bflyt'
MOD_NAME = '_XboxAttackLayout'
STATUS = '.XboxAttackLayout.prepared'


def fnv(data):
    h = 0xcbf29ce484222645
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def require(condition, why):
    if not condition:
        raise ValueError(why)


def members(data):
    require(len(data) >= 20 and data[:4] == b'SARC', 'not a SARC archive')
    require(struct.unpack_from('<H', data, 6)[0] == 0xfeff, 'unsupported SARC byte order')
    require(struct.unpack_from('<I', data, 8)[0] == len(data), 'SARC size mismatch')
    fat = struct.unpack_from('<H', data, 4)[0]
    require(fat + 12 <= len(data) and data[fat:fat+4] == b'SFAT', 'missing SFAT')
    n = struct.unpack_from('<H', data, fat+6)[0]
    nt = fat + 12 + 16*n
    start = struct.unpack_from('<I', data, 12)[0]
    require(nt + 8 <= start <= len(data) and data[nt:nt+4] == b'SFNT', 'invalid SARC names')
    for i in range(n):
        _, name, a, b = struct.unpack_from('<IIII', data, fat+12+16*i)
        require(name >> 24 == 1 and a <= b <= len(data)-start, 'invalid SARC entry')
        p = nt + 8 + (name & 0xffffff)*4
        require(nt + 8 <= p < start, 'invalid SARC name offset')
        z = data.index(b'\0', p, start)
        yield data[p:z].decode(), start+a, data[start+a:start+b]


def panes(data):
    require(len(data) >= 20 and data[:4] == b'FLYT', 'not a FLYT layout')
    require(struct.unpack_from('<I', data, 12)[0] == len(data), 'FLYT size mismatch')
    p = struct.unpack_from('<H', data, 6)[0]
    require(20 <= p <= len(data), 'invalid FLYT header')
    while p < len(data):
        require(p + 8 <= len(data), 'truncated FLYT section')
        tag, size = struct.unpack_from('<4sI', data, p)
        require(size >= 8 and p+size <= len(data), 'invalid FLYT section size')
        if tag in (b'pan1', b'prt1', b'pic1', b'txt1', b'scr1'):
            require(size >= 84, 'truncated pane')
            yield data[p+12:p+36].split(b'\0')[0].decode(), p, tag
        p += size


def inspect(data):
    require(0 < len(data) <= MAX_ARC, 'unsupported battle archive size')
    entries = list(members(data))
    byname = {name: (at, payload) for name, at, payload in entries}
    require(len(byname) == len(entries) and MEMBER in byname, 'missing or duplicate battle layout')
    at, layout = byname[MEMBER]
    rows = list(panes(layout))
    found = {name: (p, tag) for name, p, tag in rows}
    require(len(found) == len(rows), 'duplicate pane names')
    offsets = []
    xy = []
    expected_xy = [(0.0, 46.0), (54.0, -9.0), (-54.0, -9.0), (0.0, -62.0)]
    templates = ['btl_app_waza_parts_01', 'btl_app_waza_parts_00',
                 'btl_app_waza_parts_02', 'btl_app_waza_parts_01']
    for i in range(4):
        name = f'L_waza_{i:02d}'
        require(name in found and layout.count(name.encode()) == 1, 'missing or repeated move pane')
        p, tag = found[name]
        require(tag == b'prt1', 'move pane is not a part')
        position = struct.unpack_from('<2f', layout, p+44)
        require(position == expected_xy[i], 'unsupported battle pane geometry')
        section_size = struct.unpack_from('<I', layout, p+4)[0]
        require(templates[i].encode()+b'\0' in layout[p+84:p+section_size],
                'unsupported direction-specific template')
        offsets.append(at+p+20)
        xy.append(position)
    for name, (_, payload) in byname.items():
        if name.endswith('.bflan'):
            require(b'L_waza_' not in payload, 'animation targets a reassigned parent: '+name)
    return {'size': len(data), 'fnv64': f'{fnv(data):016x}', 'digits': offsets,
            'xy': xy, 'member': MEMBER, 'virtual_path': VIRTUAL}


def extract(romfs, readers):
    # These are the helper's existing ONEPACK readers and its already-loaded ooz.
    m, fd = readers['open_game'](str(romfs))
    fb = readers['FB'](fd)
    rt = fb.table(fb.u32(0))
    n, hp = fb.vec(rt[0])
    hashes = [fb.u64(hp+i*8) for i in range(n)]
    target = fnv(VIRTUAL.encode())
    require(target in hashes, 'battle path is not in the game catalog')
    index = hashes.index(target)
    ne, ep = fb.vec(rt[2])
    require(ne == n, 'catalog count mismatch')
    record = fb.table(ep+index*4+fb.u32(ep+index*4))
    pack_index = fb.u32(record[0])
    np, pp = fb.vec(rt[1])
    require(pack_index < np, 'catalog pack index out of range')
    ps = pp + pack_index*4
    ps += fb.u32(ps)
    require(fd[ps+4:ps+4+fb.u32(ps)].decode() == PACK, 'catalog points to another pack')
    candidates = [e for e in readers['game_files'](m)
                  if e.pack_hash == fnv(PACK.encode()) and e.index == 0]
    require(len(candidates) == 1, 'missing or ambiguous battle archive')
    entry = candidates[0]
    require(0 < entry.size <= MAX_ARC and 0 < len(entry.raw) <= MAX_PACK,
            'unsupported packed battle size')
    pf = readers['FB'](m)
    pt = pf.table(entry.pack_off+pf.u32(entry.pack_off))
    _, fp = pf.vec(pt[1])
    ep = fp + entry.index*4
    ft = pf.table(ep+pf.u32(ep))
    n, raw_offset = pf.vec(ft[4])
    raw = bytes(entry.raw)
    require(n == len(raw) and bytes(m[raw_offset:raw_offset+n]) == raw, 'packed source mismatch')
    guard = {'offset': raw_offset, 'size': n, 'fnv64': f'{fnv(raw):016x}'}
    data = readers['unpack'](entry.comp, entry.size, raw)
    require(len(data) == entry.size, 'decompressed size mismatch')
    return data, guard


def metadata(data, info, guard):
    require(0 < guard['size'] <= MAX_PACK and guard['offset'] <= (1 << 64)-1-guard['size'],
            'invalid source guard')
    return struct.pack('<8s9Q', MAGIC, len(data), fnv(data), guard['offset'],
                       guard['size'], int(guard['fnv64'], 16), *info['digits'])


def atomic_file(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix='.'+path.name+'-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(data)
            f.flush()
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def install(data, guard, mods, stamp):
    """Validate first, stage privately, switch as a unit, roll back a failed move.

    The previous overlay is never overwritten in place. Interrupted preparation
    leaves either a matching manifest/archive pair or a closed runtime fallback.
    """
    require(0 < len(stamp) <= 47 and stamp.isascii() and
            all(c.isalnum() or c in '._-' for c in stamp), 'invalid preparation stamp')
    info = inspect(data)
    manifest = metadata(data, info, guard)
    mods = Path(mods)
    require(not mods.is_symlink(), 'mods directory is a link')
    target = mods / MOD_NAME
    require(not target.is_symlink(), 'Xbox overlay directory is a link')
    mods.mkdir(parents=True, exist_ok=True)
    info['source_pack_guard'] = guard
    info['format_version'] = FORMAT_VERSION
    asset = target / 'romfs' / VIRTUAL
    if (target/'resource.bin').is_file() and asset.is_file():
        require(not asset.is_symlink() and not (target/'resource.bin').is_symlink(), 'Xbox overlay file is a link')
        if (target/'resource.bin').read_bytes() == manifest and asset.read_bytes() == data:
            atomic_file(mods/STATUS, (stamp+' 1\n').encode())
            return 'already prepared'
    stage = Path(tempfile.mkdtemp(prefix='.XboxAttackLayout-', dir=mods))
    old = stage.with_name(stage.name+'-old')
    committed = False
    try:
        dest = stage/'romfs'/VIRTUAL
        dest.parent.mkdir(parents=True)
        dest.write_bytes(data)
        (stage/'resource.bin').write_bytes(manifest)
        (stage/'resource.json').write_text(json.dumps(info, indent=2)+'\n', encoding='utf-8')
        if target.exists():
            os.replace(target, old)
        try:
            os.replace(stage, target)
            committed = True
        except BaseException:
            if old.exists():
                os.replace(old, target)
            raise
        atomic_file(mods/STATUS, (stamp+' 1\n').encode())
    finally:
        shutil.rmtree(stage, ignore_errors=True)
        if committed:
            shutil.rmtree(old, ignore_errors=True)
    return 'prepared'


def prepare(romfs, mods, stamp, readers):
    # A failure is remembered for this source/helper stamp, never for an update.
    # Runtime independently checks every source byte guard and the local archive.
    try:
        data, guard = extract(romfs, readers)
        result = install(data, guard, mods, stamp)
        print('[xbox-layout] '+result, flush=True)
        return True
    except (OSError, ValueError, IndexError, KeyError, struct.error, UnicodeError, SystemExit) as e:
        print('[xbox-layout] unavailable: '+str(e), flush=True)
        try:
            if not isinstance(e, OSError) and not Path(mods).is_symlink():
                atomic_file(Path(mods)/STATUS, (stamp+' 0\n').encode())
        except OSError:
            pass
        return False
