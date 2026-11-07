"""Check the game-address tables of every game version and the patch output, without any game files.

Optional:
  --game KEY=FOLDER (repeatable, e.g. --game v1_0_0=extracted --game v1_0_1=/path/to/1.0.1): extracted game files of
    that version. Every patch's instruction check and every hook word then runs on that version's main program, the
    randomizer's patch sites (tools/rando/patch_variants.json, and --seeds) must hold the same original bytes in every
    given version, the start screen's release label is checked, and with v1_0_0 and another version the committed
    tables must equal tools/version_map.py's output.
  --seeds FOLDER: randomizer seeds (rando/), whose exefs/1.0.0.pchtxt patch sites are checked like patch_variants.json.
  --compare-generated BEFORE AFTER: compares two complete generation directories made from the same player-provided
    exefs before/after a change.
"""
import argparse
import ast
import hashlib
import importlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace

import game_addresses
from game_addresses import (ADDRESS, TABLE_DIR, VERSIONS, UnknownGameVersion, addr_text, load_table, load_versions,
                            select_version, table_path, version_for_build_id)

ROOT = Path(__file__).resolve().parent.parent
MAIN_BASE = 0x7000000000
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--compare-generated', type=Path, nargs=2, metavar=('BEFORE', 'AFTER'))
parser.add_argument('--game', action='append', default=[], metavar='KEY=FOLDER')
parser.add_argument('--seeds', type=Path)
args = parser.parse_args()

# ---- every version: the same names, one header each, distinct values
assert list(VERSIONS)[0] == 'v1_0_0'
tables = {}
for key in VERSIONS:
    patch_values, runtime_values = load_table(table_path(key)), load_table(table_path(key, 'runtime'))
    assert not patch_values.keys() & runtime_values.keys(), f'{key}: define each address in one header'
    values = patch_values | runtime_values
    assert len(values) == len(set(values.values())), f'{key}: use the existing name for shared addresses'
    tables[key] = (patch_values, runtime_values)
    if key != 'v1_0_0':  # same names in the same order (the C struct is laid out from v1_0_0's)
        assert list(patch_values) == list(tables['v1_0_0'][0]), f'patches_{key}.h names differ from v1_0_0'
        assert list(runtime_values) == list(tables['v1_0_0'][1]), f'runtime_{key}.h names differ from v1_0_0'
assert len({bid for _, bid in VERSIONS.values()}) == len(VERSIONS)
assert version_for_build_id('ae16f71e002af8cb059a9a74c4d90f34ba984892') == 'v1_0_0'
assert version_for_build_id('909E904AF78AC1B8DEEFE97AB2CCDB51968F0EC7') == 'v1_0_1'
for bad in ('', 'ae16f71e002af8cb', 'ae16f71e002af8cb059a9a74c4d90f34ba98489200000001', '00' * 20):
    try:
        version_for_build_id(bad)
    except UnknownGameVersion:
        pass
    else:
        raise AssertionError(f'An unknown build ID was accepted: {bad}')

# ---- the 1.0.0 patch output, unchanged since the refactor's baseline
try:
    import patches  # noqa: F401 (nothing selected: the tables can't be built)
except AttributeError:
    pass
else:
    raise AssertionError('patches.py built its tables without a selected game version')
sys.modules.pop('patches', None)
select_version('v1_0_0')
import patches  # noqa: E402
patch_values, runtime_values = tables['v1_0_0']
fixture = json.loads((ROOT/'tools/tests/patches_v1_0_0.json').read_text())
assert set(patches.PATCHES) == {int(a, 16) for a in fixture['patches']}
for address, expected in fixture['patches'].items():
    address = int(address, 16)
    gen = SimpleNamespace(base=address, nso=SimpleNamespace(image=struct.pack('<I', int(expected['word'], 16))))
    code = patches.PATCHES[address](gen, address)
    assert hashlib.sha256(code.encode()).hexdigest() == expected['sha256'], f'Patch output changed at {address:x}'
assert hashlib.sha256(json.dumps(patches.HOOKS, sort_keys=True).encode()).hexdigest() == fixture['hooks_sha256']
assert addr_text('f_@HAS_ITEM@(c);') == f'f_{ADDRESS.HAS_ITEM:x}(c);'
try:
    addr_text('@UNKNOWN_ADDRESS@')
