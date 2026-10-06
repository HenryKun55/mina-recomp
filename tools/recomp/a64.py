"""AArch64 instruction decoder for the recompiler.

Decodes by encoding fields (never by assembler aliases), so the semantics the
generator lowers are exactly the architecture's. Covers what the game's code
uses: integer, branches, loads/stores (incl. LSE atomics), scalar FP and the
Advanced SIMD subset (see simd.py).

Register numbering in decoded instructions: 0-30 general registers, 31 = the
zero register, SP = 32. FP/SIMD registers are 0-31 in their own file.
"""

from dataclasses import dataclass, field

SP = 32
ZR = 31

CONDS = ["eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
         "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"]


def bits(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def bit(w, n):
    return (w >> n) & 1


def sext(v, n):
    if v & (1 << (n - 1)):
        v -= 1 << n
    return v


@dataclass
class Insn:
    addr: int
    word: int
    op: str
    f: dict = field(default_factory=dict)

    def __getattr__(self, k):
        try:
            return self.f[k]
        except KeyError:
            raise AttributeError(k)

    def __repr__(self):
        return "%#x %s %s" % (self.addr, self.op, self.f)


def _ror(v, r, n):
    r %= n
    return ((v >> r) | (v << (n - r))) & ((1 << n) - 1)


def decode_bitmask(n, imms, immr, datasize):
    """DecodeBitMasks(): the wmask of a logical immediate (None if reserved)."""
    combined = (n << 6) | (~imms & 0x3F)
    if combined == 0:
        return None
    length = combined.bit_length() - 1
    if length < 1:
        return None
    levels = (1 << length) - 1
    s = imms & levels
    r = immr & levels
    if s == levels:
        return None
    esize = 1 << length
    welem = (1 << (s + 1)) - 1
    elem = _ror(welem, r, esize)
    out = 0
    for k in range(datasize // esize):
        out |= elem << (k * esize)
    return out


def vfp_expand_imm(imm8, size):
    """VFPExpandImm() as a Python float."""
    sign = bit(imm8, 7)
    b6 = bit(imm8, 6)
    if size == 32:
        e = ((b6 ^ 1) << 7) | ((b6 * 0x1F) << 2) | bits(imm8, 5, 4)
        frac = bits(imm8, 3, 0) << 19
        import struct
        raw = (sign << 31) | (e << 23) | frac
        return struct.unpack("<f", struct.pack("<I", raw))[0]
    e = ((b6 ^ 1) << 10) | ((b6 * 0xFF) << 2) | bits(imm8, 5, 4)
    frac = bits(imm8, 3, 0) << 48
    import struct
    raw = (sign << 63) | (e << 52) | frac
    return struct.unpack("<d", struct.pack("<Q", raw))[0]


# ---------------------------------------------------------------------------

def decode(word, addr):
    op0 = bits(word, 28, 25)
    if op0 & 0b1110 == 0b1000:
        return _dp_imm(word, addr)
    if op0 & 0b1110 == 0b1010:
        return _branch(word, addr)
    if op0 & 0b0101 == 0b0100:
        return _ldst(word, addr)
    if op0 & 0b0111 == 0b0101:
        return _dp_reg(word, addr)
    if op0 & 0b0111 == 0b0111:
        if bit(word, 28) and not bit(word, 30) and not bit(word, 29):
            r = _fp_scalar(word, addr)
            if r is not None:
                return r
        from simd import decode_simd
        return decode_simd(word, addr)
    return Insn(addr, word, "unk")


def _dp_imm(w, a):
    sf = bit(w, 31)
    k = bits(w, 25, 23)
    rd, rn = bits(w, 4, 0), bits(w, 9, 5)
    if k in (0b000, 0b001):
        imm = (bits(w, 23, 5) << 2) | bits(w, 30, 29)
        imm = sext(imm, 21)
        if bit(w, 31):
            return Insn(a, w, "adrp", dict(rd=rd, target=(a & ~0xFFF) + (imm << 12)))
        return Insn(a, w, "adr", dict(rd=rd, target=a + imm))
    if k == 0b010:
        s = bit(w, 29)
        imm = bits(w, 21, 10) << (12 if bit(w, 22) else 0)
        return Insn(a, w, "addsub_imm", dict(
            sf=sf, sub=bit(w, 30), s=s, rd=rd if (s or rd != 31) else SP,
            rn=SP if rn == 31 else rn, imm=imm))
    if k == 0b100:
        opc = bits(w, 30, 29)
        n = bit(w, 22)
        if not sf and n:
            return Insn(a, w, "unk")
        imm = decode_bitmask(n, bits(w, 15, 10), bits(w, 21, 16), 64 if sf else 32)
        if imm is None:
            return Insn(a, w, "unk")
        if opc != 3 and rd == 31:
            rd = SP
        return Insn(a, w, "logic_imm", dict(sf=sf, opc=["and", "orr", "eor", "ands"][opc],
                                            rd=rd, rn=rn, imm=imm))
    if k == 0b101:
        opc = bits(w, 30, 29)
        if opc == 1:
            return Insn(a, w, "unk")
        hw = bits(w, 22, 21)
        return Insn(a, w, ["movn", "", "movz", "movk"][opc],
                    dict(sf=sf, rd=rd, imm=bits(w, 20, 5), shift=hw * 16))
    if k == 0b110:
        opc = bits(w, 30, 29)
        if opc == 3:
            return Insn(a, w, "unk")
        return Insn(a, w, "bfm", dict(sf=sf, kind=["sbfm", "bfm", "ubfm"][opc], rd=rd, rn=rn,
                                      immr=bits(w, 21, 16), imms=bits(w, 15, 10)))
    if k == 0b111:
        return Insn(a, w, "extr", dict(sf=sf, rd=rd, rn=rn, rm=bits(w, 20, 16), lsb=bits(w, 15, 10)))
    return Insn(a, w, "unk")


def _branch(w, a):
    if bits(w, 31, 24) == 0b01010100 and not bit(w, 4):
        return Insn(a, w, "bcond", dict(cond=bits(w, 3, 0), target=a + sext(bits(w, 23, 5), 19) * 4))
    if bits(w, 30, 26) == 0b00101:
        t = a + sext(bits(w, 25, 0), 26) * 4
        return Insn(a, w, "bl" if bit(w, 31) else "b", dict(target=t))
    if bits(w, 30, 25) == 0b011010:
        return Insn(a, w, "cbnz" if bit(w, 24) else "cbz",
                    dict(sf=bit(w, 31), rt=bits(w, 4, 0), target=a + sext(bits(w, 23, 5), 19) * 4))
    if bits(w, 30, 25) == 0b011011:
        return Insn(a, w, "tbnz" if bit(w, 24) else "tbz",
                    dict(rt=bits(w, 4, 0), bit=(bit(w, 31) << 5) | bits(w, 23, 19),
                         target=a + sext(bits(w, 18, 5), 14) * 4))
    if bits(w, 31, 25) == 0b1101011:
        opc = bits(w, 24, 21)
        if bits(w, 20, 10) == 0b11111000000 and bits(w, 4, 0) == 0 and opc in (0, 1, 2):
            return Insn(a, w, ["br", "blr", "ret"][opc], dict(rn=bits(w, 9, 5)))
        return Insn(a, w, "unk")
    if bits(w, 31, 24) == 0b11010100:
        opc, ll = bits(w, 23, 21), bits(w, 1, 0)
        if opc == 1 and ll == 0:
            return Insn(a, w, "brk", dict(imm=bits(w, 20, 5)))
        return Insn(a, w, "unk")
    if bits(w, 31, 22) == 0b1101010100:
        l, op0 = bit(w, 21), bits(w, 20, 19)
        crn, crm, op2, rt = bits(w, 15, 12), bits(w, 11, 8), bits(w, 7, 5), bits(w, 4, 0)
        op1 = bits(w, 18, 16)
        if not l and op0 == 0 and op1 == 3 and crn == 2 and rt == 31:
            return Insn(a, w, "hint", dict(imm=(crm << 3) | op2))
        if not l and op0 == 0 and op1 == 3 and crn == 3 and rt == 31:
            return Insn(a, w, "barrier", dict(kind=op2, crm=crm))
        if op0 >= 2:
            sysreg = (op0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2
            return Insn(a, w, "mrs" if l else "msr", dict(rt=rt, sysreg=sysreg))
        return Insn(a, w, "unk")
    return Insn(a, w, "unk")


def _dp_reg(w, a):
    sf = bit(w, 31)
    rd, rn, rm = bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16)
    op1 = bit(w, 28)
    op2 = bits(w, 24, 21)
    if not op1:
        if not op2 & 0b1000:
            opc = bits(w, 30, 29)
            n = bit(w, 21)
            name = [["and", "bic"], ["orr", "orn"], ["eor", "eon"], ["ands", "bics"]][opc][n]
            return Insn(a, w, "logic_reg", dict(sf=sf, opc=name, rd=rd, rn=rn, rm=rm,
                                                shtype=bits(w, 23, 22), amount=bits(w, 15, 10)))
        if not op2 & 1:
            return Insn(a, w, "addsub_reg", dict(sf=sf, sub=bit(w, 30), s=bit(w, 29), rd=rd, rn=rn, rm=rm,
                                                 shtype=bits(w, 23, 22), amount=bits(w, 15, 10)))
        if bits(w, 23, 22) != 0:
            return Insn(a, w, "unk")
        s = bit(w, 29)
        return Insn(a, w, "addsub_ext", dict(sf=sf, sub=bit(w, 30), s=s,
                                             rd=rd if (s or rd != 31) else SP,
                                             rn=SP if rn == 31 else rn, rm=rm,
                                             option=bits(w, 15, 13), amount=bits(w, 12, 10)))
    if op2 == 0b0000 and bits(w, 15, 10) == 0:
        return Insn(a, w, "adc", dict(sf=sf, sub=bit(w, 30), s=bit(w, 29), rd=rd, rn=rn, rm=rm))
    if op2 == 0b0010 and bit(w, 29) and not bit(w, 10) and not bit(w, 4):
        return Insn(a, w, "ccmp", dict(sf=sf, cmn=not bit(w, 30), is_imm=bit(w, 11), rn=rn,
                                       rm=rm, imm=rm, cond=bits(w, 15, 12), nzcv=bits(w, 3, 0)))
    if op2 == 0b0100 and not bit(w, 29):
        k = (bit(w, 30) << 1) | bit(w, 10)
        if bit(w, 11):
            return Insn(a, w, "unk")
        return Insn(a, w, "csel", dict(sf=sf, kind=["csel", "csinc", "csinv", "csneg"][k],
                                       rd=rd, rn=rn, rm=rm, cond=bits(w, 15, 12)))
    if op2 == 0b0110 and not bit(w, 29):
        opc = bits(w, 15, 10)
        if not bit(w, 30):
            names = {0b000010: "udiv", 0b000011: "sdiv", 0b001000: "lslv",
                     0b001001: "lsrv", 0b001010: "asrv", 0b001011: "rorv"}
            if opc in names:
                return Insn(a, w, "dp2", dict(sf=sf, kind=names[opc], rd=rd, rn=rn, rm=rm))
            return Insn(a, w, "unk")
        if rm == 0:
            names = {0: "rbit", 1: "rev16", 2: "rev32" if sf else "rev", 3: "rev",
                     4: "clz", 5: "cls"}
            if opc in names and not (opc == 3 and not sf):
                return Insn(a, w, "dp1", dict(sf=sf, kind=names[opc], rd=rd, rn=rn))
        return Insn(a, w, "unk")
    if op2 & 0b1000:
        op31, o0 = bits(w, 23, 21), bit(w, 15)
        names = {(0, 0): "madd", (0, 1): "msub", (1, 0): "smaddl", (1, 1): "smsubl",
                 (2, 0): "smulh", (5, 0): "umaddl", (5, 1): "umsubl", (6, 0): "umulh"}
        if (op31, o0) in names and bits(w, 30, 29) == 0:
            return Insn(a, w, "dp3", dict(sf=sf, kind=names[(op31, o0)], rd=rd, rn=rn, rm=rm,
                                          ra=bits(w, 14, 10)))
    return Insn(a, w, "unk")


def _ldst(w, a):
    op0 = bits(w, 31, 28)
    v = bit(w, 26)
    op2 = bits(w, 24, 23)
    rt, rn = bits(w, 4, 0), bits(w, 9, 5)
    rn_sp = SP if rn == 31 else rn

    # Advanced SIMD structures
    if op0 & 0b1011 == 0 and v:
        from simd import decode_simd_ldst
        return decode_simd_ldst(w, a)

    low2 = op0 & 0b11
    if low2 == 0b00 and not v and op2 in (0, 1):
        return _exclusive(w, a)

    if low2 == 0b01 and op2 in (0, 1):
        # load register (literal)
        opc = bits(w, 31, 30)
        target = a + sext(bits(w, 23, 5), 19) * 4
        if v:
            size = [4, 8, 16, 0][opc]
            if not size:
                return Insn(a, w, "unk")
            return Insn(a, w, "ldr_lit", dict(rt=rt, size=size, fp=True, signed=False, target=target))
        if opc == 3:
            return Insn(a, w, "prfm", {})
        return Insn(a, w, "ldr_lit", dict(rt=rt, size=[4, 8, 4][opc], fp=False, signed=opc == 2,
                                          target=target))

    if low2 == 0b10:
        opc = bits(w, 31, 30)
        load = bit(w, 22)
        imm7 = sext(bits(w, 21, 15), 7)
        rt2 = bits(w, 14, 10)
        if v:
            size = [4, 8, 16, 0][opc]
            signed = False
        else:
            if opc == 3 or (opc == 1 and not load):
                return Insn(a, w, "unk")
            size = 4 if opc in (0, 1) else 8
            signed = opc == 1
        mode = ["off", "post", "off", "pre"][op2]   # op2 0 = non-temporal pair
        return Insn(a, w, "ldstp", dict(load=load, fp=bool(v), size=size, signed=signed,
                                        rt=rt, rt2=rt2, rn=rn_sp, mode=mode, imm=imm7 * size))

    if low2 == 0b11:
        if not v and not op2 & 0b10 and bit(w, 21) and bits(w, 11, 10) == 0:
            return _atomic(w, a)
        size = bits(w, 31, 30)
        opc = bits(w, 23, 22)
        if v:
            nbytes = 16 if (opc & 2 and size == 0) else (1 << size)
            if opc & 2 and size != 0:
                return Insn(a, w, "unk")
            load = bool(opc & 1)
            signed = 0
        else:
            nbytes = 1 << size
            if opc == 0:
                load, signed = False, 0
            elif opc == 1:
                load, signed = True, 0
            elif opc == 2:
                if size == 3:
                    return Insn(a, w, "prfm", {})
                load, signed = True, 64
            else:
                if size >= 2:
                    return Insn(a, w, "unk")
                load, signed = True, 32
        base = dict(load=load, fp=bool(v), size=nbytes, signed=signed, rt=rt, rn=rn_sp)

        if op2 & 0b10:
            base.update(mode="off", imm=bits(w, 21, 10) * nbytes)
            if not v and opc == 2 and size == 3:
                return Insn(a, w, "prfm", {})
            return Insn(a, w, "ldst", base)
        if not bit(w, 21):
            imm9 = sext(bits(w, 20, 12), 9)
            k = bits(w, 11, 10)
            if k == 2:
                return Insn(a, w, "unk")       # unprivileged
            base.update(mode=["off", "post", "off", "pre"][k], imm=imm9)
            return Insn(a, w, "ldst", base)
        k = bits(w, 11, 10)
        if k == 2:
            option = bits(w, 15, 13)
            s = bit(w, 12)
            shift = 0
            if s:
                shift = 4 if nbytes == 16 else size
            base.update(mode="reg", rm=bits(w, 20, 16), option=option, shift=shift)
            return Insn(a, w, "ldst", base)
    return Insn(a, w, "unk")


def _exclusive(w, a):
    size = bits(w, 31, 30)
    o2, l, o1, o0 = bit(w, 23), bit(w, 22), bit(w, 21), bit(w, 15)
    rs, rt2, rn, rt = bits(w, 20, 16), bits(w, 14, 10), bits(w, 9, 5), bits(w, 4, 0)
    rn = SP if rn == 31 else rn
    nbytes = 1 << size
    if o2 and o1:
        return Insn(a, w, "cas", dict(size=nbytes, rs=rs, rt=rt, rn=rn, acq=l, rel=o0))
    if o2 and not o1:
        return Insn(a, w, "ldar" if l else "stlr", dict(size=nbytes, rt=rt, rn=rn))
    if not o2 and not o1:
        if l:
            return Insn(a, w, "ldxr", dict(size=nbytes, rt=rt, rn=rn, acq=o0))
        return Insn(a, w, "stxr", dict(size=nbytes, rs=rs, rt=rt, rn=rn, rel=o0))
    return Insn(a, w, "unk")


def _atomic(w, a):
    size = bits(w, 31, 30)
    o3, opc = bit(w, 15), bits(w, 14, 12)
    rs, rn, rt = bits(w, 20, 16), bits(w, 9, 5), bits(w, 4, 0)
    rn = SP if rn == 31 else rn
    nbytes = 1 << size
    if not o3:
        kind = ["add", "clr", "eor", "set", "smax", "smin", "umax", "umin"][opc]
    elif opc == 0:
        kind = "swp"
    elif opc == 4:
        return Insn(a, w, "ldar", dict(size=nbytes, rt=rt, rn=rn))   # ldapr: same for us
    else:
        return Insn(a, w, "unk")
    return Insn(a, w, "ldop", dict(kind=kind, size=nbytes, rs=rs, rt=rt, rn=rn))


def _fp_scalar(w, a):
    rd, rn, rm = bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16)
    ftype = bits(w, 23, 22)
    fsize = {0: 32, 1: 64, 3: 16}.get(ftype)
    if bits(w, 28, 24) == 0b11111:
        if fsize is None:
            return None
        o1, o0 = bit(w, 21), bit(w, 15)
        kind = ["fmadd", "fmsub", "fnmadd", "fnmsub"][(o1 << 1) | o0]
        return Insn(a, w, "fp3", dict(kind=kind, ft=fsize, rd=rd, rn=rn, rm=rm, ra=bits(w, 14, 10)))
    if bits(w, 28, 24) != 0b11110:
        return None
    if not bit(w, 21):
        # fixed-point <-> fp
        sf = bit(w, 31)
        rmode, opcode = bits(w, 20, 19), bits(w, 18, 16)
        fbits = 64 - bits(w, 15, 10)
        if fsize is None:
            return None
        if rmode == 0 and opcode in (2, 3):
            return Insn(a, w, "cvtf", dict(sf=sf, ft=fsize, signed=opcode == 2, rd=rd, rn=rn, fbits=fbits))
        if rmode == 3 and opcode in (0, 1):
            return Insn(a, w, "fcvti", dict(sf=sf, ft=fsize, signed=opcode == 0, rmode="z",
                                            rd=rd, rn=rn, fbits=fbits))
        return None
    k = bits(w, 11, 10)
    if k == 0:
        if bits(w, 15, 10) == 0:
            sf = bit(w, 31)
            rmode, opcode = bits(w, 20, 19), bits(w, 18, 16)
            if opcode in (0, 1):
                if fsize is None:
                    return None
                return Insn(a, w, "fcvti", dict(sf=sf, ft=fsize, signed=opcode == 0,
                                                rmode="npmz"[rmode], rd=rd, rn=rn, fbits=0))
            if opcode in (4, 5) and rmode == 0:
                if fsize is None:
                    return None
                return Insn(a, w, "fcvti", dict(sf=sf, ft=fsize, signed=opcode == 4, rmode="a",
                                                rd=rd, rn=rn, fbits=0))
            if opcode in (2, 3) and rmode == 0:
                if fsize is None:
                    return None
                return Insn(a, w, "cvtf", dict(sf=sf, ft=fsize, signed=opcode == 2, rd=rd, rn=rn, fbits=0))
            if opcode in (6, 7):
                # fmov between general and fp registers
                if rmode == 0:
                    if (sf, ftype) not in ((0, 0), (1, 1), (0, 3), (1, 3)):
                        return None
                    return Insn(a, w, "fmov_gen", dict(to_fp=opcode == 7, sf=sf, ft=fsize, top=0,
                                                       rd=rd, rn=rn))
                if rmode == 1 and sf and ftype == 2:
                    return Insn(a, w, "fmov_gen", dict(to_fp=opcode == 7, sf=1, ft=64, top=1, rd=rd, rn=rn))
            return None
        if bits(w, 14, 10) == 0b10000:
            if fsize is None:
                return None
            opcode = bits(w, 20, 15)
            names = {0: "fmov", 1: "fabs", 2: "fneg", 3: "fsqrt",
                     8: "frintn", 9: "frintp", 10: "frintm", 11: "frintz",
                     12: "frinta", 14: "frintx", 15: "frinti"}
            if opcode in names:
                return Insn(a, w, "fp1", dict(kind=names[opcode], ft=fsize, rd=rd, rn=rn))
            if opcode in (4, 5, 7):
                to = {4: 32, 5: 64, 7: 16}[opcode]
                return Insn(a, w, "fcvt", dict(ft=fsize, to=to, rd=rd, rn=rn))
            return None
        if bits(w, 13, 10) == 0b1000:
            if fsize is None:
                return None
            opc2 = bits(w, 4, 0)
            return Insn(a, w, "fcmp", dict(ft=fsize, rn=rn, rm=rm, zero=bool(opc2 & 0b01000)))
        if bits(w, 12, 10) == 0b100:
            if fsize is None:
                return None
            return Insn(a, w, "fmov_imm", dict(ft=fsize, rd=rd,
                                               value=vfp_expand_imm(bits(w, 20, 13), 32 if fsize == 32 else 64)))
        return None
    if fsize is None:
        return None
    if k == 1:
        return Insn(a, w, "fccmp", dict(ft=fsize, rn=rn, rm=rm, cond=bits(w, 15, 12), nzcv=bits(w, 3, 0)))
    if k == 2:
        opcode = bits(w, 15, 12)
        names = ["fmul", "fdiv", "fadd", "fsub", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul"]
        if opcode < len(names):
            return Insn(a, w, "fp2", dict(kind=names[opcode], ft=fsize, rd=rd, rn=rn, rm=rm))
        return None
    return Insn(a, w, "fcsel", dict(ft=fsize, rd=rd, rn=rn, rm=rm, cond=bits(w, 15, 12)))
