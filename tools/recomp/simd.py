"""Advanced SIMD (NEON) part of the AArch64 decoder.

Every vector instruction decodes to Insn("vec", name=..., ...) with:
  name   canonical mnemonic (with a "2" suffix folded into `part` instead)
  esize  element size in bits (of the destination unless stated)
  q      1 for a 128-bit operation, 0 for 64-bit
  scalar 1 for the scalar forms (one element, rest of the register zeroed)
  rd/rn/rm, idx (element index), imm, shift, part (0 lower / 1 upper half)
The generator lowers each name to a helper in runtime/include/simd.h.
"""

from a64 import Insn, SP, bit, bits

_THREE_SAME_INT = {
    0b00000: ("shadd", "uhadd"), 0b00001: ("sqadd", "uqadd"), 0b00010: ("srhadd", "urhadd"),
    0b00100: ("shsub", "uhsub"), 0b00101: ("sqsub", "uqsub"), 0b00110: ("cmgt", "cmhi"),
    0b00111: ("cmge", "cmhs"), 0b01000: ("sshl", "ushl"), 0b01001: ("sqshl", "uqshl"),
    0b01010: ("srshl", "urshl"), 0b01100: ("smax", "umax"), 0b01101: ("smin", "umin"),
    0b01110: ("sabd", "uabd"), 0b01111: ("saba", "uaba"), 0b10000: ("add", "sub"),
    0b10001: ("cmtst", "cmeq"), 0b10010: ("mla", "mls"), 0b10011: ("mul", "pmul"),
    0b10100: ("smaxp", "umaxp"), 0b10101: ("sminp", "uminp"), 0b10110: ("sqdmulh", "sqrdmulh"),
    0b10111: ("addp", None),
}

_THREE_SAME_FP = {
    # (U, a, opcode)
    (0, 0, 0b11000): "fmaxnm", (0, 0, 0b11001): "fmla", (0, 0, 0b11010): "fadd",
    (0, 0, 0b11011): "fmulx", (0, 0, 0b11100): "fcmeq", (0, 0, 0b11110): "fmax",
    (0, 0, 0b11111): "frecps",
    (0, 1, 0b11000): "fminnm", (0, 1, 0b11001): "fmls", (0, 1, 0b11010): "fsub",
    (0, 1, 0b11110): "fmin", (0, 1, 0b11111): "frsqrts",
    (1, 0, 0b11000): "fmaxnmp", (1, 0, 0b11010): "faddp", (1, 0, 0b11011): "fmul",
    (1, 0, 0b11100): "fcmge", (1, 0, 0b11101): "facge", (1, 0, 0b11110): "fmaxp",
    (1, 0, 0b11111): "fdiv",
    (1, 1, 0b11000): "fminnmp", (1, 1, 0b11010): "fabd", (1, 1, 0b11100): "fcmgt",
    (1, 1, 0b11101): "facgt", (1, 1, 0b11110): "fminp",
}

_TWO_MISC_INT = {
    (0, 0b00000): "rev64", (0, 0b00001): "rev16", (0, 0b00010): "saddlp", (0, 0b00100): "cls",
    (0, 0b00101): "cnt", (0, 0b00110): "sadalp", (0, 0b00111): "sqabs", (0, 0b01000): "cmgtz",
    (0, 0b01001): "cmeqz", (0, 0b01010): "cmltz", (0, 0b01011): "abs", (0, 0b10010): "xtn",
    (0, 0b10100): "sqxtn",
    (1, 0b00000): "rev32", (1, 0b00010): "uaddlp", (1, 0b00100): "clz", (1, 0b00110): "uadalp",
    (1, 0b00111): "sqneg", (1, 0b01000): "cmgez", (1, 0b01001): "cmlez", (1, 0b01011): "neg",
    (1, 0b10010): "sqxtun", (1, 0b10011): "shll", (1, 0b10100): "uqxtn",
}

