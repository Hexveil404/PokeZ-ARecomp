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
# here: regpromote.py leaves patched functions' registers in the Cpu struct.) 2.0.2 only: the one build checked (1.0.0
# / 2.0.0 likely have it at the same offset, unverified - the assert would stop their builds); there the 60 fps mode
# keeps the game's own step.
FRAME_STEP = {'b1f12fd919eae86a': ZA_BUILDS['b1f12fd919eae86a'] + 0xa0}  # 0x7001b29778


def _za_frame_step(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x2a1403e2, hex(w)  # mov w2, w20
    return ('{ extern uint32_t rt_za_frame_step(uint32_t, uint32_t); '
            'c->x[2] = rt_za_frame_step((uint32_t)c->x[20], (uint32_t)c->x[22]); }'
            f'  /* {a:#x}: mov w2, w20 (the channels\' step -> the decoupled one, any frame mode) */')


def za_patches(build):
    out = {ZA_BUILDS[build]: _za_frame_cfg}
    if build in FRAME_DT:
        out[FRAME_DT[build]] = _za_frame_dt
    if build in FRAME_STEP:
        out[FRAME_STEP[build]] = _za_frame_step
    return out
