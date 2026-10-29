"""Inspect/extract the player's battle layout for an opt-in, in-memory RomFS overlay.

Reads the original only. This does not run the game, repack ONEPACK, or change inputs.
The output is private game-derived mod data: do not redistribute it.
"""
import argparse
import ast
import ctypes
import hashlib
import json
import mmap
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
PACK = 'arc/uidatabtl_appbtl_app_waza_00.arc.trpak'
VIRTUAL = 'ui/data/btl_app/btl_app_waza_00.arc'
MEMBER = 'blyt/btl_app_waza_00.bflyt'

def fnv(data):
    h = 0xcbf29ce484222645
    for b in data: h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h

def readers():
    tree = ast.parse((ROOT / 'tools/za/prompt_textures.py').read_text(encoding='utf-8'))
    wanted = {'FB', 'Entry', 'open_game', 'game_files', 'sarc_files'}
    scope = dict(globals())
    exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, (ast.FunctionDef, ast.ClassDef))
                                 and n.name in wanted], type_ignores=[]), 'asset-readers', 'exec'), scope)
    return scope

def members(data):
    assert data[:4] == b'SARC' and struct.unpack_from('<H', data, 6)[0] == 0xfeff
    assert struct.unpack_from('<I', data, 8)[0] == len(data)
    fat = struct.unpack_from('<H', data, 4)[0]
    assert data[fat:fat+4] == b'SFAT'
    n = struct.unpack_from('<H', data, fat+6)[0]
    nt = fat+12+16*n
    assert data[nt:nt+4] == b'SFNT'
    start = struct.unpack_from('<I', data, 12)[0]
    for i in range(n):
        _, name, a, b = struct.unpack_from('<IIII', data, fat+12+16*i)
        assert name >> 24 == 1 and 0 <= a <= b <= len(data)-start
        p = nt+8+(name & 0xffffff)*4
        z = data.index(b'\0', p, start)
        yield data[p:z].decode(), start+a, data[start+a:start+b]

def panes(data):
    assert data[:4] == b'FLYT' and struct.unpack_from('<I',data,12)[0] == len(data)
    p = struct.unpack_from('<H',data,6)[0]
    while p < len(data):
        tag, size = struct.unpack_from('<4sI',data,p)
        assert size >= 8 and p+size <= len(data)
        if tag in (b'pan1',b'prt1',b'pic1',b'txt1',b'scr1'):
            assert size >= 84
            yield data[p+12:p+36].split(b'\0')[0].decode(), p, tag
        p += size
    assert p == len(data)

def inspect(data):
    byname = {name: (at, content) for name, at, content in members(data)}
    at, layout = byname[MEMBER]
    found = {name:(p,tag) for name,p,tag in panes(layout)}
    offsets=[]
    for i in range(4):
        name=f'L_waza_{i:02d}'
        p,tag=found[name]
        assert tag==b'prt1'
        offsets.append(at+p+12+8)  # the final digit only, all names have equal length
    for name, (_, content) in byname.items():
        if name.endswith('.bflan'):
            assert b'L_waza_' not in content, ('animation names a parent being reassigned',name)
    return {'size':len(data),'sha256':hashlib.sha256(data).hexdigest(),
            'fnv64':f'{fnv(data):016x}','digits':offsets,
            'layout_sha256':hashlib.sha256(layout).hexdigest(),
            'xy':[struct.unpack_from('<2f',layout,found[f'L_waza_{i:02d}'][0]+44) for i in range(4)],
            'member':MEMBER,'virtual_path':VIRTUAL}

def extract(installation):
    rd = readers()
    m, fd = rd['open_game'](str(installation/'extracted/romfs.bin'))
    fb = rd['FB'](fd)
    rt = fb.table(fb.u32(0))
    n,hp=fb.vec(rt[0])
    hashes=[fb.u64(hp+i*8) for i in range(n)]
    target=fnv(VIRTUAL.encode())
    assert target in hashes, 'virtual resource path not present in the game file catalog'
    index=hashes.index(target)
    ne, ep=fb.vec(rt[2]);assert ne==n
    record=fb.table(ep+index*4+fb.u32(ep+index*4))
    pack_index=fb.u32(record[0])
    np,pp=fb.vec(rt[1]);assert pack_index<np
    ps=pp+pack_index*4;ps+=fb.u32(ps)
    assert fd[ps+4:ps+4+fb.u32(ps)].decode()==PACK, 'catalog points to another pack'
    candidates=[e for e in rd['game_files'](m) if e.pack_hash==fnv(PACK.encode()) and e.index==0]
    assert len(candidates)==1
    entry=candidates[0]
    raw=bytes(entry.raw)
    pf=rd['FB'](m)
    pt=pf.table(entry.pack_off+pf.u32(entry.pack_off))
    _,fp=pf.vec(pt[1]);ep=fp+entry.index*4
    ft=pf.table(ep+pf.u32(ep));n,raw_offset=pf.vec(ft[4])
    assert n==len(raw) and bytes(m[raw_offset:raw_offset+n])==raw
    guard={'offset':raw_offset,'size':n,'fnv64':f'{fnv(raw):016x}'}
    if entry.comp==255 or len(raw)==entry.size: data=raw
    else:
        dll=ctypes.CDLL(str(installation/'zaprompts/_internal/ooz.dll'))
        dll.ooz_decompress.argtypes=[ctypes.c_char_p,ctypes.c_size_t,ctypes.c_char_p,ctypes.c_size_t]
        out=ctypes.create_string_buffer(entry.size+64)
        assert dll.ooz_decompress(raw,len(raw),out,entry.size)==entry.size
        data=out.raw[:entry.size]
    return data,guard

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('installation',type=Path)
    ap.add_argument('--prepare',action='store_true',help='write only inside the source checkout, never inside the installation')
    args=ap.parse_args()
    data,guard=extract(args.installation)
    info=inspect(data)
    info['source_pack_guard']=guard
    print(json.dumps(info,indent=2))
    if args.prepare:
        dest=ROOT/'assets/xbox_attack_layout/romfs'/VIRTUAL
        assert not dest.exists() or dest.read_bytes()==data, 'refusing to overwrite a different prepared asset'
        dest.parent.mkdir(parents=True,exist_ok=True)
        dest.write_bytes(data)
        (ROOT/'assets/xbox_attack_layout/resource.json').write_text(json.dumps(info,indent=2)+'\n',encoding='utf-8')
        constants = ('// Generated from the player\'s verified battle resource by tools/za/xbox_attack_layout.py.\n'
                     '// Hashes/offsets only; no game data. Do not edit by hand.\n#pragma once\n'
                     f'#define XBOX_ATTACK_ARC_SIZE {info["size"]}u\n'
                     f'#define XBOX_ATTACK_ARC_FNV UINT64_C(0x{info["fnv64"]})\n'
                     f'#define XBOX_ATTACK_PACK_OFFSET UINT64_C({guard["offset"]})\n'
                     f'#define XBOX_ATTACK_PACK_SIZE {guard["size"]}u\n'
                     f'#define XBOX_ATTACK_PACK_FNV UINT64_C(0x{guard["fnv64"]})\n'
                     'static const size_t xbox_attack_digits[4] = {'+', '.join(map(str,info['digits']))+'};\n')
        (ROOT/'runtime/xbox_attack_resource.h').write_text(constants,encoding='utf-8',newline='\n')

if __name__=='__main__': main()