_TWO_MISC_FP = {
    # (U, a, opcode)
    (0, 0, 0b10110): "fcvtn", (0, 0, 0b10111): "fcvtl", (0, 0, 0b11000): "frintn",
    (0, 0, 0b11001): "frintm", (0, 0, 0b11010): "fcvtns", (0, 0, 0b11011): "fcvtms",
    (0, 0, 0b11100): "fcvtas", (0, 0, 0b11101): "scvtf",
    (0, 1, 0b01100): "fcmgtz", (0, 1, 0b01101): "fcmeqz", (0, 1, 0b01110): "fcmltz",
    (0, 1, 0b01111): "fabs", (0, 1, 0b11000): "frintp", (0, 1, 0b11001): "frintz",
    (0, 1, 0b11010): "fcvtps", (0, 1, 0b11011): "fcvtzs", (0, 1, 0b11101): "frecpe",
    (1, 0, 0b10110): "fcvtxn", (1, 0, 0b11000): "frinta", (1, 0, 0b11001): "frintx",
    (1, 0, 0b11010): "fcvtnu", (1, 0, 0b11011): "fcvtmu", (1, 0, 0b11100): "fcvtau",
    (1, 0, 0b11101): "ucvtf",
    (1, 1, 0b01100): "fcmgez", (1, 1, 0b01101): "fcmlez", (1, 1, 0b01111): "fneg",
    (1, 1, 0b11001): "frinti", (1, 1, 0b11010): "fcvtpu", (1, 1, 0b11011): "fcvtzu",
    (1, 1, 0b11101): "frsqrte", (1, 1, 0b11111): "fsqrt",
}

_ACROSS = {
    (0, 0b00011): "saddlv", (0, 0b01010): "smaxv", (0, 0b11010): "sminv", (0, 0b11011): "addv",
    (1, 0b00011): "uaddlv", (1, 0b01010): "umaxv", (1, 0b11010): "uminv",
}

_THREE_DIFF = {
    0b0000: ("saddl", "uaddl"), 0b0001: ("saddw", "uaddw"), 0b0010: ("ssubl", "usubl"),
    0b0011: ("ssubw", "usubw"), 0b0100: ("addhn", "raddhn"), 0b0101: ("sabal", "uabal"),
    0b0110: ("subhn", "rsubhn"), 0b0111: ("sabdl", "uabdl"), 0b1000: ("smlal", "umlal"),
    0b1010: ("smlsl", "umlsl"), 0b1100: ("smull", "umull"), 0b1110: ("pmull", None),
}

_SHIFT_IMM = {
    (0, 0b00000): "sshr", (0, 0b00010): "ssra", (0, 0b00100): "srshr", (0, 0b00110): "srsra",
    (0, 0b01010): "shl", (0, 0b01110): "sqshl", (0, 0b10000): "shrn", (0, 0b10001): "rshrn",
    (0, 0b10010): "sqshrn", (0, 0b10011): "sqrshrn", (0, 0b10100): "sshll",
    (0, 0b11100): "scvtf_fx", (0, 0b11111): "fcvtzs_fx",
    (1, 0b00000): "ushr", (1, 0b00010): "usra", (1, 0b00100): "urshr", (1, 0b00110): "ursra",
    (1, 0b01000): "sri", (1, 0b01010): "sli", (1, 0b01100): "sqshlu", (1, 0b01110): "uqshl",
    (1, 0b10000): "sqshrun", (1, 0b10001): "sqrshrun", (1, 0b10010): "uqshrn",
    (1, 0b10100): "ushll", (1, 0b11100): "ucvtf_fx", (1, 0b11111): "fcvtzu_fx",
}
_RIGHT_SHIFTS = {"sshr", "ssra", "srshr", "srsra", "shrn", "rshrn", "sqshrn", "sqrshrn",
                 "scvtf_fx", "fcvtzs_fx", "ushr", "usra", "urshr", "ursra", "sri", "sqshrun",
                 "sqrshrun", "uqshrn", "ucvtf_fx", "fcvtzu_fx"}
_NARROWING = {"shrn", "rshrn", "sqshrn", "sqrshrn", "sqshrun", "sqrshrun", "uqshrn"}

_BY_ELEM_INT = {
    (0, 0b0010): "smlal", (0, 0b0110): "smlsl", (0, 0b1000): "mul", (0, 0b1010): "smull",
    (0, 0b1100): "sqdmulh", (0, 0b1101): "sqrdmulh",
    (1, 0b0000): "mla", (1, 0b0100): "mls", (1, 0b0010): "umlal", (1, 0b0110): "umlsl",
    (1, 0b1010): "umull",
}
_BY_ELEM_FP = {(0, 0b0001): "fmla", (0, 0b0101): "fmls", (0, 0b1001): "fmul", (1, 0b1001): "fmulx"}


def _vec(a, w, name, **kw):
    kw.setdefault("scalar", 0)
    return Insn(a, w, "vec", dict(name=name, **kw))


