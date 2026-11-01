# What lasgen (the frozen tools/gen.py) runs instead of unicorn: the few calls gen.py makes to work out jump tables,
# interpreted in plain Python. The real unicorn generates machine code at run time: it reserves a buffer, commits it from
# an exception handler at each first touch, and longjmps out of the code it wrote. A PC where any of that is refused loses
# the generator on the very first call (#99: 0xC0000409 right after the first commit's access violation, on one Windows
# 11 PC, in Z-A and in another game built the same way). Nothing here needs executable memory or an exception handler.
# gen.py's output with this file is byte for byte the one with unicorn 2.1.4 (Z-A 1.0.0 and 2.0.0, main and sdk).
# Source checkouts (`make gen`, tools/test_*.py) keep the real unicorn: only the frozen lasgen installs this one
# (tools/lasgen_utf8.py). Not in GAMELIB_SRCS: changing it doesn't make players recompile.
#
# Floating point and vector results aren't worked out: such a register is None, and reads as 0 where it reaches a general
# register (v_low). A value not known in an address, a branch or as the jump's target raises UcError: gen.py then leaves
# that table to its run-time dispatch, as for any table it can't resolve (slower, still right). So does anything this
# file doesn't decode.
import sys
import types

M64 = (1 << 64) - 1
PAGE = 0x1000


class UcError(Exception):
    pass


class arm64_const:
    UC_ARM64_REG_X0 = 1000  # X0-X28 follow; the numbers are only this file's own
    UC_ARM64_REG_X29 = 1029
    UC_ARM64_REG_X30 = 1030
    UC_ARM64_REG_SP = 1031
    UC_ARM64_REG_PC = 1032
    UC_ARM64_REG_CPACR_EL1 = 1033


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def ror(v, n, size):
    n %= size
    m = (1 << size) - 1
    v &= m
    return ((v >> n) | (v << (size - n))) & m if n else v


def bitmask(n, imms, immr, size, logical=True):
    """(wmask, tmask) of a logical immediate or a bitfield; None if reserved."""
    x = (n << 6) | (~imms & 0x3F)
    if x == 0:
        return None
    length = x.bit_length() - 1
    if length < 1:
        return None
    levels = (1 << length) - 1
    s, r = imms & levels, immr & levels
    if logical and s == levels:
        return None
    esize = 1 << length
    welem = (1 << (s + 1)) - 1
    d = (s - r) & levels
    telem = (1 << (d + 1)) - 1
    wmask = tmask = 0
    rotated = ror(welem, r, esize)
    for i in range(0, size, esize):
        wmask |= rotated << i
        tmask |= telem << i
    return wmask & ((1 << size) - 1), tmask & ((1 << size) - 1)


