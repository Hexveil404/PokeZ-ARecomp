"""IPSwitch .pchtxt code patches (a mod's exefs/*.pchtxt): `offset hexbytes` lines in @enabled blocks, applied to
the NSO's image before it is lifted (gen.py --patch). The offsets are the IPS file's: the decompressed NSO's, header
(0x100) included, minus `@flag offset_shift` (0x100 in every Z-A patch seen: the offsets are then image offsets).
Only byte patches: IPSwitch's "string" values and anything else unknown in an enabled block are refused, not
skipped - a half-applied patch is worse than none."""
import re

_LINE = re.compile(r'^([0-9A-Fa-f]{1,8})\s+([0-9A-Fa-f]+)$')


def parse_pchtxt(text):
    """(build id lowercased, [(image offset, bytes)]) of the @enabled blocks."""
    bid, shift, enabled, out = '', 0, False, []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split('//', 1)[0].strip()
        if not line or line.startswith('#'):
            continue
        low = line.lower()
        if low.startswith('@nsobid-'):
            bid = line[8:].strip().lower()
        elif low.startswith('@flag offset_shift'):
            shift = int(line.split()[2], 0)
        elif low == '@enabled':
            enabled = True
        elif low in ('@disabled', '@stop'):
            enabled = False
        elif line.startswith('@'):
            continue  # (@flag print_values, author credits)
        elif enabled:
            m = _LINE.match(line)
            if not m or len(m.group(2)) % 2:
                raise ValueError(f'line {n}: unsupported patch line {raw.strip()!r}')
            out.append((int(m.group(1), 16) + shift - 0x100, bytes.fromhex(m.group(2))))
    return bid, out


def apply(image, patches):
    """Writes the patches into image (a bytearray); the number of bytes that changed. All or nothing: one outside the
    image raises before anything is written."""
    for off, data in patches:
        if off < 0 or off + len(data) > len(image):
            raise ValueError(f'patch at {off:#x} (+{len(data)}) is outside the image ({len(image):#x})')
    changed = 0
    for off, data in patches:
        changed += sum(a != b for a, b in zip(image[off:off + len(data)], data))
        image[off:off + len(data)] = data
    return changed