def _expand_imm(op, cmode, imm8):
    """AdvSIMDExpandImm(): the 64-bit pattern (None for the fp forms)."""
    c = cmode >> 1
    if c in (0, 1, 2, 3):
        v = imm8 << (8 * c)
        return v | (v << 32)
    if c in (4, 5):
        v = imm8 << (8 * (c & 1))
        return v | (v << 16) | (v << 32) | (v << 48)
    if c == 6:
        v = ((imm8 << 8) | 0xFF) if not cmode & 1 else ((imm8 << 16) | 0xFFFF)
        return v | (v << 32)
    if cmode == 0b1110:
        if not op:
            return int.from_bytes(bytes([imm8]) * 8, "little")
        out = 0
        for k in range(8):
            if imm8 & (1 << k):
                out |= 0xFF << (8 * k)
        return out
    return None


def _fp_imm_bits(imm8, size):
    b6 = (imm8 >> 6) & 1
    a = imm8 >> 7
    if size == 32:
        return (a << 31) | ((b6 ^ 1) << 30) | ((0x1F if b6 else 0) << 25) | ((imm8 & 0x3F) << 19)
    return (a << 63) | ((b6 ^ 1) << 62) | ((0xFF if b6 else 0) << 54) | ((imm8 & 0x3F) << 48)