class Uc:
    def __init__(self, arch, mode):
        self.maps = []  # (start, end, bytearray)
        self.x = [0] * 31
        self.sp = 0
        self.pc = 0
        self.nzcv = 4  # None: not known (Z set: how the real thing starts)
        self.v = [bytes(16)] * 32  # vector registers: bytes (zero or from a load) or None
        self.unknown = set()  # memory addresses holding a value not known

    # --------------------------------------------------- the calls gen.py makes

    def mem_map(self, addr, size):
        self.maps.append((addr, addr + size, bytearray(size)))

    def mem_write(self, addr, data):
        s, e, b = self._at(addr)
        b[addr - s:addr - s + len(data)] = data

    def reg_write(self, reg, value):
        if 1000 <= reg <= 1030:
            self.x[reg - 1000] = value & M64
        elif reg == 1031:
            self.sp = value & M64
        elif reg == 1032:
            self.pc = value & M64

    def reg_read(self, reg):
        if reg == 1032:
            return self.pc
        v = self.x[reg - 1000] if 1000 <= reg <= 1030 else self.sp
        if v is None:
            raise UcError('value not known')
        return v

    def emu_start(self, pc, until, timeout=0, count=0):
        s = self._at(pc)
        self.pc = pc
        try:
            self.step(int.from_bytes(s[2][pc - s[0]:pc - s[0] + 4], 'little'))
        except (TypeError, ValueError, IndexError, OverflowError) as e:  # (a form not met yet: never the generator's end)
            raise UcError(f'{pc:#x}: {e}')

    # --------------------------------------------------- memory

    def _at(self, addr):
        for m in self.maps:
            if m[0] <= addr < m[1]:
                return m
        raise UcError('unmapped memory')

    def load(self, addr, n):
        if addr is None:
            raise UcError('address not known')
        addr &= M64
        s, e, b = self._at(addr)
        if addr + n > e:
            self._at(addr + n - 1)
            return b''.join(self.load(addr + i, 1) for i in range(n))
        return bytes(b[addr - s:addr - s + n])

    def load_int(self, addr, n):
        if self.unknown and addr is not None and any((addr + i) & M64 in self.unknown for i in range(n)):
            self.load(addr, n)
            return None
        return int.from_bytes(self.load(addr, n), 'little')

    def store(self, addr, n, data):
        """data: little-endian bytes of n, or None: a value not known"""
        if addr is None:
            raise UcError('address not known')
        addr &= M64
        for i in range(n):
            self._at((addr + i) & M64)
        if data is None:
            self.unknown.update((addr + i) & M64 for i in range(n))
            data = bytes(n)
        elif self.unknown:
            self.unknown.difference_update((addr + i) & M64 for i in range(n))
        for i in range(n):
            s, e, b = self._at((addr + i) & M64)
            b[addr + i - s] = data[i]

    # --------------------------------------------------- registers

    def r(self, n, sf=1):  # register or zero
        if n == 31:
            return 0
        v = self.x[n]
        return v if v is None or sf else v & 0xFFFFFFFF

    def rs(self, n, sf=1):  # register or SP
        if n == 31:
            return self.sp if sf else None if self.sp is None else self.sp & 0xFFFFFFFF
        return self.r(n, sf)

    def w(self, n, v, sf=1):
        if n != 31:
            self.x[n] = v if v is None else v & (M64 if sf else 0xFFFFFFFF)

    def ws(self, n, v, sf=1):
        if n == 31:
            self.sp = v if v is None else v & (M64 if sf else 0xFFFFFFFF)
        else:
            self.w(n, v, sf)

    def cond(self, c):
        if self.nzcv is None:
            raise UcError('flags not known')
        n, z, k, v = (self.nzcv >> 3) & 1, (self.nzcv >> 2) & 1, (self.nzcv >> 1) & 1, self.nzcv & 1
        r = (z, k, n, v, k and not z, n == v, n == v and not z, True)[c >> 1]
        return (not r) if (c & 1) and c != 15 else bool(r)

    def add_with_flags(self, a, b, carry, size, setflags):
        m = (1 << size) - 1
        u = a + b + carry
        res = u & m
        if setflags:
            s = sx(a, size) + sx(b, size) + carry
            self.nzcv = ((res >> (size - 1)) << 3) | ((res == 0) << 2) | ((u != res) << 1) | int(s != sx(res, size))
        return res

    def shifted(self, v, kind, n, size):
        if v is None:
            return None
        m = (1 << size) - 1
        v &= m
        if kind == 0:
            return (v << n) & m
        if kind == 1:
            return v >> n
        if kind == 2:
            return (sx(v, size) >> n) & m
        return ror(v, n, size)

    def extended(self, v, opt, shift, size):
        if v is None:
            return None
        n = 8 << (opt & 3)
        v &= (1 << n) - 1
        if opt & 4:
            v = sx(v, n)
        return (v << shift) & ((1 << size) - 1)

    # --------------------------------------------------- one instruction

    def step(self, w):
        next_pc = self.pc + 4
        top = (w >> 25) & 0xF
        if top in (8, 9):
            self.data_imm(w)
        elif top in (10, 11):
            next_pc = self.branch(w, next_pc)
        elif top & 5 == 5:
            if top & 7 == 7:
                self.simd(w)
            else:
                self.data_reg(w)
        elif top & 5 == 4:
            self.load_store(w)
        else:
            raise UcError(f'undefined instruction {w:#x}')
        self.pc = next_pc & M64

    def data_imm(self, w):
        sf, rd, rn = w >> 31, w & 31, (w >> 5) & 31
        size = 64 if sf else 32
        kind = (w >> 23) & 7
        if kind < 2:  # ADR, ADRP
            imm = sx(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21)
            self.w(rd, (self.pc & ~0xFFF) + (imm << 12) if sf else self.pc + imm)
        elif kind == 2:  # ADD, SUB (immediate)
            imm = ((w >> 10) & 0xFFF) << (12 if (w >> 22) & 1 else 0)
            sub, setf = (w >> 30) & 1, (w >> 29) & 1
            a = self.rs(rn, sf)
            if a is None:
                res = None
                if setf:
                    self.nzcv = None
            else:
                b = imm ^ ((1 << size) - 1) if sub else imm
                res = self.add_with_flags(a, b, sub, size, setf)
            (self.w if setf else self.ws)(rd, res, sf)
        elif kind == 4:  # logical (immediate)
            n = (w >> 22) & 1
            if not sf and n:
                raise UcError('reserved')
            mask = bitmask(n, (w >> 10) & 63, (w >> 16) & 63, size)
            if mask is None:
                raise UcError('reserved')
            imm, opc = mask[0], (w >> 29) & 3
            a = self.r(rn, sf)
            res = None if a is None else (a & imm, a | imm, a ^ imm, a & imm)[opc]
            if opc == 3:
                self.flags_logic(res, size)
                self.w(rd, res, sf)
            else:
                self.ws(rd, res, sf)
        elif kind == 5:  # MOVN, MOVZ, MOVK
            opc, hw = (w >> 29) & 3, (w >> 21) & 3
            if opc == 1 or (not sf and hw > 1):
                raise UcError('reserved')
            imm = ((w >> 5) & 0xFFFF) << (hw * 16)
            if opc == 0:
                self.w(rd, ~imm & ((1 << size) - 1), sf)
            elif opc == 2:
                self.w(rd, imm, sf)
            else:
                a = self.r(rd, sf)
                self.w(rd, None if a is None else (a & ~(0xFFFF << (hw * 16))) | imm, sf)
        elif kind == 6:  # SBFM, BFM, UBFM
            opc, n, immr, imms = (w >> 29) & 3, (w >> 22) & 1, (w >> 16) & 63, (w >> 10) & 63
            if opc == 3 or n != sf:
                raise UcError('reserved')
            mask = bitmask(n, imms, immr, size, False)
            if mask is None:
                raise UcError('reserved')
            wmask, tmask = mask
            a = self.r(rn, sf)
            d = self.r(rd, sf) if opc == 1 else 0
            if a is None or d is None:
                self.w(rd, None, sf)
                return
            bot = (d & ~wmask) | (ror(a, immr, size) & wmask)
            if opc == 0:
                sign = (a >> imms) & 1
                top = ((1 << size) - 1) if sign else 0
            else:
                top = d
            self.w(rd, (top & ~tmask) | (bot & tmask), sf)
        elif kind == 7:  # EXTR
            rm, lsb = (w >> 16) & 31, (w >> 10) & 63
            a, b = self.r(rn, sf), self.r(rm, sf)
            if a is None or b is None:
                self.w(rd, None, sf)
            else:
                self.w(rd, (((a << size) | b) >> lsb) & ((1 << size) - 1), sf)
        else:
            raise UcError(f'undefined instruction {w:#x}')

    def flags_logic(self, res, size):
        self.nzcv = None if res is None else ((res >> (size - 1)) << 3) | ((res == 0) << 2)

    def branch(self, w, next_pc):
        rt = w & 31
        if (w >> 26) & 31 == 5:  # B, BL
            if w >> 31:
                self.x[30] = next_pc
            return self.pc + sx(w, 26) * 4
        if (w >> 25) & 0x3F == 0x1A:  # CBZ, CBNZ
            sf = w >> 31
            v = self.r(rt, sf)
            if v is None:
                raise UcError('value not known')
            return self.pc + sx(w >> 5, 19) * 4 if (v == 0) != bool((w >> 24) & 1) else next_pc
        if (w >> 25) & 0x3F == 0x1B:  # TBZ, TBNZ
            v = self.r(rt)
            if v is None:
                raise UcError('value not known')
            bit = (w >> 31) << 5 | ((w >> 19) & 31)
            return self.pc + sx(w >> 5, 14) * 4 if ((v >> bit) & 1) == ((w >> 24) & 1) else next_pc
        if w >> 24 == 0x54 and not w & 16:  # B.cond
            return self.pc + sx(w >> 5, 19) * 4 if self.cond(w & 15) else next_pc
        if w >> 24 == 0xD4:
            raise UcError('exception')
        if w >> 22 == 0x354:  # hints, barriers, MRS, MSR
            if (w & 0xFFFFF01F) == 0xD503201F or (w & 0xFFFFF01F) == 0xD503301F:
                return next_pc
            if (w >> 20) & 0xFFF == 0xD53:  # MRS
                self.w(rt, 0 if (w >> 5) & 0x7FFF == 0x5E82 else None)  # (TPIDR_EL0 starts at 0)
                return next_pc
            if (w >> 20) & 0xFFF == 0xD51:  # MSR
                return next_pc
            raise UcError('system instruction')
        if w >> 25 == 0x6B:  # BR, BLR, RET
            opc = (w >> 21) & 15
            if opc > 2:
                raise UcError('undefined instruction')
            t = self.r((w >> 5) & 31)
            if t is None:
                raise UcError('value not known')
            if opc == 1:
                self.x[30] = next_pc
            return t
        raise UcError(f'undefined instruction {w:#x}')

    def data_reg(self, w):
        sf, rd, rn, rm = w >> 31, w & 31, (w >> 5) & 31, (w >> 16) & 31
        size = 64 if sf else 32
        op = (w >> 24) & 31
        mask = (1 << size) - 1
        if op == 0x0A:  # logical (shifted register)
            opc, neg, sh, imm = (w >> 29) & 3, (w >> 21) & 1, (w >> 22) & 3, (w >> 10) & 63
            if not sf and imm > 31:
                raise UcError('reserved')
            a, b = self.r(rn, sf), self.shifted(self.r(rm, sf), sh, imm, size)
            if neg and b is not None:
                b ^= mask
            res = None if a is None or b is None else (a & b, a | b, a ^ b, a & b)[opc]
            if opc == 3:
                self.flags_logic(res, size)
            self.w(rd, res, sf)
        elif op in (0x0B,):
            sub, setf = (w >> 30) & 1, (w >> 29) & 1
            if (w >> 21) & 1:  # extended register
                opt, shift = (w >> 13) & 7, (w >> 10) & 7
                if shift > 4:
                    raise UcError('reserved')
                a, b = self.rs(rn, sf), self.extended(self.r(rm, 1), opt, shift, size)
                put = self.w if setf else self.ws
            else:  # shifted register
                sh, imm = (w >> 22) & 3, (w >> 10) & 63
                if sh == 3 or (not sf and imm > 31):
                    raise UcError('reserved')
                a, b = self.r(rn, sf), self.shifted(self.r(rm, sf), sh, imm, size)
                put = self.w
            if a is None or b is None:
                if setf:
                    self.nzcv = None
                put(rd, None, sf)
                return
            put(rd, self.add_with_flags(a, b ^ mask if sub else b, sub, size, setf), sf)
        elif op == 0x1A:
            self.data_reg_1a(w, sf, size, rd, rn, rm)
        elif op == 0x1B:  # MADD, MSUB, S/UMADDL, S/UMSUBL, S/UMULH
            op31, o0, ra = (w >> 21) & 7, (w >> 15) & 1, (w >> 10) & 31
            if op31 == 0:
                a, b, c = self.r(rn, sf), self.r(rm, sf), self.r(ra, sf)
                if a is None or b is None or c is None:
                    self.w(rd, None, sf)
                else:
                    self.w(rd, c - a * b if o0 else c + a * b, sf)
            elif op31 in (1, 5):
                a, b, c = self.r(rn, 0), self.r(rm, 0), self.r(ra)
                if a is None or b is None or c is None:
                    self.w(rd, None)
                else:
                    if op31 == 1:
                        a, b = sx(a, 32), sx(b, 32)
                    self.w(rd, c - a * b if o0 else c + a * b)
            elif op31 in (2, 6):
                a, b = self.r(rn), self.r(rm)
                if a is None or b is None:
                    self.w(rd, None)
                else:
                    if op31 == 2:
                        a, b = sx(a, 64), sx(b, 64)
                    self.w(rd, (a * b) >> 64)
            else:
                raise UcError('undefined instruction')
        else:
            raise UcError(f'undefined instruction {w:#x}')

    def data_reg_1a(self, w, sf, size, rd, rn, rm):
        mask = (1 << size) - 1
        kind = (w >> 21) & 7
        if kind == 0:  # ADC, SBC
            sub, setf = (w >> 30) & 1, (w >> 29) & 1
            a, b = self.r(rn, sf), self.r(rm, sf)
            if a is None or b is None or self.nzcv is None:
                self.w(rd, None, sf)
                if setf:
                    self.nzcv = None
                return
            self.w(rd, self.add_with_flags(a, b ^ mask if sub else b, (self.nzcv >> 1) & 1, size, setf), sf)
        elif kind == 2:  # CCMN, CCMP
            sub = (w >> 30) & 1
            imm = (w >> 11) & 1
            c = (w >> 12) & 15
            a = self.r(rn, sf)
            b = rm if imm else self.r(rm, sf)
            if self.nzcv is None:
                pass
            elif self.cond(c):
                if a is None or b is None:
                    self.nzcv = None
                else:
                    self.add_with_flags(a, b ^ mask if sub else b, sub, size, True)
            else:
                self.nzcv = w & 15
        elif kind == 4:  # CSEL, CSINC, CSINV, CSNEG
            op, o2, c = (w >> 30) & 1, (w >> 10) & 1, (w >> 12) & 15
            if self.nzcv is None:
                self.w(rd, None, sf)
                return
            a, b = self.r(rn, sf), self.r(rm, sf)
            if self.cond(c):
                res = a
            elif b is None:
                res = None
            elif not op:
                res = b + o2
            else:
                res = -b if o2 else ~b
            self.w(rd, res, sf)
        elif kind == 6 and (w >> 30) & 1:  # one source
            op = (w >> 10) & 63
            a = self.r(rn, sf)
            if a is None:
                self.w(rd, None, sf)
            elif op == 0:  # RBIT
                self.w(rd, int(format(a, f'0{size}b')[::-1], 2), sf)
            elif op in (1, 2, 3):  # REV16, REV32 / REV
                chunk = 2 if op == 1 else 4 if op == 2 else 8
                if op == 2 and not sf:
                    chunk = 4
                if op == 3 and not sf:
                    raise UcError('undefined instruction')
                b = a.to_bytes(size // 8, 'little')
                self.w(rd, int.from_bytes(b''.join(b[i:i + chunk][::-1] for i in range(0, size // 8, chunk)), 'little'), sf)
            elif op == 4:  # CLZ
                self.w(rd, size - a.bit_length(), sf)
            elif op == 5:  # CLS
                s = sx(a, size)
                self.w(rd, size - (s if s >= 0 else ~s).bit_length() - 1, sf)
            else:
                raise UcError('undefined instruction')
        elif kind == 6:  # two sources
            op = (w >> 10) & 63
            a, b = self.r(rn, sf), self.r(rm, sf)
            if op not in (2, 3, 8, 9, 10, 11):
                raise UcError('undefined instruction')
            if a is None or b is None:
                self.w(rd, None, sf)
            elif op == 2 or op == 3:
                if op == 3:
                    a, b = sx(a, size), sx(b, size)
                if b == 0:
                    q = 0
                else:
                    q = abs(a) // abs(b)
                    if (a < 0) != (b < 0):
                        q = -q
                self.w(rd, q, sf)
            else:
                self.w(rd, self.shifted(a, op - 8, b % size, size), sf)
        else:
            raise UcError(f'undefined instruction {w:#x}')

    # --------------------------------------------------- loads and stores

    def base_addr(self, n, off, mode):
        """mode 0 post-index, 1 signed offset / no writeback, 2 pre-index -> (access address, writeback or None)"""
        b = self.rs(n)
        if b is None:
            raise UcError('address not known')
        return (b if mode == 0 else b + off) & M64, ((b + off) & M64 if mode != 1 else None)

    @staticmethod
    def signed(v, bits):
        return None if v is None else sx(v, bits)

    def get_v(self, n, size):
        v = self.v[n]
        return None if v is None else v[:size]

    def put_v(self, n, data):
        self.v[n] = None if data is None else data.ljust(16, b'\0')

    def load_store(self, w):
        vec = (w >> 26) & 1
        rt, rn = w & 31, (w >> 5) & 31
        size_bits = w >> 30
        if (w >> 27) & 7 == 5 and (w >> 28) & 3 == 2:  # pairs
            return self.load_store_pair(w, vec, rt, rn)
        kind = (w >> 28) & 3
        if kind == 1 and (w >> 27) & 7 == 3:  # literal
            off = sx(w >> 5, 19) * 4
            a = self.pc + off
            if vec:
                n = 4 << size_bits
                self.put_v(rt, self.load(a, n))
            elif size_bits == 2:
                self.w(rt, self.signed(self.load_int(a, 4), 32))
            elif size_bits != 3:  # (3: PRFM)
                self.w(rt, self.load_int(a, 8 if size_bits else 4))
            return
        if kind == 0 and not vec and (w >> 27) & 7 == 1:  # exclusives, acquire / release
            n = 1 << size_bits
            a = self.rs(rn)
            if a is None or ((w >> 23) & 1 and (w >> 21) & 1):  # (CAS and the like: not in this game's tables)
                raise UcError('address not known' if a is None else 'atomic')
            if (w >> 22) & 1:
                self.w(rt, self.load_int(a, n))
                if (w >> 21) & 1:
                    self.w((w >> 10) & 31, self.load_int(a + n, n))
            else:
                v = self.r(rt)
                self.store(a, n, None if v is None else (v & ((1 << n * 8) - 1)).to_bytes(n, 'little'))
                if not (w >> 23) & 1:
                    self.w((w >> 16) & 31, 0)
            return
        if kind != 3:
            raise UcError(f'undefined instruction {w:#x}')
        opc = (w >> 22) & 3
        if size_bits == 3 and opc == 2 and not vec:  # PRFM: touches nothing, whatever its address
            return
        if (w >> 24) & 1:  # unsigned offset
            scale = (size_bits | (4 if vec and opc >= 2 else 0))
            addr, wb = self.base_addr(rn, ((w >> 10) & 0xFFF) << scale, 1)
        else:
            mode = (w >> 10) & 3
            if (w >> 21) & 1:
                if mode != 2:
                    raise UcError(f'undefined instruction {w:#x}')  # (atomics: not in this game's tables)
                scale = (size_bits | (4 if vec and opc >= 2 else 0))
                rm, opt, s = (w >> 16) & 31, (w >> 13) & 7, (w >> 12) & 1
                b = self.rs(rn)
                off = self.extended(self.r(rm), opt, scale if s else 0, 64)
                if b is None or off is None:
                    raise UcError('address not known')
                addr, wb = (b + off) & M64, None
            else:
                off = sx(w >> 12, 9)
                addr, wb = self.base_addr(rn, off, {0: 1, 1: 0, 2: 1, 3: 2}[mode])
        if vec:
            n = 16 if opc >= 2 and not size_bits else 1 << size_bits
            if opc & 1:
                self.put_v(rt, self.load(addr, n))
            else:
                self.store(addr, n, self.get_v(rt, n))
        else:
            n = 1 << size_bits
            if opc == 0:
                v = self.r(rt)
                self.store(addr, n, None if v is None else (v & ((1 << n * 8) - 1)).to_bytes(n, 'little'))
            elif opc == 1:
                self.w(rt, self.load_int(addr, n), 0 if n < 8 else 1)
            else:
                self.w(rt, self.signed(self.load_int(addr, n), n * 8), opc == 2)
        if wb is not None:
            self.ws(rn, wb)

    def load_store_pair(self, w, vec, rt, rn):
        opc, mode, is_load = w >> 30, (w >> 23) & 3, (w >> 22) & 1
        rt2 = (w >> 10) & 31
        scale = 2 + opc if vec else 2 + (opc >> 1)
        if not vec and opc == 3:
            raise UcError(f'undefined instruction {w:#x}')
        n = 1 << scale
        off = sx(w >> 15, 7) << scale
        addr, wb = self.base_addr(rn, off, {0: 1, 1: 0, 2: 1, 3: 2}[mode])
        if vec:
            if is_load:
                a, b = self.load(addr, n), self.load(addr + n, n)
                self.put_v(rt, a)
                self.put_v(rt2, b)
            else:
                self.store(addr, n, self.get_v(rt, n))
                self.store(addr + n, n, self.get_v(rt2, n))
        elif is_load:
            a, b = self.load_int(addr, n), self.load_int(addr + n, n)
            if opc == 1:  # LDPSW
                self.w(rt, self.signed(a, 32))
                self.w(rt2, self.signed(b, 32))
            else:
                self.w(rt, a, opc >> 1)
                self.w(rt2, b, opc >> 1)
        else:
            for i, r in enumerate((rt, rt2)):
                v = self.r(r)
                self.store(addr + i * n, n, None if v is None else (v & ((1 << n * 8) - 1)).to_bytes(n, 'little'))
        if wb is not None:
            self.ws(rn, wb)

    # --------------------------------------------------- floating point, vectors: results not worked out

    def simd(self, w):
        rd = w & 31
        if (w >> 24) & 0x5F == 0x1E and (w >> 21) & 1:  # scalar FP, FP <-> integer
            if (w >> 10) & 63 == 0 and not (w >> 29) & 1:
                opcode = (w >> 16) & 7
                if opcode in (2, 3) or opcode == 7:  # SCVTF, UCVTF, FMOV (to FP)
                    self.v[rd] = None
                else:  # FCVT*, FMOV (to a general register)
                    self.w(rd, self.v_low((w >> 5) & 31))
                return
            if (w >> 10) & 15 == 8 or (w >> 10) & 3 == 1:  # FCMP, FCCMP
                self.nzcv = None
                return
        elif (w & 0xBFE08400) == 0x0E000400 and ((w >> 11) & 15) in (5, 7):  # SMOV, UMOV
            self.w(rd, self.v_low((w >> 5) & 31))
            return
        self.v[rd] = None

    def v_low(self, n):
        # (19 tables of main's, a NEON decoder: bits picked out of its data choose a row of a constant table before the
        # bounds-checked jump. The real unicorn runs them on whatever its vector registers hold, so 0 is no worse.)
        v = self.v[n]
        return 0 if v is None else int.from_bytes(v[:8], 'little')


unicorn = types.ModuleType('unicorn')
unicorn.Uc = Uc
unicorn.UcError = UcError
unicorn.UC_ARCH_ARM64 = 1
unicorn.UC_MODE_ARM = 0
unicorn.arm64_const = arm64_const


def install():
    sys.modules['unicorn'] = unicorn
    sys.modules['unicorn.arm64_const'] = arm64_const
