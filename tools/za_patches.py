"""Pokémon Legends: Z-A's game patches (tools/gen.py: exefs/main builds in ZA_BUILDS), apart from tools/patches.py:
that one builds Link's Awakening's tables from its address table on import (tools/game_addresses.py), which has no
Z-A version."""
import struct

# Frame mode: the game's frame clock (f_70019e3730) reads its configured "vsyncs per frame - 1" byte (1 on the
# Switch: 30 fps) from [[x19, #0x40], #0x20]; it sets the present interval from it and hands every subsystem
# max(that, the vsyncs that really passed) as the frame's step. Through rt_za_frame_cfg the 60 fps option makes it 0:
# a present every vsync and a 1/60 s step - the world keeps its speed.
ZA_BUILDS = {'7222e13ecf6adb32': 0x70019e37a8,  # 1.0.0
             '8a1c86c437394b69': 0x7001b29cf8,  # 2.0.0 (Mega Dimension; the same function)
             'b1f12fd919eae86a': 0x7001b296d8}  # 2.0.2


def _za_frame_cfg(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x39c08134, hex(w)  # ldrsb w20, [x9, #0x20]
    return ('{ extern int32_t rt_za_frame_cfg(int32_t); '
            'c->x[20] = (uint32_t)rt_za_frame_cfg((int8_t)LD8(c->x[9] + 0x20)); }'
            f'  /* {a:#x}: ldrsb w20, [x9, #0x20] (frame mode) */')


# The frame step's duration read (`ldr x8, [x1, #8]` at the function's head, 0x68 before the config read above). Its
# result is the world step, an integer number of 1/60 s vsyncs; runtime/hle/framepace.c rt_za_frame_dt makes it track
# wall-clock time instead of the frame rate (no slow motion below the chosen rate; F1 > Constant game speed).
FRAME_DT = {b: a - 0x68 for b, a in ZA_BUILDS.items()}


def _za_frame_dt(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    if w != 0xf9400428:  # (not this shape: another build's layout - the plain load, no decoupling)
        return 'c->x[8] = LD64(c->x[1] + 8);'
    return ('{ extern uint64_t rt_za_frame_dt(uint64_t); '
            'c->x[8] = rt_za_frame_dt(LD64(c->x[1] + 8)); }'
            f'  /* {a:#x}: ldr x8, [x1, #8] (frame duration -> world step, decoupled from the frame rate) */')


# The frame clock's update loop hands each clock channel the frame's step: `mov w2, w20` (0xa0 after the config read).
# In the game's 60 fps mode w20 is the configured 0 whatever the frame took, so the duration above never reaches it;
# runtime/hle/framepace.c rt_za_frame_step hands on max(w20, the accumulator's step) instead, leaving the frame
# clock's mode logic alone (LARECOMP_FRAME_STEP=0: w20). The channel's index (w22, the loop's counter) goes along:
# channel 0 keeps the game's own step (two ticks there leave the player stuck after a roll). (c->x[22] is current
# here: regpromote.py leaves patched functions' registers in the Cpu struct.) 2.0.2 and 2.0.0: the function is the
# same in both (0x200 bytes compared, only BL / ADRP+ADD targets differ), `mov w2, w20` at the same offset. 1.0.0 at
# that offset too, unchecked (no 1.0.0 files here): the word is checked at build time and anything else keeps the plain
# move, as FRAME_DT does. Without this hook the per-frame mode can't hold channel 0 back, and a roll left the player
# stuck on 1.0.0 (#188, ~45 fps): runtime/hle/framepace.c keeps that mode off where the hook never runs.
FRAME_STEP = {b: a + 0xa0 for b, a in ZA_BUILDS.items()}  # 2.0.2 0x7001b29778, 2.0.0 0x7001b29d98, 1.0.0 0x70019e3848


def _za_frame_step(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    if w != 0x2a1403e2:  # mov w2, w20 (not this shape: another layout - the plain move, no per-channel step)
        return 'c->x[2] = (uint32_t)c->x[20];'
    return ('{ extern uint32_t rt_za_frame_step(uint32_t, uint32_t); '
            'c->x[2] = rt_za_frame_step((uint32_t)c->x[20], (uint32_t)c->x[22]); }'
            f'  /* {a:#x}: mov w2, w20 (the channels\' step -> the decoupled one, any frame mode) */')


# The archive reader's OodleLZ_Decompress call (`bl` in f_7001a9bd38 on 2.0.0 / f_7001a9b718 on 2.0.2, matched by
# 0xa0 bytes with BL / ADRP targets masked: one hit) goes through runtime/services/fs.c rt_za_oodle_call, which decodes
# again when it returns 0 (#173 #206: right bytes, failed decode, only on Windows 11 build 26300). 1.0.0: not located
# (no 1.0.0 files here). The `bl`'s target is checked at build time; anything else keeps the plain call.
OODLE_CALL = {'8a1c86c437394b69': (0x7001a9bddc, 0x7001a9d000), 'b1f12fd919eae86a': (0x7001a9b7bc, 0x7001a9c9e0)}


def _za_oodle_call(gen, a, target):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    off = (w & 0x3ffffff) - (1 << 26 if w & 0x2000000 else 0)
    if w >> 26 != 0x25 or a + off * 4 != target:  # (not this call: the plain one)
        return f'c->x[30] = {a + 4:#x}ull; f_{target:x}(c);'
    return ('{ extern void rt_za_oodle_call(Cpu *, uint64_t); '
            f'c->x[30] = {a + 4:#x}ull; rt_za_oodle_call(c, {target:#x}ull); }}'
            f'  /* {a:#x}: bl {target:#x} (OodleLZ_Decompress, decoded again if it fails) */')


# Smooth distant animation: the per-frame animation update (f_7001dcdf40) reads one byte of each entity's context,
# `ldrb w8, [x8, #0x1f4]` ([x8] is [this+0x48]). Set: the entity is updated every frame. Clear: it goes through the
# reduced path that makes distant characters and Pokemon move in visible steps (about 24 of 70 entities are set in the
# station, chosen by distance and view). rt_za_smooth_anim (runtime/hle/framepace.c; F1 > Smooth distant animation, on
# by default) hands the update a set byte whatever the game chose; the loaded value is what the update tests, and later
# tests again, so the one patch covers both. Found and verified in game in MonZaMac (lucabravi/MonZaMac#150, the same
# game on macOS/Metal). 2.0.2 only: the address and the word are checked there and nowhere else.
SMOOTH_ANIM = {'b1f12fd919eae86a': 0x7001dcdf60}


def _za_smooth_anim(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x3947d108, hex(w)  # ldrb w8, [x8, #0x1f4]
    return ('{ extern uint8_t rt_za_smooth_anim(uint8_t); '
            'c->x[8] = rt_za_smooth_anim((uint8_t)LD8(c->x[8] + 0x1f4)); }'
            f'  /* {a:#x}: ldrb w8, [x8, #0x1f4] (the entity updates every frame: distant ones too) */')


def za_patches(build):
    out = {ZA_BUILDS[build]: _za_frame_cfg}
    if build in FRAME_DT:
        out[FRAME_DT[build]] = _za_frame_dt
    if build in FRAME_STEP:
        out[FRAME_STEP[build]] = _za_frame_step
    if build in OODLE_CALL:
        a, target = OODLE_CALL[build]
        out[a] = lambda gen, a, target=target: _za_oodle_call(gen, a, target)
    if build in SMOOTH_ANIM:
        out[SMOOTH_ANIM[build]] = _za_smooth_anim
    return out