except AttributeError:
    pass
else:
    raise AssertionError('An unknown address name was accepted')
assert all(a < 0x7004000000 for a in list(patches.PATCHES) + list(patches.HOOKS)), 'patch sites are main addresses'
# every version: the same patches and hooks, at that version's addresses
for key in VERSIONS:
    select_version(key)
    p = importlib.reload(patches)
    assert len(p.PATCHES) == len(fixture['patches']) and len(p.HOOKS) == len(patches.HOOKS), key
    assert set(p.PATCHES) | set(p.HOOKS) <= set(tables[key][0].values()), key
select_version('v1_0_0')
patches = importlib.reload(patches)

# ---- the native side: the same values, selected by build ID; release labels
_tmp = tempfile.TemporaryDirectory()  # (kept to the end: the native helper checks the game files too)
tmp = Path(_tmp.name)
source = tmp/'addresses.c'
source.write_text(r'''#include "game_addresses.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
void rt_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }
static int hex(const char *s, uint8_t *id) {
for (int k = 0; k < 20; k++) { unsigned b; if (sscanf(s + 2 * k, "%2x", &b) != 1) return 0; id[k] = (uint8_t)b; }
return 1;
}
int main(int argc, char **argv) {
if (argc == 3 && !strcmp(argv[1], "id")) {  // id <40 hex>: the version's table, one address a line
    uint8_t id[20];
    const GameVersion *v = hex(argv[2], id) ? game_version_by_build_id(id) : NULL;
    if (!v) { puts("none"); return 0; }
    printf("%s\n", v->key);
    for (size_t i = 0; i < sizeof(GameAddrs) / 8; i++) printf("%llx\n", (unsigned long long)((const uint64_t *)v->addrs)[i]);
    return 0;
}
if (argc == 3 && !strcmp(argv[1], "label")) {  // label <exefs folder>
    char b[32];
    const char *l = game_release_label(argv[2], b, sizeof b);
    puts(l ? l : "none");
    return 0;
}
if (argc == 3 && !strcmp(argv[1], "select")) {  // select <game folder>
    char why[256] = "";
    int ok = game_version_select(argv[2], why, sizeof why);
    printf("%d %s %s\n", ok, ok ? game_version()->key : "-", why);
    if (ok && GA(HAS_ITEM) != g_ga->HAS_ITEM) return 1;
    return 0;
}
return 2;
}
''')
exe = tmp/('addresses.exe' if os.name == 'nt' else 'addresses')
cc = shlex.split(os.environ.get('CC', 'clang'))
cxx = shlex.split(os.environ.get('CXX', 'clang++'))
objs = []
for i, (compiler, src, std) in enumerate(((cc, source, '-std=gnu11'), (cc, ROOT/'runtime/game_addresses.c', '-std=gnu11'),
                                         (cxx, ROOT/'runtime/game_release.cpp', '-std=c++17'))):
    obj = tmp/f'{i}.o'
    subprocess.run(compiler+[std, '-Wall', '-Werror', '-Iruntime', '-c', str(src), '-o', str(obj)], cwd=ROOT, check=True)
    objs.append(str(obj))
subprocess.run(cxx+objs+['-o', str(exe)], cwd=ROOT, check=True)
native = lambda *a: subprocess.check_output([str(exe), *map(str, a)], text=True).split()  # noqa: E731
for key, (label, bid) in VERSIONS.items():
    out = native('id', bid)
    assert out[0] == key, (key, out[0])
    assert [int(x, 16) for x in out[1:]] == list((tables[key][0] | tables[key][1]).values()), key
assert native('id', '00' * 20) == ['none']
# game folders made up for the selection and the labels: an NSO header with the build ID, an npdm
def fake_game(name, bid, npdm=b'fixture npdm'):
    d = tmp/name/'exefs'
    d.mkdir(parents=True)
    (d/'main').write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(bid) + bytes(0x0c) + bytes(0x20))
    (d/'main.npdm').write_bytes(npdm)
    return d.parent
