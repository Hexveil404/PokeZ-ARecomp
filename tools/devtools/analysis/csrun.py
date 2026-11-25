"""Runs captured compute dispatches again on this machine's GPU and compares with what the capture's GPU wrote.

#345: AMD GPUs end up with garbage in Z-A's indirect draw parameters after a while; the GPU write report named the
culling dispatches as the only writers, and the test build's capture (runtime/nvn/vulkan/exec.c gwc_want,
logs/gpuw-good-<frame>.bin / gpuw-bad-<frame>.bin) records each such dispatch's inputs ('DISB': constant buffers,
storage buffers, GLSL, local size, compiler constants) and outputs ('DISA': the storage over indirect parameter pages,
the textures it sampled). Run here with the same inputs: words that differ from the capture's output were computed
differently there (atomic appends may order differently: see the per-buffer counts, and the indirect parameter
blocks, which only add up).

    python csrun.py <capture.bin> [dispatch number ...]   (no numbers: every dispatch in it)

Needs csrun.exe next to this file (csrun.c; build line there) and fcread.py.
"""
import os, re, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fcread  # noqa: E402

TEX_HDR = '<8I4f8I'  # FcTexImg (exec.c): index, vkformat, width, height, layers, levels, samples, aspect, sc[2], fr[2],
TEX_HDR_N = struct.calcsize(TEX_HDR)  # wh[2], has_view, view_base, view_levels, view_format, view_mask, data_bytes; + 40 B sampler


def blobs(d, kind):
    return [(s, a, sz, dt) for k, s, a, sz, dt in d['blobs'] if k == kind]


def sampler_args(raw):
    """NvnSamplerDesc's first 40 bytes -> Vulkan min, mag, mip mode, address mode, reduction, min / max lod
    (textures.c sampler_make)."""
    minf, magf, w0, w1, w2, cmpm, cmpf, red = raw[:8]
    lmin, lmax, bias, aniso = struct.unpack_from('<4f', raw, 8)
    vmin = 1 if minf in (1, 3, 5) else 0
    vmag = 1 if magf == 1 else 0
    mip = 1 if minf in (4, 5) else 0
    addr = {0: 2, 7: 2, 1: 0, 2: 4, 3: 4, 4: 3, 5: 3, 6: 1}.get(w0, 0)
    if lmax > 1000: lmax = 1000.0
    if minf <= 1: lmax = lmin + 0.25
    return vmin, vmag, mip, addr, red, lmin, lmax