def decode_simd(w, a):
    rd, rn, rm = bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16)
    q = bit(w, 30)
    u = bit(w, 29)
    size = bits(w, 23, 22)

    if bit(w, 31):
        return Insn(a, w, "unk")
    scalar = bits(w, 28, 24) in (0b11110, 0b11111) and q
    vector = bits(w, 28, 24) in (0b01110, 0b01111)
    if not (scalar or vector):
        return Insn(a, w, "unk")
    if scalar:
        q = 0
    low = bits(w, 28, 24) & 1        # 0: x1110 groups, 1: x1111 groups

    if not low:
        # copy
        if bits(w, 23, 21) == 0 and not bit(w, 15) and bit(w, 10):
            imm5, imm4 = bits(w, 20, 16), bits(w, 14, 11)
            lsb = (imm5 & -imm5).bit_length() - 1
            if lsb > 3:
                return Insn(a, w, "unk")
            esize = 8 << lsb
            idx = imm5 >> (lsb + 1)
            if scalar:
                if u or imm4:
                    return Insn(a, w, "unk")
                return _vec(a, w, "dup_elem", esize=esize, q=0, rd=rd, rn=rn, idx=idx, scalar=1)
            if u:
                return _vec(a, w, "ins_elem", esize=esize, q=1, rd=rd, rn=rn, idx=idx,
                            idx2=imm4 >> lsb)
            if imm4 == 0:
                return _vec(a, w, "dup_elem", esize=esize, q=q, rd=rd, rn=rn, idx=idx)
            if imm4 == 1:
                return _vec(a, w, "dup_gen", esize=esize, q=q, rd=rd, rn=rn)
            if imm4 == 3:
                return _vec(a, w, "ins_gen", esize=esize, q=1, rd=rd, rn=rn, idx=idx)
            if imm4 == 5:
                return _vec(a, w, "smov", esize=esize, q=q, rd=rd, rn=rn, idx=idx)
            if imm4 == 7:
                return _vec(a, w, "umov", esize=esize, q=q, rd=rd, rn=rn, idx=idx)
            return Insn(a, w, "unk")

        # permute / extract / table (vector only, bit 21 = 0)
        if vector and not bit(w, 21) and not bit(w, 15):
            if not u and bits(w, 11, 10) == 0b10:
                opc = bits(w, 14, 12)
                names = {1: "uzp1", 2: "trn1", 3: "zip1", 5: "uzp2", 6: "trn2", 7: "zip2"}
                if opc in names:
                    return _vec(a, w, names[opc], esize=8 << size, q=q, rd=rd, rn=rn, rm=rm)
            if u and size == 0 and not bit(w, 10):
                return _vec(a, w, "ext", esize=8, q=q, rd=rd, rn=rn, rm=rm, imm=bits(w, 14, 11))
            if not u and size == 0 and bits(w, 11, 10) == 0:
                return _vec(a, w, "tbx" if bit(w, 12) else "tbl", esize=8, q=q, rd=rd, rn=rn, rm=rm,
                            imm=bits(w, 14, 13) + 1)
            return Insn(a, w, "unk")

        if bit(w, 21):
            k = bits(w, 11, 10)
            # three same
            if bit(w, 10):
                opcode = bits(w, 15, 11)
                if opcode >= 0b11000:
                    name = _THREE_SAME_FP.get((u, size >> 1, opcode))
                    if not name:
                        return Insn(a, w, "unk")
                    return _vec(a, w, name, esize=32 << (size & 1), q=q, rd=rd, rn=rn, rm=rm,
                                scalar=int(scalar))
                if opcode == 0b00011:
                    name = ["and", "bic", "orr", "orn", "eor", "bsl", "bit", "bif"][u * 4 + size]
                    return _vec(a, w, name, esize=8, q=q, rd=rd, rn=rn, rm=rm)
                pair = _THREE_SAME_INT.get(opcode)
                name = pair and pair[u]
                if not name:
                    return Insn(a, w, "unk")
                return _vec(a, w, name, esize=8 << size, q=q, rd=rd, rn=rn, rm=rm, scalar=int(scalar))
            # two-reg misc / across lanes
            if k == 0b10 and bits(w, 20, 17) in (0b0000, 0b1000):
                opcode = bits(w, 16, 12)
                if bits(w, 20, 17) == 0b1000:
                    if scalar:
                        # scalar pairwise
                        if u and opcode in (0b01100, 0b01101, 0b01111) or u and opcode == 0b01100:
                            pass
                        if opcode == 0b11011 and not u and size == 3:
                            return _vec(a, w, "addp_scalar", esize=64, q=1, rd=rd, rn=rn, scalar=1)
                        fp_names = {0b01100: "fmaxnmp", 0b01101: "faddp", 0b01111: "fmaxp"}
                        if u and opcode in fp_names:
                            name = fp_names[opcode]
                            if size >> 1:
                                name = {"fmaxnmp": "fminnmp", "fmaxp": "fminp"}.get(name)
                                if not name:
                                    return Insn(a, w, "unk")
                            return _vec(a, w, name + "_scalar", esize=32 << (size & 1), q=1,
                                        rd=rd, rn=rn, scalar=1)
                        return Insn(a, w, "unk")
                    if opcode in (0b01100, 0b01111) and u:
                        base = "fmaxnmv" if opcode == 0b01100 else "fmaxv"
                        if size >> 1:
                            base = base.replace("max", "min")
                        return _vec(a, w, base, esize=32, q=q, rd=rd, rn=rn)
                    name = _ACROSS.get((u, opcode))
                    if not name:
                        return Insn(a, w, "unk")
                    return _vec(a, w, name, esize=8 << size, q=q, rd=rd, rn=rn)
                if opcode >= 0b01100 and (opcode >= 0b10110 or opcode in (0b01100, 0b01101, 0b01110, 0b01111)):
                    name = _TWO_MISC_FP.get((u, size >> 1, opcode))
                    if name:
                        es = 32 << (size & 1)
                        if name in ("fcvtn", "fcvtxn", "fcvtl"):
                            es = 16 << (size & 1)    # narrow element size
                        return _vec(a, w, name, esize=es, q=q, rd=rd, rn=rn, scalar=int(scalar))
                if u and opcode == 0b00101:
                    name = "not" if size == 0 else ("rbit" if size == 1 else None)
                    if not name:
                        return Insn(a, w, "unk")
                    return _vec(a, w, name, esize=8, q=q, rd=rd, rn=rn)
                name = _TWO_MISC_INT.get((u, opcode))
                if not name:
                    return Insn(a, w, "unk")
                return _vec(a, w, name, esize=8 << size, q=q, rd=rd, rn=rn, scalar=int(scalar), part=q)
            # three different
            if k == 0b00:
                pair = _THREE_DIFF.get(bits(w, 15, 12))
                name = pair and pair[u]
                if not name or scalar:
                    return Insn(a, w, "unk")
                return _vec(a, w, name, esize=8 << size, q=q, rd=rd, rn=rn, rm=rm, part=q)
        return Insn(a, w, "unk")

    # x1111 groups: modified immediate, shift by immediate, by element
    if bit(w, 10):
        immh = bits(w, 22, 19)
        if vector and immh == 0 and bits(w, 23, 19) == 0:
            cmode, o2 = bits(w, 15, 12), bit(w, 11)
            imm8 = (bits(w, 18, 16) << 5) | bits(w, 9, 5)
            op = u
            if o2:
                return Insn(a, w, "unk")
            if cmode == 0b1111:
                if not op:
                    v = _fp_imm_bits(imm8, 32)
                    return _vec(a, w, "movi", esize=64, q=q, rd=rd, imm=v | (v << 32))
                if not q:
                    return Insn(a, w, "unk")
                return _vec(a, w, "movi", esize=64, q=q, rd=rd, imm=_fp_imm_bits(imm8, 64))
            imm = _expand_imm(op, cmode, imm8)
            if cmode < 8 and cmode & 1 or 8 <= cmode < 12 and cmode & 1:
                return _vec(a, w, "bic_imm" if op else "orr_imm", esize=64, q=q, rd=rd, imm=imm)
            if op and cmode != 0b1110:
                return _vec(a, w, "movi", esize=64, q=q, rd=rd, imm=~imm & 0xFFFFFFFFFFFFFFFF)
            return _vec(a, w, "movi", esize=64, q=q, rd=rd, imm=imm)
        if immh == 0 or bit(w, 23):
            return Insn(a, w, "unk")
        name = _SHIFT_IMM.get((u, bits(w, 15, 11)))
        if not name:
            return Insn(a, w, "unk")
        hsb = immh.bit_length() - 1
        esize = 8 << hsb
        immhb = bits(w, 22, 16)
        shift = (2 * esize - immhb) if name in _RIGHT_SHIFTS else (immhb - esize)
        if scalar and name in _NARROWING | {"sshll", "ushll"}:
            return Insn(a, w, "unk")
        return _vec(a, w, name, esize=esize, q=q, rd=rd, rn=rn, shift=shift, part=q,
                    scalar=int(scalar))

    # by element
    opcode = bits(w, 15, 12)
    l, m, h = bit(w, 21), bit(w, 20), bit(w, 11)
    fpname = _BY_ELEM_FP.get((u, opcode))
    if fpname:
        sz = size & 1
        if not size >> 1:
            return Insn(a, w, "unk")
        if sz:
            if l:
                return Insn(a, w, "unk")
            idx = h
        else:
            idx = (h << 1) | l
        return _vec(a, w, fpname + "_elem", esize=32 << sz, q=q, rd=rd, rn=rn,
                    rm=(m << 4) | bits(w, 19, 16), idx=idx, scalar=int(scalar))
    name = _BY_ELEM_INT.get((u, opcode))
    if not name or scalar:
        return Insn(a, w, "unk")
    if size == 1:
        idx, vm = (h << 2) | (l << 1) | m, bits(w, 19, 16)
    elif size == 2:
        idx, vm = (h << 1) | l, (m << 4) | bits(w, 19, 16)
    else:
        return Insn(a, w, "unk")
    return _vec(a, w, name + "_elem", esize=8 << size, q=q, rd=rd, rn=rn, rm=vm, idx=idx, part=q)


