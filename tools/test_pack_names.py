#!/usr/bin/env python3
"""Regression test for #229 (button prompts: PlayStation / Xbox icons never shown, whatever the setting).

The texture packs are named offline by hashing the game's texture bytes, and the runtime looks a texture up by
hashing the same bytes. The button prompt sets (tools/za/prompt_textures.py hash_full) and the HD pack
(tools/za/hd_pack.py -> hd_native.c pack_hash) use different hashes; the runtime looked both up by the HD pack's,
so no prompt texture was ever found. This checks, for textures of many sizes, that the name the runtime asks for
(runtime/nvn/tex_hash.h tex_pack_name) is the prompt generator's file name for a texture in the prompt set and the
HD pack builder's for any other - with the generators' own hash code, so a change on either side fails here.
"""
import ast, os, random, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def generator_hash():
    """prompt_textures.py's hash_full (and the names it needs), without running the script."""
    src = open(os.path.join(ROOT, 'tools', 'za', 'prompt_textures.py'), encoding='utf-8').read()
    tree = ast.parse(src)
    keep = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name == 'hash_full') or
            (isinstance(n, ast.Assign) and any(getattr(t, 'id', '') == 'M64' for t in n.targets))]
    assert len(keep) == 2, 'prompt_textures.py: hash_full / M64 not found'
    env = {'struct': __import__('struct')}
    exec(compile(ast.Module(body=keep, type_ignores=[]), 'prompt_textures.py', 'exec'), env)
    return env['hash_full']


def main():
    cc = os.environ.get('CC', 'clang')
    tmp = tempfile.mkdtemp(prefix='pack-names-')
    exe = os.path.join(tmp, 'test-pack-names' + ('.exe' if os.name == 'nt' else ''))
    subprocess.check_call([cc, '-std=gnu11', '-O1', '-Itools', os.path.join(ROOT, 'tools', 'test_pack_names.c'),
                           '-o', exe], cwd=ROOT)
    prompt_hash = generator_hash()
    rnd = random.Random(229)
    # sizes: block multiples of the BC formats the icons use, and odd tails both hashes treat byte by byte
    sizes = [16, 31, 32, 33, 63, 64, 65, 256, 1000, 4096, 8192, 65536, 65537, 262144, 300001]
    files, in_prompts = [], []
    for i, n in enumerate(sizes * 2):
        p = os.path.join(tmp, f't{i}.bin')
        with open(p, 'wb') as f:
            f.write(bytes(rnd.getrandbits(8) for _ in range(n)))
        files.append(p)
        in_prompts.append(i % 2 == 0)
    datas = [open(p, 'rb').read() for p in files]
    names = os.path.join(tmp, 'prompts.txt')
    with open(names, 'w') as f:  # (as textures/prompts/<set>/<name>.png: the generator's names)
        for d, inp in zip(datas, in_prompts):
            if inp: f.write(f'{prompt_hash(d):016x}\n')
    out = subprocess.check_output([exe, names] + files, text=True).split('\n')
    bad = 0
    for i, (d, inp) in enumerate(zip(datas, in_prompts)):
        got, hd = (int(x, 16) for x in out[i].split())
        gen = prompt_hash(d)
        assert gen != hd, 'the two hashes agree: the test proves nothing'
        want = gen if inp else hd
        if got != want:
            bad += 1
            print(f'FAIL {len(d)} bytes ({"prompt set" if inp else "HD pack"}): runtime asks {got:016x}, '
                  f'pack file is {want:016x}')
    if bad:
        print(f'{bad} of {len(datas)} textures would not be found (#229)')
        return 1
    print(f'ok: {len(datas)} textures, button prompt and HD pack names match the runtime')
    return 0


if __name__ == '__main__':
    sys.exit(main())