g0, g1 = fake_game('g0', VERSIONS['v1_0_0'][1]), fake_game('g1', VERSIONS['v1_0_1'][1])
gx = fake_game('gx', '11' * 20)
assert native('label', g0/'exefs') == ['1.0.0']
assert ' '.join(native('label', g1/'exefs')) == '1.0.1 / 1.1.0'  # (an npdm of neither release)
assert native('label', gx/'exefs') == ['none']
assert native('label', tmp/'missing') == ['none']
assert native('select', g1)[:2] == ['1', 'v1_0_1']
out = native('select', gx)
assert out[0] == '0' and 'unsupported version' in ' '.join(out), out
assert native('select', tmp/'missing')[0] == '0'

# Same resource location used by tools/lasgen.spec in the frozen helper.
bundled = tmp/'bundle/runtime/game_addresses'
bundled.mkdir(parents=True)
shutil.copyfile(TABLE_DIR/'versions.h', bundled/'versions.h')
for key in VERSIONS:
    shutil.copyfile(table_path(key), bundled/table_path(key).name)
check = ('import sys; sys._MEIPASS=sys.argv[1]; sys.path.insert(0,sys.argv[2]); '
         'import game_addresses as g; g.select_build_id(sys.argv[3]); print(g.ADDRESS.HAS_ITEM)')
for key, (label, bid) in VERSIONS.items():
    output = subprocess.check_output([sys.executable, '-c', check, str(tmp/'bundle'), str(ROOT/'tools'), bid])
    assert int(output) == tables[key][0]['HAS_ITEM']
bad = tmp/'bad.h'
for content in ('', 'GA_ADDR(BAD, 123)\n', 'GA_ADDR(DUP, 0x123)\n'*2):
    bad.write_text(content)
    try:
        load_table(bad)
    except ValueError:
        pass
    else:
        raise AssertionError('Malformed table accepted')
for content in ('', 'GA_VERSION(v1_0_0, "1.0.0", "AE16")\n'):
    bad.write_text(content)
    try:
        load_versions(bad)
    except ValueError:
        pass
    else:
        raise AssertionError('Malformed version list accepted')

# New hard-coded sites should not silently escape the shared table. Guest VM
# bases/range checks are runtime layout constants, not addresses of game objects.
values = patch_values | runtime_values
strip_c = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"', re.S)
for path in (ROOT/'runtime').rglob('*'):
    if path.suffix not in ('.c', '.h', '.m', '.mm', '.cpp') or path.parent.name == 'game_addresses':
        continue
    code = strip_c.sub('', path.read_text(encoding='utf-8'))
    assert not re.search(r'\bGA_(?!ADDR\b|VERSION\b|RELEASE\b|FIELD\b|AT\b)[A-Z]', code), f'{path}: use GA(NAME)'
    assert set(re.findall(r'\bGA(?:_FIELD)?\(([A-Z][A-Z0-9_]*)\)', code)) <= values.keys(), str(path)
    # (ZA Recomp: 0x7008000000 is Pokémon Z-A's sdk base; dispatch.c holds Z-A's own boot delegate sites, one per game
    # build - 1.0.0 / 2.0.0 / 2.0.2 - which belong to that game, not to Link's Awakening's shared table)
    for match in re.finditer(r'0x(700[0-9a-fA-F]{7})', code):
        assert path.name == 'dispatch.c' or int(match[1], 16) in (0x7000000000, 0x7004000000, 0x7008000000), str(path)
    assert not re.search(r'(?:MAIN_BASE|SDK_BASE)\s*\+\s*0x[0-9a-fA-F]+', code), str(path)
tree = ast.parse((ROOT/'tools/patches.py').read_text(encoding='utf-8'))
used = {n.attr for n in ast.walk(tree) if isinstance(n, ast.Attribute) and isinstance(n.value, ast.Name) and n.value.id == 'A'}
used.update(re.findall(r'@([A-Z][A-Z0-9_]*)@', (ROOT/'tools/patches.py').read_text(encoding='utf-8')))
assert used == patch_values.keys(), 'Keep runtime-only entries out of the generated-code table'
docstring = tree.body[0].value
for node in ast.walk(tree):
    if isinstance(node, ast.Constant) and node is not docstring:
        if isinstance(node.value, int):
            assert not 0x7000000000 < node.value < 0x7100000000
        elif isinstance(node.value, str):
            assert not re.search(r'(?:0x|f_|L_)700[0-9a-fA-F]{7}', node.value)