def decode_simd_ldst(w, a):
    q = bit(w, 30)
    load = bit(w, 22)
    rn, rt = bits(w, 9, 5), bits(w, 4, 0)
    rn = SP if rn == 31 else rn
    post = bit(w, 23)
    rm = bits(w, 20, 16)
    size = bits(w, 11, 10)
    if not bit(w, 24):
        if bit(w, 21) or (not post and rm):
            return Insn(a, w, "unk")
        opcode = bits(w, 15, 12)
        table = {0b0000: (4, 4), 0b0010: (1, 4), 0b0100: (3, 3), 0b0110: (1, 3),
                 0b0111: (1, 1), 0b1000: (2, 2), 0b1010: (1, 2)}
        if opcode not in table:
            return Insn(a, w, "unk")
        selem, nregs = table[opcode]
        esize = 8 << size
        total = (16 if q else 8) * nregs if selem == 1 else (16 if q else 8) * selem
        return Insn(a, w, "vldst_multi", dict(load=load, selem=selem, nregs=nregs if selem == 1 else selem,
                                              esize=esize, q=q, rt=rt, rn=rn, post=post,
                                              rm=rm, total=total))
    # single structure
    if not post and rm:
        return Insn(a, w, "unk")
    opcode = bits(w, 15, 13)
    r = bit(w, 21)
    s = bit(w, 12)
    selem = (((opcode & 1) << 1) | r) + 1
    if opcode in (6, 7):
        if not load or s:
            return Insn(a, w, "unk")
        esize = 8 << size
        return Insn(a, w, "vld_rep", dict(selem=selem, esize=esize, q=q, rt=rt, rn=rn, post=post, rm=rm,
                                          total=selem * esize // 8))
    scale = opcode >> 1
    if scale == 0:
        idx = (q << 3) | (s << 2) | size
    elif scale == 1:
        if size & 1:
            return Insn(a, w, "unk")
        idx = (q << 2) | (s << 1) | (size >> 1)
    elif scale == 2:
        if size & 2:
            return Insn(a, w, "unk")
        if not size & 1:
            idx = (q << 1) | s
        else:
            if s:
                return Insn(a, w, "unk")
            idx = q
            scale = 3
    else:
        return Insn(a, w, "unk")
    esize = 8 << scale
    return Insn(a, w, "vldst_one", dict(load=load, selem=selem, esize=esize, idx=idx, rt=rt, rn=rn,
                                        post=post, rm=rm, total=selem * esize // 8))