def job(before, after, out):
    glsl = blobs(before, 23)[0][3].decode()
    sb = {s: (a, sz, dt) for s, a, sz, dt in blobs(before, 20)}
    ub = {s: (a, sz, dt) for s, a, sz, dt in blobs(before, 22)}
    lo = min(a for a, sz, dt in sb.values()) & ~0xFFFF
    hi = max(a + sz for a, sz, dt in sb.values())
    # the uniform buffers after the storage, each at its own address the table names (the runtime's ring copies)
    uaddr, top = {}, (hi + 0xFFFF) & ~0xFFFF
    for s, (a, sz, dt) in sorted(ub.items()):
        uaddr[s] = top
        top = (top + len(dt) + 255) & ~255
    size = (top - lo + 0xFFFF) & ~0xFFFF
    mem = bytearray(size)
    for s, (a, sz, dt) in sb.items(): mem[a - lo:a - lo + len(dt)] = dt
    for s, (a, sz, dt) in ub.items(): mem[uaddr[s] - lo:uaddr[s] - lo + len(dt)] = dt
    bases = set(re.findall(r'\) - 0x([0-9a-f]+)ul; bool ok = ga < 0x([0-9a-f]+)ul;', glsl))
    if len(bases) > 1: raise SystemExit(f'more than one guest window base in the GLSL: {bases}')
    if bases:
        (b0, n0), = bases
        glsl = glsl.replace(f') - 0x{b0}ul; bool ok = ga < 0x{n0}ul;', f') - 0x{lo:x}ul; bool ok = ga < 0x{size:x}ul;')
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, 'cs.comp'), 'w', newline='\n').write(glsl)
    open(os.path.join(out, 'mem.bin'), 'wb').write(mem)
    lines = ['glsl cs.comp']
    local = struct.unpack('<4I', blobs(before, 24)[0][3])
    lines.append('local %d %d %d' % local[:3])
    gx, gy, gz = before['groups']
    lines.append(f'groups {gx} {gy} {gz}')
    # c0: the driver constants (exec.c cbuf_info, compute): groups at 0x200, the uniform buffer table at 0x210, the
    # storage buffer table at 0x310 (address, size per 16 bytes)
    c0 = bytearray(0x800)
    struct.pack_into('<3I', c0, 0x200, gx, gy, gz)
    for s, (a, sz, dt) in ub.items():
        if s < 16: struct.pack_into('<QI', c0, 0x210 + 16 * s, uaddr[s], sz)
    for s, (a, sz, dt) in sb.items():
        if s < 16: struct.pack_into('<QI', c0, 0x310 + 16 * s, a, sz)
    decl = {int(k): int(n) * 16 for k, n in re.findall(r'binding = (\d+)\) uniform CB\d+ \{ uvec4 cb\d+\[(\d+)\]', glsl)}
    for k, n in decl.items():
        if k == 0: data = bytes(c0)
        elif k == 1: data = blobs(before, 25)[0][3] if blobs(before, 25) else b''
        else: data = ub.get(k - 3, (0, 0, b''))[2]
        data = bytes(data[:n]) + bytes(max(0, n - len(data)))
        open(os.path.join(out, f'cb{k}.bin'), 'wb').write(data)
        lines.append(f'ubo {k} cb{k}.bin')
    lines.append('ssbo 17 mem.bin')
    if 'binding = 20) uniform GMap' in glsl:  # (the fast-copy table: empty, everything in mem.bin)
        open(os.path.join(out, 'gmap.bin'), 'wb').write(bytes(17 * 16)); lines.append('ubo 20 gmap.bin')
    if 'binding = 21) buffer GFast' in glsl:
        open(os.path.join(out, 'gfast.bin'), 'wb').write(bytes(256)); lines.append('ssbo 21 gfast.bin')
    tex = {s: dt for s, a, sz, dt in blobs(after, 26)}
    if 'binding = 18) uniform RScale' in glsl:
        rs = [[1.0, 1.0, 0.0, 0.0]] + [[0.0] * 4 for _ in range(64)]
        for t, dt in tex.items():
            h = struct.unpack_from(TEX_HDR, dt, 0)
            rs[1 + t] = [h[8], h[9], h[10], h[11]]
        open(os.path.join(out, 'rscale.bin'), 'wb').write(b''.join(struct.pack('<4f', *r) for r in rs))
        lines.append('ubo 18 rscale.bin')
    for t, dt in tex.items():
        h = struct.unpack_from(TEX_HDR, dt, 0)
        fmt, w, ht, levels, aspect, nbytes = h[1], h[2], h[3], h[5], h[7], h[19]
        # (the runtime bound its 1x1 dummy: the depth pyramid at a non-integer scale, exec.c texture_info. Captures
        # before that flag - test-amd-capture 45a06d97 - tell it by the same rule: a mipmapped depth texture whose
        # scale isn't a whole number)
        dummy = bool(h[18] & 0x80000000) or (aspect & 2 and levels > 1 and (h[8] != int(h[8]) or h[9] != int(h[9])))
        if dummy:
            open(os.path.join(out, f'tex{t}.bin'), 'wb').write(bytes(4))
            lines.append(f'tex {32 + t} 100 1 1 1 0 tex{t}.bin 0 0 0 2 0 0.0 0.25')  # (R32_SFLOAT 0)
            continue
        if not nbytes: raise SystemExit(f'texture {t}: no texels in the capture (format {fmt})')
        open(os.path.join(out, f'tex{t}.bin'), 'wb').write(dt[TEX_HDR_N + 40:TEX_HDR_N + 40 + nbytes])
        vmin, vmag, mip, addr, red, lmin, lmax = sampler_args(dt[TEX_HDR_N:TEX_HDR_N + 40])
        lines.append(f'tex {32 + t} {fmt} {w} {ht} {levels} {1 if aspect & 2 else 0} tex{t}.bin {vmin} {vmag} {mip} {addr} {red} {lmin} {lmax}')
    open(os.path.join(out, 'job.txt'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    return lo, sb


def compare(after, lo, out, sb):
    mem = open(os.path.join(out, 'mem.bin.out'), 'rb').read()
    for s, a, sz, dt in blobs(after, 21):
        mine = mem[a - lo:a - lo + len(dt)]
        n = len(dt) // 4
        theirs = struct.unpack(f'<{n}I', dt)
        here = struct.unpack(f'<{n}I', mine[:n * 4])
        before = struct.unpack(f'<{n}I', sb[s][2][:n * 4]) if s in sb else here
        diff = [i for i in range(n) if theirs[i] != here[i]]
        changed_there = sum(1 for i in range(n) if theirs[i] != before[i])
        changed_here = sum(1 for i in range(n) if here[i] != before[i])
        print(f'  sb{s} {a:x}+{sz:x}: words written there {changed_there}, here {changed_here}, differing {len(diff)}')
        for i in diff[:12]:
            print(f'    {a + 4 * i:x}: before {before[i]:#x}  capture {theirs[i]:#x}  here {here[i]:#x}')


def main():
    path = sys.argv[1]
    fcread.load(path)
    ds = fcread.dispatches
    pairs = [(ds[i], ds[i + 1]) for i in range(len(ds) - 1) if not ds[i]['after'] and ds[i + 1]['after']]
    want = [int(x) for x in sys.argv[2:]] or range(len(pairs))
    exe = os.path.join(HERE, 'csrun.exe')
    for k in want:
        b, a = pairs[k]
        out = os.path.join(tempfile.gettempdir(), 'csrun', f'{os.path.basename(path)}-{k}')
        lo, sb = job(b, a, out)
        print(f'dispatch {k}: program {b["code"]:x}, {b["groups"]}, job {out}')
        r = subprocess.run([exe, out], capture_output=True, text=True)
        if r.returncode:
            print(r.stderr); continue
        compare(a, lo, out, sb)


if __name__ == '__main__':
    main()