# ---- with game files
games = {}
for g in args.game:
    key, _, folder = g.partition('=')
    assert key in VERSIONS, f'--game {g}: unknown version {key}'
    games[key] = Path(folder)
if games:
    from nso import Nso
    images = {}
    for key, folder in games.items():
        n = Nso(str(folder/'exefs/main'))
        assert version_for_build_id(n.build_id) == key, f'{folder}: not {key}'
        images[key] = n
        select_version(key)
        p = importlib.reload(patches)
        gen = SimpleNamespace(nso=n, base=MAIN_BASE)
        for site, fn in p.PATCHES.items():
            fn(gen, site)  # (each one asserts the instruction it replaces)
        for site, (word, targets, what) in p.HOOKS.items():
            assert struct.unpack_from('<I', n.image, site - MAIN_BASE)[0] == word, f'{key}: hook {site:#x} ({what})'
        print(f'{key}: {len(p.PATCHES)} patch checks and {len(p.HOOKS)} hook words pass on {folder}')
    select_version('v1_0_0')
    patches = importlib.reload(patches)
    # the randomizer: the same original bytes at every patch site (rando.c reads 1.0.0.pchtxt on every version)
    variants = json.loads((ROOT/'tools/rando/patch_variants.json').read_text())
    sites = {int(a, 16): 4 for a in variants['code']} | {int(a, 16): 16 for a in variants['data']}
    if args.seeds:
        for pch in sorted(args.seeds.glob('*/exefs/1.0.0.pchtxt')):
            enabled = False
            for line in pch.read_text(encoding='utf-8', errors='replace').splitlines():
                if line.startswith('@enabled'):
                    enabled = True
                elif line.startswith('@stop'):
                    enabled = False
                elif enabled and re.match(r'[0-9A-Fa-f]{8} ', line):
                    off, data = line.split(None, 1)
                    n = len(data.split('"')[1]) + 1 if data.startswith('"') else len(bytes.fromhex(data.split()[0]))
                    sites[int(off, 16)] = max(sites.get(int(off, 16), 0), n)
    first = next(iter(images.values()))
    for key, n in images.items():
        for off, size in sorted(sites.items()):
            assert bytes(first.image[off:off + size]) == bytes(n.image[off:off + size]), \
                f'{key}: the randomizer patch site main+{off:#x} differs'
    if len(images) > 1:
        print(f'{len(sites)} randomizer patch sites hold the same bytes in {", ".join(images)}')
    # the start screen's release label from the real files
    for key, folder in games.items():
        label = ' '.join(native('label', folder/'exefs'))
        print(f'{key}: {folder} is release {label}')
        assert label in (VERSIONS[key][0], *VERSIONS[key][0].split(' / ')), (key, label)
    # the committed tables are version_map.py's output
    if 'v1_0_0' in games:
        import version_map
        for key in games:
            if key == 'v1_0_0':
                continue
            mapper = version_map.Mapper(games['v1_0_0'], games[key])
            for kind in ('patches', 'runtime'):
                src = table_path('v1_0_0', kind)
                text, _ = version_map.map_header(src.read_text(encoding='utf-8'), load_table(src), mapper, VERSIONS[key][0])
                assert text == table_path(key, kind).read_text(encoding='utf-8'), f'{kind}_{key}.h: run tools/version_map.py'
            print(f'{key}: the committed tables match tools/version_map.py')

if args.compare_generated:
    def hashes(folder):
        result = {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in folder.rglob('*.c')}
        assert result, f'No generated C in {folder}'
        return result
    before, after = map(hashes, args.compare_generated)
    assert before == after, 'Generated file names or contents changed'
    print(f'{len(before)} generated C files are byte-identical')
print(f'{len(VERSIONS)} game versions; {len(patch_values)} patch addresses, {len(runtime_values)} runtime constants, '
      f'{len(patches.PATCHES)} patch outputs and {len(patches.HOOKS)} hooks passed')
