"""Generate C from the arm64 slice of the game's executable.

    python3 tools/recomp/gen.py <MinaTheHollower> <outdir> [--only ADDR,...]

Writes funcs_NNN.c (one C function per guest function), funcs.h, tables.c
(function list, imports, binds, rebases, TLS template) into <outdir>.

Conventions of the generated code (see docs/PLAN.md and runtime/include/recomp.h):
  * guest registers are C locals; Ctx is their home across calls;
  * AAPCS64: a call stores the argument registers the function wrote
    (x0-x8, v0-v7, sp) and reloads x0/x1/v0-v3 afterwards; callee-saved
    registers simply stay in locals;
  * flags: a setter stores its operands (fa/fb or fda/fdb); a consumer reached
    only by setters of one kind compares them directly, otherwise the setters
    also materialize nzcv.
"""

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "analysis"))
from a64 import CONDS, SP, ZR, decode, decode_bitmask  # noqa: E402
from macho import MachO  # noqa: E402

ORIG_BASE = 0x100000000
IMAGE_BASE = 0x01000000
THUNK_BASE = 0x00010000

FUNCS_PER_FILE = 400

ARG_X = list(range(9))          # x0-x8
ARG_V = list(range(8))          # v0-v7
RET_X = [0, 1]
RET_V = [0, 1, 2, 3]


def rel(a):
    return a - ORIG_BASE + IMAGE_BASE


def cname(a):
    return "f_%x" % (a - ORIG_BASE)


def label(a):
    return "L_%x" % (a - ORIG_BASE)


def hexu(v, sf=1):
    v &= (1 << 64) - 1 if sf else 0xFFFFFFFF
    return ("0x%xull" % v) if sf else ("0x%xu" % v)


def mask(n):
    return (1 << n) - 1


class GenError(Exception):
    pass


# ---------------------------------------------------------------------------
# Program-level information


class Program:
    def __init__(self, path):
        self.m = m = MachO(path)
        self.text = m.section("__TEXT", "__text")
        self.stubs = m.section("__TEXT", "__stubs")
        self.objc_stubs = m.section("__TEXT", "__objc_stubs")

        starts = set(a for a in m.function_starts if self.text.addr <= a < self.text.end)
        # LC_FUNCTION_STARTS misses some functions (static initializers):
        # add named code symbols, static initializers and direct call targets.
        for s in m.symbols:
            if (s.type & 0xE0) == 0 and (s.type & 0x0E) == 0x0E and self.text.addr <= s.value < self.text.end \
                    and s.value % 4 == 0:
                starts.add(s.value)
        mi = m.section("__DATA_CONST", "__mod_init_func")
        if mi:
            for k in range(mi.size // 8):
                a = m.u64(mi.addr + 8 * k)
                if self.text.addr <= a < self.text.end:
                    starts.add(a)
        code = m.read(self.text.addr, self.text.size)
        for off in range(0, len(code), 4):
            w = struct.unpack_from("<I", code, off)[0]
            if (w >> 26) == 0b100101:          # bl
                imm = w & 0x3FFFFFF
                if imm & 0x2000000:
                    imm -= 0x4000000
                t = self.text.addr + off + imm * 4
                if self.text.addr <= t < self.text.end:
                    starts.add(t)
        starts = sorted(starts)
        self.funcs = []
        for i, a in enumerate(starts):
            end = starts[i + 1] if i + 1 < len(starts) else self.text.end
            self.funcs.append((a, end))
        if self.objc_stubs:
            for a in range(self.objc_stubs.addr, self.objc_stubs.end, 32):
                self.funcs.append((a, a + 32))
        self.func_starts = {a for a, _ in self.funcs}
        self.func_end = dict(self.funcs)

        self.names = {}
        for s in m.symbols:
            if (s.type & 0x0E) == 0x0E and s.value in self.func_starts and s.name:
                self.names.setdefault(s.value, s.name)

        # Imports: one index per name, in order of first appearance.
        self.import_index = {}
        self.import_names = []
        stub_map = m.stub_imports()
        for b in m.binds:
            self._imp(b.name)
        for name in stub_map.values():
            self._imp(name)
        self.stub_import = {}
        if self.stubs:
            for a in range(self.stubs.addr, self.stubs.end, self.stubs.reserved2):
                if a in stub_map:
                    self.stub_import[a] = self.import_index[stub_map[a]]

        self._code = m.read(self.text.addr, self.text.size)
        self._objc = m.read(self.objc_stubs.addr, self.objc_stubs.size) if self.objc_stubs else b""

    def _imp(self, name):
        if name not in self.import_index:
            self.import_index[name] = len(self.import_names)
            self.import_names.append(name)

    def word(self, a):
        if self.text.addr <= a < self.text.end:
            return struct.unpack_from("<I", self._code, a - self.text.addr)[0]
        if self.objc_stubs and self.objc_stubs.addr <= a < self.objc_stubs.end:
            return struct.unpack_from("<I", self._objc, a - self.objc_stubs.addr)[0]
        return struct.unpack("<I", self.m.read(a, 4))[0]

    def read(self, a, n):
        return self.m.read(a, n)


# ---------------------------------------------------------------------------
# Per-function analysis and lowering


# Lean-memory patches: instruction immediates changed at recompile time.
# ycAudio::Mixer::s_sources is {0xc00 header, 256 CircBufs of {0x20 header,
# 0x40000 bytes of float}, entries of 0x68 bytes at +0x4002c00}: 64 MiB of
# bss. The code builds the CircBuf stride, the capacity and the loop bounds
# with movz/movk of the high half; the offsets of the entries after the array
# stay as they are. With AUDIO_RING bytes per ring (~0.17 s of stereo float
# at 0x10000) the sources take 16 MiB and the rest of the array is never
# touched: the runtime reuses it (g_spare_lo/hi).
AUDIO_RING = 0x10000
_STRIDE, _OLD_STRIDE = AUDIO_RING + 0x20, 0x40020


def _hi(v):
    return (v >> 16) & 0xffff


PATCH_HI16 = {   # address: (high half in the game, high half here)
    # stride 0x40020
    0x1000cb3bc: (0x4, _hi(_STRIDE)),                      # Decoder::ThreadMain
    0x1000cb9a8: (0x4, _hi(_STRIDE)),                      # Decoder::DecodeCtx
    0x1000d02ec: (0x4, _hi(_STRIDE)),                      # Mixer::$_0::~$_0
    0x1000d0424: (0x4, _hi(_STRIDE)), 0x1000d04c0: (0x4, _hi(_STRIDE)),   # Mixer::ThreadMain
    0x1000d09bc: (0x4, _hi(_STRIDE)), 0x1000d1384: (0x4, _hi(_STRIDE)),   # Mixer::MixTrack
    0x100c786bc: (0x4, _hi(_STRIDE)),                      # static init
    # capacity 0x40000
    0x1000cb3cc: (0x4, _hi(AUDIO_RING)),                   # Decoder::ThreadMain
    0x100c786b4: (0x4, _hi(AUDIO_RING)),                   # static init
    # static init: end of the CircBufs, 0xc00 + 256 * stride
    0x100c786c4: (0x400, _hi(0xc00 + 256 * _STRIDE)),
    # ~$_0 walks back from the last CircBuf (+0x18) by -stride, 256 times
    0x1000d02d0: (0x3fc, _hi(0xc00 + 255 * _STRIDE + 0x18)),
    0x1000d02dc: (0xfbff, _hi(-256 * _STRIDE & 0xffffffff)),
    0x1000d02e4: (0xfffb, _hi(-_STRIDE & 0xffffffff)),
}
assert (0xc00 + 255 * _STRIDE + 0x18) & 0xffff == 0x2bf8
assert -256 * _STRIDE & 0xffff == 0xe000 and -_STRIDE & 0xffff == 0xffe0
_patched = set()


def patch_word(addr, w):
    p = PATCH_HI16.get(addr)
    if p is None:
        return w
    old, new = p
    # movz/movk (32 or 64-bit) with hw = 1: imm16 in bits 5..20
    if (w & 0x7f800000) not in (0x52800000, 0x72800000) or (w >> 21) & 3 != 1 or (w >> 5) & 0xffff != old:
        raise SystemExit("patch at %#x: unexpected instruction %#010x" % (addr, w))
    _patched.add(addr)
    return (w & ~(0xffff << 5)) | (new << 5)


class Func:
    def __init__(self, prog, start, end, words=None):
        self.p = prog
        self.start, self.end = start, end
        if words is None:
            words = [prog.word(a) for a in range(start, end, 4)]
        words = [patch_word(start + 4 * k, w) for k, w in enumerate(words)]
        self.insns = [decode(w, start + 4 * k) for k, w in enumerate(words)]
        self.index = {ins.addr: i for i, ins in enumerate(self.insns)}
        self.jtables = {}       # br addr -> list of targets
        self.labels = set()

    def inside(self, a):
        return self.start <= a < self.end

    # -- control flow -----------------------------------------------------

    def analyze(self):
        for ins in self.insns:
            if ins.op in ("b", "bcond", "cbz", "cbnz", "tbz", "tbnz"):
                if self.inside(ins.target):
                    self.labels.add(ins.target)
            elif ins.op == "br":
                t = self.jump_table(ins)
                if t is not None:
                    self.jtables[ins.addr] = t
                    self.labels.update(t)
        self.build_blocks()
        self.flag_flow()

    def jump_table(self, br):
        """Targets of a compiler jump table ending at `br`, or None."""
        i = self.index[br.addr]
        rb = br.rn
        lo = max(0, i - 16)
        add = adr = load = None
        for k in range(i - 1, lo - 1, -1):
            ins = self.insns[k]
            if add is None:
                if ins.op in ("addsub_reg", "addsub_ext") and not ins.sub and not ins.s and ins.rd == rb:
                    add = ins
                continue
            if adr is None and ins.op == "adr" and ins.rd == add.rn:
                adr = ins
            if load is None and ins.op == "ldst" and ins.load and ins.mode == "reg" and ins.rt == add.rm:
                load = ins
            if adr and load:
                break
        if not (add and adr and load):
            return None
        amount = add.amount
        if amount != 2:
            return None
        # table address: adrp + add into load.rn, before the load
        tab = self.const_before(self.index[load.addr], load.rn)
        if tab is None:
            return None
        n = self.bound_before(self.index[load.addr], load.rm)
        size, signed = load.size, load.signed
        targets = []
        if n is not None and n < 0:          # upper limit: read until a target leaves the function
            limit, n = -n, None
        else:
            limit = n if n is not None else 4096
        for e in range(limit):
            raw = self.p.read(tab + e * size, size)
            v = int.from_bytes(raw, "little", signed=bool(signed))
            if add.op == "addsub_ext" and add.option in (4, 5, 6):  # sxtb/sxth/sxtw
                bitsz = {4: 8, 5: 16, 6: 32}[add.option]
                v &= mask(bitsz)
                if v >> (bitsz - 1):
                    v -= 1 << bitsz
            t = adr.target + (v << 2)
            if not self.inside(t):
                if n is not None:
                    raise GenError("jump table at %#x: target %#x outside function" % (br.addr, t))
                break
            targets.append(t)
        return targets or None

    def const_before(self, i, reg):
        """Constant value of `reg` before instruction i (adrp/add/adr chains);
        None if it is redefined by anything else."""
        add = 0
        for k in range(i - 1, max(-1, i - 256), -1):
            ins = self.insns[k]
            if ins.op == "addsub_imm" and ins.rd == reg and not ins.s and not ins.sub and ins.rn == reg:
                add += ins.imm
                continue
            if ins.op in ("adrp", "adr") and ins.rd == reg:
                return ins.target + add
            if reg in writes(ins):
                return None
        return None

    def bound_before(self, i, reg, depth=0):
        """Entry count of the table indexed by `reg` (from cmp #imm / b.hi), or None.

        Walks back through straight-line code following `mov` copies of the
        index; where the block is entered only by a jump, continues from the
        branches that target it. Stops when the index is redefined."""
        for k in range(i - 1, max(-1, i - 64), -1):
            ins = self.insns[k]
            if ins.op in ("b", "br", "ret", "brk"):
                if depth > 2:
                    return None
                leader = ins.addr + 4
                found = []
                for j, o in enumerate(self.insns):
                    if o.f.get("target") == leader and o.op != "bl":
                        n = self.bound_before(j + 1, reg, depth + 1)
                        if n is not None:
                            found.append(n)
                return max(found, key=abs) if found else None
            if ins.op == "addsub_imm" and ins.s and ins.sub and ins.rd == ZR and ins.rn == reg:
                # a bound check is cmp #n followed by b.hi (n+1 entries) or b.hs (n)
                nxt = self.insns[k + 1] if k + 1 < len(self.insns) else None
                if nxt is not None and nxt.op == "bcond" and nxt.cond in (8, 2):
                    return ins.imm + 1 if nxt.cond == 8 else ins.imm
                continue
            if ins.op == "logic_imm" and ins.opc == "and" and ins.rd == reg and ins.imm < 1024 and ins.imm & (ins.imm + 1) == 0:
                return -(ins.imm + 1)      # an upper limit only (may be a zero-extension)
            if ins.op == "logic_reg" and ins.opc == "orr" and ins.rn == ZR and ins.rd == reg \
                    and ins.amount == 0:
                reg = ins.rm
                continue
            if reg in writes(ins):
                return None
        return None

    def build_blocks(self):
        leaders = {self.start} | self.labels
        for ins in self.insns:
            if ins.op in ("b", "bcond", "cbz", "cbnz", "tbz", "tbnz", "br", "ret", "brk"):
                leaders.add(ins.addr + 4)
        leaders = sorted(a for a in leaders if self.inside(a))
        self.blocks = []
        for j, a in enumerate(leaders):
            e = leaders[j + 1] if j + 1 < len(leaders) else self.end
            self.blocks.append((a, e))
        self.block_of = {}
        for b, (a, e) in enumerate(self.blocks):
            for x in range(a, e, 4):
                self.block_of[x] = b

    def succs(self, b):
        a, e = self.blocks[b]
        last = self.insns[self.index[e - 4]]
        out = []
        op = last.op
        if op == "b":
            if self.inside(last.target):
                out.append(last.target)
        elif op in ("bcond", "cbz", "cbnz", "tbz", "tbnz"):
            if self.inside(last.target):
                out.append(last.target)
            out.append(e)
        elif op == "br":
            out.extend(self.jtables.get(last.addr, []))
        elif op in ("ret", "brk"):
            pass
        else:
            out.append(e)
        return [self.block_of[t] for t in out if t in self.block_of]

    # -- flags --------------------------------------------------------------

    def flag_kind(self, ins):
        """Kind of flags an instruction sets, or None."""
        op = ins.op
        if op in ("addsub_imm", "addsub_reg", "addsub_ext") and ins.s:
            return ("sub" if ins.sub else "add", 64 if ins.sf else 32)
        if op in ("logic_imm", "logic_reg") and ins.opc in ("ands", "bics"):
            return ("logic", 64 if ins.sf else 32)
        if op == "fcmp":
            return ("fcmp", 0)
        if op in ("ccmp", "fccmp"):
            return ("nzcv", 0)
        if op == "adc" and ins.s:
            return ("nzcv", 0)
        return None

    @staticmethod
    def flag_reader(ins):
        if ins.op in ("bcond", "csel", "ccmp", "fccmp", "fcsel"):
            return True
        if ins.op == "adc":
            return True
        return False

    def flag_flow(self):
        nb = len(self.blocks)
        UNK = "U"
        ins_state = {}
        block_in = [None] * nb
        block_in[0] = frozenset([UNK])
        preds_dirty = [0]
        seen = set()
        work = [0]
        while work:
            b = work.pop()
            state = block_in[b]
            a, e = self.blocks[b]
            for x in range(a, e, 4):
                ins = self.insns[self.index[x]]
                ins_state[x] = state
                if self.flag_kind(ins):
                    state = frozenset([x])
                elif ins.op in ("bl", "blr"):
                    state = frozenset([UNK])
            for s in self.succs(b):
                old = block_in[s]
                new = state if old is None else old | state
                if old is None or new != old:
                    block_in[s] = new
                    work.append(s)
        del preds_dirty, seen
        self.materialize = set()
        self.cond_mode = {}       # consumer addr -> ("direct", kind) | ("nzcv",)
        for ins in self.insns:
            if not self.flag_reader(ins):
                continue
            st = ins_state.get(ins.addr)
            if st is None:
                st = frozenset([UNK])        # unreachable code
            kinds = set()
            for s in st:
                kinds.add("U" if s == UNK else self.flag_kind(self.insns[self.index[s]]))
            if ins.op != "adc" and len(kinds) == 1:
                (k,) = kinds
                if k != "U" and k[0] != "nzcv":
                    self.cond_mode[ins.addr] = ("direct", k)
                    continue
            self.cond_mode[ins.addr] = ("nzcv",)
            for s in st:
                if s != UNK:
                    self.materialize.add(s)


def getattr_rd(ins):
    return ins.f.get("rd", ins.f.get("rt"))


CALL_CLOBBERS = frozenset(list(range(19)) + [30])


def writes(ins):
    """General registers an instruction writes (approximation good enough for
    the jump-table analysis)."""
    op = ins.op
    f = ins.f
    if op in ("bl", "blr"):
        return CALL_CLOBBERS
    if op in ("ldst", "ldstp"):
        out = set()
        if f.get("load") and not f.get("fp"):
            out.add(f["rt"])
            if op == "ldstp":
                out.add(f["rt2"])
        if f.get("mode") in ("pre", "post"):
            out.add(f["rn"])
        return out
    if op in ("ldr_lit", "ldar", "ldop"):
        return set() if f.get("fp") else {f["rt"]}
    if op == "cas":
        return {f["rs"]}
    if op in ("fcvti", "fmov_gen") and not f.get("to_fp", False):
        return {f["rd"]}
    if op == "vec" and f.get("name") in ("umov", "smov"):
        return {f["rd"]}
    if op in ("addsub_imm", "addsub_reg", "addsub_ext", "logic_imm", "logic_reg", "movz", "movn",
              "movk", "bfm", "extr", "csel", "dp1", "dp2", "dp3", "adr", "adrp", "adc"):
        return {f["rd"]}
    return set()


# ---------------------------------------------------------------------------


class Lower:
    """Lowers one analyzed function to C."""

    def __init__(self, fn, test_mode=False):
        self.fn = fn
        self.p = fn.p
        self.test_mode = test_mode
        self.out = []
        self.xs = set()       # general registers referenced
        self.vs = set()       # vector registers referenced
        self.wx = set()       # general registers written
        self.wv = set()       # vector registers written
        self.uses_sp = False
        self.wrote_sp = False
        self.uses_flags = False
        self.cur = None

    # -- register access ------------------------------------------------------

    def X(self, n, sf=1):
        if n == ZR:
            return "0" if sf else "0u"
        if n == SP:
            self.uses_sp = True
            return "sp" if sf else "(uint32_t)sp"
        self.xs.add(n)
        return "x%d" % n if sf else "(uint32_t)x%d" % n

    def setX(self, n, sf, expr):
        if n == ZR:
            return
        if n == SP:
            self.uses_sp = self.wrote_sp = True
            self.emit("sp = %s;" % (expr if sf else "(uint32_t)(%s)" % expr))
            return
        self.xs.add(n)
        self.wx.add(n)
        self.emit("x%d = %s;" % (n, expr if sf else "(uint32_t)(%s)" % expr))

    def V(self, n):
        self.vs.add(n)
        return "v%d" % n

    def setV(self, n, expr):
        self.vs.add(n)
        self.wv.add(n)
        self.emit("v%d = %s;" % (n, expr))

    def F(self, n, ft):
        v = self.V(n)
        if ft == 32:
            return "%s.f[0]" % v
        if ft == 64:
            return "%s.df[0]" % v
        return "h2f(%s.h[0])" % v

    def setF(self, n, ft, expr):
        if ft == 32:
            self.setV(n, "vset_f32(%s)" % expr)
        elif ft == 64:
            self.setV(n, "vset_f64(%s)" % expr)
        else:
            self.setV(n, "vset_u32(f2h(%s))" % expr)

    def emit(self, s):
        self.out.append("    " + s)

    # -- operand helpers ---------------------------------------------------------

    def shifted(self, rm, shtype, amount, sf):
        v = self.X(rm, sf)
        if amount == 0 and shtype != 3:
            return v
        w = 64 if sf else 32
        t = "uint64_t" if sf else "uint32_t"
        st = "int64_t" if sf else "int32_t"
        if shtype == 0:
            return "((%s)(%s << %d))" % (t, v, amount)
        if shtype == 1:
            return "(%s >> %d)" % (v, amount)
        if shtype == 2:
            return "((%s)((%s)%s >> %d))" % (t, st, v, amount)
        return "ror%d(%s, %d)" % (w, v, amount)

    def extended(self, rm, option, amount, sf):
        raw = self.X(rm, 1)
        e = {0: "(uint64_t)(uint8_t)%s", 1: "(uint64_t)(uint16_t)%s", 2: "(uint64_t)(uint32_t)%s",
             3: "%s", 4: "(uint64_t)(int64_t)(int8_t)%s", 5: "(uint64_t)(int64_t)(int16_t)%s",
             6: "(uint64_t)(int64_t)(int32_t)%s", 7: "%s"}[option] % raw
        if amount:
            e = "(%s << %d)" % (e, amount)
        return e if sf else "(uint32_t)(%s)" % e

    def ea(self, ins):
        """(address expression, writeback line or None) of a load/store."""
        base = self.X(ins.rn, 1)
        if ins.mode == "reg":
            off = self.extended(ins.rm, ins.option, ins.shift, 1)
            return "(%s + %s)" % (base, off), None
        imm = ins.imm
        off = "%s + %d" % (base, imm) if imm > 0 else ("%s - %d" % (base, -imm) if imm < 0 else base)
        if ins.mode == "off":
            return "(%s)" % off, None
        if ins.mode == "pre":
            return "(%s)" % off, (ins.rn, "(%s)" % off)
        return base, (ins.rn, "(%s)" % off)

    # -- condition codes ---------------------------------------------------------

    def cond(self, ins, c):
        """C expression for condition c at a flag consumer."""
        self.uses_flags = True
        name = CONDS[c]
        if name in ("al", "nv"):
            return "1"
        mode = self.fn.cond_mode.get(ins.addr, ("nzcv",))
        if mode[0] == "nzcv":
            return "cond_holds(nzcv, %d)" % c
        kind, w = mode[1]
        if kind == "sub":
            if w == 32:
                a, b, s = "(uint32_t)fa", "(uint32_t)fb", "(int32_t)"
            else:
                a, b, s = "fa", "fb", "(int64_t)"
            r = "(%s - %s)" % (a, b) if w == 64 else "(uint32_t)(%s - %s)" % (a, b)
            ov = "((((%s ^ %s) & (%s ^ %s)) >> %d) & 1)" % (a, b, a, r, w - 1)
            return {
                "eq": "%s == %s" % (a, b), "ne": "%s != %s" % (a, b),
                "hs": "%s >= %s" % (a, b), "lo": "%s < %s" % (a, b),
                "hi": "%s > %s" % (a, b), "ls": "%s <= %s" % (a, b),
                "ge": "%s%s >= %s%s" % (s, a, s, b), "lt": "%s%s < %s%s" % (s, a, s, b),
                "gt": "%s%s > %s%s" % (s, a, s, b), "le": "%s%s <= %s%s" % (s, a, s, b),
                "mi": "%s%s < 0" % (s, r), "pl": "%s%s >= 0" % (s, r),
                "vs": ov, "vc": "!%s" % ov,
            }[name]
        if kind == "logic":
            r, s = ("(uint32_t)fa", "(int32_t)") if w == 32 else ("fa", "(int64_t)")
            return {
                "eq": "%s == 0" % r, "ne": "%s != 0" % r, "mi": "%s%s < 0" % (s, r),
                "pl": "%s%s >= 0" % (s, r), "hs": "0", "lo": "1", "vs": "0", "vc": "1",
                "hi": "0", "ls": "1", "ge": "%s%s >= 0" % (s, r), "lt": "%s%s < 0" % (s, r),
                "gt": "%s%s > 0" % (s, r), "le": "%s%s <= 0" % (s, r),
            }[name]
        if kind == "add":
            if w == 32:
                return "cond_holds(flags_add32((uint32_t)fa, (uint32_t)fb), %d)" % c
            return "cond_holds(flags_add64(fa, fb), %d)" % c
        if kind == "fcmp":
            a, b = "fda", "fdb"
            return {
                "eq": "%s == %s" % (a, b), "ne": "!(%s == %s)" % (a, b),
                "hs": "!(%s < %s)" % (a, b), "lo": "%s < %s" % (a, b),
                "mi": "%s < %s" % (a, b), "pl": "!(%s < %s)" % (a, b),
                "vs": "(%s != %s || %s != %s)" % (a, a, b, b),
                "vc": "(%s == %s && %s == %s)" % (a, a, b, b),
                "hi": "!(%s <= %s)" % (a, b), "ls": "%s <= %s" % (a, b),
                "ge": "%s >= %s" % (a, b), "lt": "!(%s >= %s)" % (a, b),
                "gt": "%s > %s" % (a, b), "le": "!(%s > %s)" % (a, b),
            }[name]
        raise GenError("bad flag kind %r" % (kind,))

    def set_flags(self, ins, kind, a, b):
        """Flag setter: operands into fa/fb (or fda/fdb), nzcv if needed."""
        self.uses_flags = True
        k, w = kind
        if k == "fcmp":
            self.emit("fda = %s; fdb = %s;" % (a, b))
            if ins.addr in self.fn.materialize:
                self.emit("nzcv = flags_fcmp(fda, fdb);")
            return
        self.emit("fa = %s; fb = %s;" % (a, b) if b is not None else "fa = %s;" % a)
        if ins.addr in self.fn.materialize:
            if k == "logic":
                self.emit("nzcv = flags_logic%d(%sfa);" % (w, "(uint32_t)" if w == 32 else ""))
            else:
                cast = "(uint32_t)" if w == 32 else ""
                self.emit("nzcv = flags_%s%d(%sfa, %sfb);" % (k, w, cast, cast))

    # -- calls and control flow -------------------------------------------------

    def call_seq(self, call):
        self.emit("@STORE@")
        self.emit(call)
        self.emit("@RELOAD@")

    def tail_seq(self, call):
        self.emit("@STORE@")
        self.emit(call)
        self.emit("return;")

    def call_target(self, t):
        """C call statement for a direct call/jump to guest address t."""
        if t in self.p.func_starts:
            return "%s(c);" % cname(t)
        if t in self.p.stub_import:
            idx = self.p.stub_import[t]
            return "IMPORT_CALL(c, %d); /* %s */" % (idx, self.p.import_names[idx])
        return "rt_bad_jump(c, 0x%xu, 0x%xull);" % (rel(self.cur.addr), rel(t))

    def goto(self, t):
        """Statement for a branch to t (local label or tail call)."""
        if self.fn.inside(t):
            if t in self.fn.labels:
                return "goto %s;" % label(t)
            raise GenError("branch to %#x without label" % t)
        return "{ @STORE@ %s return; }" % self.call_target(t)

    def branch_if(self, cond, t):
        self.emit("if (%s) %s" % (cond, self.goto(t)))

    # -- main ---------------------------------------------------------------------------

    def run(self):
        fn = self.fn
        last = None
        for ins in fn.insns:
            if ins.addr in fn.labels:
                self.out.append("%s:" % label(ins.addr))
            self.cur = ins
            getattr(self, "i_" + ins.op, self.i_unknown)(ins)
            last = ins
        if last is not None and last.op not in ("b", "br", "ret", "brk"):
            nxt = fn.end
            if nxt in self.p.func_starts:
                self.emit("/* falls into the next function */")
                self.tail_seq("%s(c);" % cname(nxt))
            else:
                self.emit("rt_bad_jump(c, 0x%xu, 0x%xull);" % (rel(last.addr), rel(nxt)))
        return self.finish()

    def finish(self):
        fn = self.fn
        store = []
        for r in ARG_X:
            if r in self.wx:
                store.append("c->x[%d] = x%d;" % (r, r))
        for r in ARG_V:
            if r in self.wv:
                store.append("c->v[%d] = v%d;" % (r, r))
        if self.wrote_sp:
            store.append("c->sp = sp;")
        reload = []
        for r in RET_X:
            if r in self.xs:
                reload.append("x%d = c->x[%d];" % (r, r))
        for r in RET_V:
            if r in self.vs:
                reload.append("v%d = c->v[%d];" % (r, r))
        ret = []
        for r in (sorted(self.xs) if self.test_mode else RET_X):
            if r in self.xs:
                ret.append("c->x[%d] = x%d;" % (r, r))
        for r in (sorted(self.vs) if self.test_mode else RET_V):
            if r in self.vs:
                ret.append("c->v[%d] = v%d;" % (r, r))
        if self.test_mode and self.uses_flags:
            ret.append("c->nzcv = nzcv;")
        s_store, s_reload, s_ret = " ".join(store), " ".join(reload), " ".join(ret)

        name = self.p.names.get(fn.start, "")
        lines = ["/* %s */" % name.replace("*/", "* /") if name else "/* sub_%x */" % fn.start,
                 "void %s(Ctx *c)" % cname(fn.start), "{",
                 "    uint8_t *const gm = g_mem; (void)gm;"]
        if self.xs:
            lines.append("    uint64_t " + ", ".join("x%d = c->x[%d]" % (r, r) for r in sorted(self.xs)) + ";")
        if self.uses_sp or self.wrote_sp:
            lines.append("    uint64_t sp = c->sp;")
        if self.vs:
            lines.append("    V128 " + ", ".join("v%d = c->v[%d]" % (r, r) for r in sorted(self.vs)) + ";")
        if self.uses_flags:
            lines.append("    uint64_t fa = 0, fb = 0; double fda = 0, fdb = 0; uint32_t nzcv = %s;"
                         % ("c->nzcv" if self.test_mode else "0"))
            lines.append("    (void)fa; (void)fb; (void)fda; (void)fdb; (void)nzcv;")
        for l in self.out:
            l = l.replace("@STORE@", s_store).replace("@RELOAD@", s_reload).replace("@RET@", s_ret)
            if l.strip():
                lines.append(l)
        lines.append("}")
        return "\n".join(lines) + "\n"

    def i_unknown(self, ins):
        self.emit('rt_unimpl(c, 0x%xu, "%s");' % (rel(ins.addr), ins.op))

    def i_unk(self, ins):
        self.emit('rt_unimpl(c, 0x%xu, "undecoded %08x");' % (rel(ins.addr), ins.word))

    # -- branches -------------------------------------------------------------------

    def i_b(self, ins):
        if self.fn.inside(ins.target):
            self.emit(self.goto(ins.target))
        else:
            self.tail_seq(self.call_target(ins.target))

    def i_bl(self, ins):
        self.xs.add(30)
        self.wx.add(30)
        self.emit("x30 = 0x%xull;" % rel(ins.addr + 4))
        self.call_seq(self.call_target(ins.target))

    def i_bcond(self, ins):
        self.branch_if(self.cond(ins, ins.cond), ins.target)

    def i_cbz(self, ins):
        self.branch_if("%s == 0" % self.X(ins.rt, ins.sf), ins.target)

    def i_cbnz(self, ins):
        self.branch_if("%s != 0" % self.X(ins.rt, ins.sf), ins.target)

    def i_tbz(self, ins):
        self.branch_if("!((%s >> %d) & 1)" % (self.X(ins.rt, 1), ins.bit), ins.target)

    def i_tbnz(self, ins):
        self.branch_if("((%s >> %d) & 1)" % (self.X(ins.rt, 1), ins.bit), ins.target)

    def i_br(self, ins):
        targets = self.fn.jtables.get(ins.addr)
        r = self.X(ins.rn, 1)
        if targets:
            self.emit("switch (%s) {" % r)
            for t in sorted(set(targets)):
                self.emit("case 0x%xull: goto %s;" % (rel(t), label(t)))
            self.emit("default: rt_bad_jump(c, 0x%xu, %s);" % (rel(ins.addr), r))
            self.emit("}")
            return
        self.tail_seq("rt_call(c, %s);" % r)

    def i_blr(self, ins):
        r = self.X(ins.rn, 1)
        self.emit("{ uint64_t t_ = %s;" % r)
        self.xs.add(30)
        self.wx.add(30)
        self.emit("x30 = 0x%xull;" % rel(ins.addr + 4))
        self.call_seq("rt_call(c, t_); }")

    def i_ret(self, ins):
        self.emit("@RET@")
        self.emit("return;")

    def i_brk(self, ins):
        self.emit("rt_trap(c, 0x%xu, %d);" % (rel(ins.addr), ins.imm))

    def i_hint(self, ins):
        pass

    def i_barrier(self, ins):
        self.emit("__atomic_thread_fence(__ATOMIC_SEQ_CST);")

    def i_prfm(self, ins):
        pass

    # -- data processing (immediate) ----------------------------------------------------

    def i_adr(self, ins):
        self.setX(ins.rd, 1, "0x%xull" % rel(ins.target))

    def i_adrp(self, ins):
        self.setX(ins.rd, 1, "0x%xull" % rel(ins.target))

    def i_addsub_imm(self, ins):
        sf = ins.sf
        a = self.X(ins.rn, sf)
        if ins.s:
            self.set_flags(ins, ("sub" if ins.sub else "add", 64 if sf else 32), a, hexu(ins.imm, sf))
        if ins.rd == ZR and ins.s:
            return
        if ins.imm == 0:
            expr = a
        else:
            expr = "%s %s %s" % (a, "-" if ins.sub else "+", hexu(ins.imm, sf))
        self.setX(ins.rd, sf, expr)

    def i_addsub_reg(self, ins):
        sf = ins.sf
        a = self.X(ins.rn, sf)
        b = self.shifted(ins.rm, ins.shtype, ins.amount, sf)
        if ins.s:
            self.set_flags(ins, ("sub" if ins.sub else "add", 64 if sf else 32), a, b)
            if ins.rd == ZR:
                return
        self.setX(ins.rd, sf, "%s %s %s" % (a, "-" if ins.sub else "+", b))

    def i_addsub_ext(self, ins):
        sf = ins.sf
        a = self.X(ins.rn, sf)
        b = self.extended(ins.rm, ins.option, ins.amount, sf)
        if ins.s:
            self.set_flags(ins, ("sub" if ins.sub else "add", 64 if sf else 32), a, b)
            if ins.rd == ZR:
                return
        self.setX(ins.rd, sf, "%s %s %s" % (a, "-" if ins.sub else "+", b))

    def logic(self, ins, a, b, opc):
        sf = ins.sf
        op = {"and": "&", "ands": "&", "orr": "|", "eor": "^"}[opc]
        expr = "(%s %s %s)" % (a, op, b)
        if opc == "orr" and a in ("0", "0u"):
            expr = b
        if opc == "ands":
            self.set_flags(ins, ("logic", 64 if sf else 32), expr, None)
            if ins.rd == ZR:
                return
            expr = "fa"
        self.setX(ins.rd, sf, expr)

    def i_logic_imm(self, ins):
        self.logic(ins, self.X(ins.rn, ins.sf), hexu(ins.imm, ins.sf), ins.opc)

    def i_logic_reg(self, ins):
        sf = ins.sf
        b = self.shifted(ins.rm, ins.shtype, ins.amount, sf)
        opc = ins.opc
        if opc in ("bic", "orn", "eon", "bics"):
            b = "(%s)~%s" % ("uint64_t" if sf else "uint32_t", b)
            opc = {"bic": "and", "orn": "orr", "eon": "eor", "bics": "ands"}[opc]
        self.logic(ins, self.X(ins.rn, sf), b, opc)

    def i_movz(self, ins):
        self.setX(ins.rd, ins.sf, hexu(ins.imm << ins.shift, ins.sf))

    def i_movn(self, ins):
        self.setX(ins.rd, ins.sf, hexu(~(ins.imm << ins.shift), ins.sf))

    def i_movk(self, ins):
        sf = ins.sf
        keep = hexu(~(0xFFFF << ins.shift), sf)
        self.setX(ins.rd, sf, "(%s & %s) | %s" % (self.X(ins.rd, sf), keep, hexu(ins.imm << ins.shift, sf)))

    def i_bfm(self, ins):
        sf = ins.sf
        w = 64 if sf else 32
        t = "uint64_t" if sf else "uint32_t"
        st = "int64_t" if sf else "int32_t"
        r, s = ins.immr, ins.imms
        src = self.X(ins.rn, sf)
        if s >= r:
            width = s - r + 1
            lsb = r
            if ins.kind == "ubfm":
                expr = "((%s >> %d) & %s)" % (src, lsb, hexu(mask(width), sf))
            elif ins.kind == "sbfm":
                expr = "((%s)((%s)(%s << %d) >> %d))" % (t, st, src, w - 1 - s, w - width)
            else:
                dst = self.X(ins.rd, sf)
                expr = "((%s & %s) | ((%s >> %d) & %s))" % (dst, hexu(~mask(width), sf), src, lsb,
                                                            hexu(mask(width), sf))
        else:
            width = s + 1
            lsb = (w - r) % w
            if ins.kind == "ubfm":
                expr = "((%s & %s) << %d)" % (src, hexu(mask(width), sf), lsb)
            elif ins.kind == "sbfm":
                expr = "((%s)(((%s)(%s << %d) >> %d)) << %d)" % (t, st, src, w - width, w - width, lsb)
            else:
                dst = self.X(ins.rd, sf)
                fm = mask(width) << lsb
                expr = "((%s & %s) | ((%s & %s) << %d))" % (dst, hexu(~fm, sf), src, hexu(mask(width), sf), lsb)
        self.setX(ins.rd, sf, "(%s)%s" % (t, expr))

    def i_extr(self, ins):
        sf = ins.sf
        w = 64 if sf else 32
        n, m = self.X(ins.rn, sf), self.X(ins.rm, sf)
        if ins.lsb == 0:
            self.setX(ins.rd, sf, m)
        else:
            self.setX(ins.rd, sf, "(%s >> %d) | (%s << %d)" % (m, ins.lsb, n, w - ins.lsb))

    # -- data processing (register) -----------------------------------------------------

    def i_csel(self, ins):
        sf = ins.sf
        t = "uint64_t" if sf else "uint32_t"
        n, m = self.X(ins.rn, sf), self.X(ins.rm, sf)
        alt = {"csel": m, "csinc": "(%s)(%s + 1)" % (t, m), "csinv": "(%s)~%s" % (t, m),
               "csneg": "(%s)-%s" % (t, m)}[ins.kind]
        self.setX(ins.rd, sf, "(%s) ? %s : %s" % (self.cond(ins, ins.cond), n, alt))

    def i_ccmp(self, ins):
        sf = ins.sf
        w = 64 if sf else 32
        a = self.X(ins.rn, sf)
        b = hexu(ins.imm, sf) if ins.is_imm else self.X(ins.rm, sf)
        c = self.cond(ins, ins.cond)
        kind = "add" if ins.cmn else "sub"
        self.uses_flags = True
        self.emit("nzcv = (%s) ? flags_%s%d(%s, %s) : %du;" % (c, kind, w, a, b, ins.nzcv))

    def i_dp2(self, ins):
        sf = ins.sf
        w = 64 if sf else 32
        n, m = self.X(ins.rn, sf), self.X(ins.rm, sf)
        k = ins.kind
        if k in ("udiv", "sdiv"):
            expr = "%s%d(%s, %s)" % (k, w, n, m)
        elif k == "lslv":
            expr = "%s << (%s & %d)" % (n, m, w - 1)
        elif k == "lsrv":
            expr = "%s >> (%s & %d)" % (n, m, w - 1)
        elif k == "asrv":
            expr = "(%s)((%s)%s >> (%s & %d))" % ("uint64_t" if sf else "uint32_t",
                                                 "int64_t" if sf else "int32_t", n, m, w - 1)
        else:
            expr = "ror%d(%s, (unsigned)(%s & %d))" % (w, n, m, w - 1)
        self.setX(ins.rd, sf, expr)

    def i_dp1(self, ins):
        sf = ins.sf
        w = 64 if sf else 32
        n = self.X(ins.rn, sf)
        k = ins.kind
        if k == "rev":
            expr = "__builtin_bswap%d(%s)" % (w, n)
        elif k == "rev16":
            expr = "rev16_%d(%s)" % (w, n)
        elif k == "rev32":
            expr = "rev32_64(%s)" % n
        else:
            expr = "%s%d(%s)" % (k, w, n)
        self.setX(ins.rd, sf, expr)

    def i_dp3(self, ins):
        sf = ins.sf
        k = ins.kind
        if k in ("madd", "msub"):
            n, m, a = self.X(ins.rn, sf), self.X(ins.rm, sf), self.X(ins.ra, sf)
            self.setX(ins.rd, sf, "%s %s %s * %s" % (a, "+" if k == "madd" else "-", n, m))
            return
        if k in ("smulh", "umulh"):
            self.setX(ins.rd, 1, "%s(%s, %s)" % ("smulh64" if k == "smulh" else "umulh64",
                                                self.X(ins.rn, 1), self.X(ins.rm, 1)))
            return
        if k[0] == "s":
            n = "(uint64_t)(int64_t)(int32_t)%s" % self.X(ins.rn, 0)
            m = "(uint64_t)(int64_t)(int32_t)%s" % self.X(ins.rm, 0)
        else:
            n = "(uint64_t)%s" % self.X(ins.rn, 0)
            m = "(uint64_t)%s" % self.X(ins.rm, 0)
        a = self.X(ins.ra, 1)
        self.setX(ins.rd, 1, "%s %s %s * %s" % (a, "+" if k.endswith("addl") else "-", n, m))

    def i_adc(self, ins):
        self.i_unknown(ins)

    def i_mrs(self, ins):
        self.i_unknown(ins)

    def i_msr(self, ins):
        self.i_unknown(ins)

    # -- loads and stores ---------------------------------------------------------------

    def load_expr(self, size, signed, fp, addr):
        if fp:
            return {1: "vset_u32(LD8(%s))", 2: "vset_u32(LD16(%s))", 4: "vset_u32(LD32(%s))",
                    8: "vset_u64(LD64(%s))", 16: "LD128(%s)"}[size] % addr
        raw = {1: "LD8(%s)", 2: "LD16(%s)", 4: "LD32(%s)", 8: "LD64(%s)"}[size] % addr
        if signed == 64:
            ct = {1: "int8_t", 2: "int16_t", 4: "int32_t"}[size]
            return "(uint64_t)(int64_t)(%s)%s" % (ct, raw)
        if signed == 32:
            ct = {1: "int8_t", 2: "int16_t"}[size]
            return "(uint32_t)(int32_t)(%s)%s" % (ct, raw)
        return raw

    def do_load(self, rt, size, signed, fp, addr):
        e = self.load_expr(size, signed, fp, addr)
        if fp:
            self.setV(rt, e)
        else:
            self.setX(rt, 1 if (size == 8 or signed == 64) else 0, e)

    def do_store(self, rt, size, fp, addr):
        if fp:
            v = self.V(rt)
            val = {1: "%s.b[0]", 2: "%s.h[0]", 4: "%s.s[0]", 8: "%s.d[0]", 16: "%s"}[size] % v
        else:
            val = self.X(rt, 1 if size == 8 else 0)
        self.emit("ST%d(%s, %s);" % (size * 8, addr, val))

    def i_ldst(self, ins):
        addr, wb = self.ea(ins)
        if wb:
            self.emit("{ uint64_t ea_ = %s;" % addr)
            addr = "ea_"
        if ins.load:
            self.do_load(ins.rt, ins.size, ins.signed, ins.fp, addr)
        else:
            self.do_store(ins.rt, ins.size, ins.fp, addr)
        if wb:
            self.setX(wb[0], 1, wb[1])
            self.emit("}")

    def i_ldstp(self, ins):
        addr, wb = self.ea(ins)
        self.emit("{ uint64_t ea_ = %s;" % addr)
        a2 = "ea_ + %d" % ins.size
        if ins.load:
            signed = 64 if ins.signed else 0
            # load both before writing, in case rt aliases the base
            if not ins.fp:
                sf = 1 if (ins.size == 8 or signed) else 0
                e1 = self.load_expr(ins.size, signed, False, "ea_")
                e2 = self.load_expr(ins.size, signed, False, "(%s)" % a2)
                self.emit("uint64_t l1_ = %s, l2_ = %s;" % (e1, e2))
                if wb:
                    self.setX(wb[0], 1, wb[1])
                self.setX(ins.rt, sf, "l1_")
                self.setX(ins.rt2, sf, "l2_")
                self.emit("}")
                return
            self.do_load(ins.rt, ins.size, 0, True, "ea_")
            self.do_load(ins.rt2, ins.size, 0, True, "(%s)" % a2)
        else:
            self.do_store(ins.rt, ins.size, ins.fp, "ea_")
            self.do_store(ins.rt2, ins.size, ins.fp, "(%s)" % a2)
        if wb:
            self.setX(wb[0], 1, wb[1])
        self.emit("}")

    def i_ldr_lit(self, ins):
        self.do_load(ins.rt, ins.size, 64 if ins.signed else 0, ins.fp, "0x%xull" % rel(ins.target))

    def i_ldar(self, ins):
        t = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}[ins.size]
        self.setX(ins.rt, 1 if ins.size == 8 else 0, "AT_LOAD(%s, GA(%s))" % (t, self.X(ins.rn, 1)))

    def i_stlr(self, ins):
        t = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}[ins.size]
        self.emit("AT_STORE(%s, GA(%s), %s);" % (t, self.X(ins.rn, 1), self.X(ins.rt, 1 if ins.size == 8 else 0)))

    def i_cas(self, ins):
        n = ins.size * 8
        t = "uint%d_t" % n
        sf = 1 if ins.size == 8 else 0
        self.emit("{ %s old_ = rt_cas%d(GA(%s), (%s)%s, (%s)%s);" % (
            t, n, self.X(ins.rn, 1), t, self.X(ins.rs, sf), t, self.X(ins.rt, sf)))
        self.setX(ins.rs, sf, "old_")
        self.emit("}")

    def i_ldop(self, ins):
        n = ins.size * 8
        t = "uint%d_t" % n
        sf = 1 if ins.size == 8 else 0
        if ins.kind in ("smax", "smin", "umax", "umin") and n < 32:
            self.i_unknown(ins)
            return
        self.emit("{ %s old_ = rt_%s%d(GA(%s), (%s)%s);" % (
            t, "swp" if ins.kind == "swp" else "ld" + ins.kind, n, self.X(ins.rn, 1), t, self.X(ins.rs, sf)))
        self.setX(ins.rt, sf, "old_")
        self.emit("}")

    def i_ldxr(self, ins):
        self.i_unknown(ins)

    def i_stxr(self, ins):
        self.i_unknown(ins)

    # -- scalar floating point ----------------------------------------------------------

    def i_fmov_gen(self, ins):
        if ins.to_fp:
            if ins.top:
                v = self.V(ins.rd)
                self.wv.add(ins.rd)
                self.emit("%s.d[1] = %s;" % (v, self.X(ins.rn, 1)))
            elif ins.ft == 64:
                self.setV(ins.rd, "vset_u64(%s)" % self.X(ins.rn, 1))
            elif ins.ft == 32:
                self.setV(ins.rd, "vset_u32(%s)" % self.X(ins.rn, 0))
            else:
                self.setV(ins.rd, "vset_u32((uint16_t)%s)" % self.X(ins.rn, 0))
            return
        v = self.V(ins.rn)
        if ins.top:
            self.setX(ins.rd, 1, "%s.d[1]" % v)
        elif ins.ft == 64:
            self.setX(ins.rd, 1, "%s.d[0]" % v)
        elif ins.ft == 32:
            self.setX(ins.rd, 0, "%s.s[0]" % v)
        else:
            self.setX(ins.rd, ins.sf, "%s.h[0]" % v)

    def i_fmov_imm(self, ins):
        if ins.ft == 32:
            self.setV(ins.rd, "vset_u32(0x%08xu) /* %r */" % (struct.unpack("<I", struct.pack("<f", ins.value))[0], ins.value))
        elif ins.ft == 64:
            self.setV(ins.rd, "vset_u64(0x%016xull) /* %r */" % (struct.unpack("<Q", struct.pack("<d", ins.value))[0], ins.value))
        else:
            self.setF(ins.rd, 16, "%r" % ins.value)

    def i_fp1(self, ins):
        ft, k = ins.ft, ins.kind
        if k == "fmov":
            v = self.V(ins.rn)
            self.setV(ins.rd, {32: "vset_u32(%s.s[0])", 64: "vset_u64(%s.d[0])", 16: "vset_u32(%s.h[0])"}[ft] % v)
            return
        a = self.F(ins.rn, ft)
        suf = "f32" if ft != 64 else "f64"
        fsuf = "f" if ft != 64 else ""
        expr = {
            "fabs": "fabs_%s(%s)" % (suf, a), "fneg": "fneg_%s(%s)" % (suf, a),
            "fsqrt": "sqrt%s(%s)" % (fsuf, a), "frintn": "rint%s(%s)" % (fsuf, a),
            "frintp": "ceil%s(%s)" % (fsuf, a), "frintm": "floor%s(%s)" % (fsuf, a),
            "frintz": "trunc%s(%s)" % (fsuf, a), "frinta": "round%s(%s)" % (fsuf, a),
            "frintx": "rint%s(%s)" % (fsuf, a), "frinti": "nearbyint%s(%s)" % (fsuf, a),
        }[k]
        self.setF(ins.rd, ft, expr)

    def i_fcvt(self, ins):
        a = self.F(ins.rn, ins.ft)
        cast = {32: "(float)", 64: "(double)", 16: "(float)"}[ins.to]
        self.setF(ins.rd, ins.to, "%s%s" % (cast, a))

    def i_fp2(self, ins):
        ft, k = ins.ft, ins.kind
        a, b = self.F(ins.rn, ft), self.F(ins.rm, ft)
        suf = "f32" if ft != 64 else "f64"
        expr = {
            "fadd": "%s + %s" % (a, b), "fsub": "%s - %s" % (a, b), "fmul": "%s * %s" % (a, b),
            "fdiv": "%s / %s" % (a, b), "fnmul": "fneg_%s(%s * %s)" % (suf, a, b),
            "fmax": "fmax_%s(%s, %s)" % (suf, a, b), "fmin": "fmin_%s(%s, %s)" % (suf, a, b),
            "fmaxnm": "fmaxnm_%s(%s, %s)" % (suf, a, b), "fminnm": "fminnm_%s(%s, %s)" % (suf, a, b),
        }[k]
        if ft == 16:
            expr = "(float)(%s)" % expr
        self.setF(ins.rd, ft, expr)

    def i_fp3(self, ins):
        ft = ins.ft
        n, m, a = self.F(ins.rn, ft), self.F(ins.rm, ft), self.F(ins.ra, ft)
        fma = "FMA_F64" if ft == 64 else "FMA_F32"
        neg = "fneg_f64" if ft == 64 else "fneg_f32"
        expr = {
            "fmadd": "%s(%s, %s, %s)" % (fma, n, m, a),
            "fmsub": "%s(%s(%s), %s, %s)" % (fma, neg, n, m, a),
            "fnmadd": "%s(%s(%s), %s, %s(%s))" % (fma, neg, n, m, neg, a),
            "fnmsub": "%s(%s, %s, %s(%s))" % (fma, n, m, neg, a),
        }[ins.kind]
        self.setF(ins.rd, ft, expr)

    def i_fcmp(self, ins):
        a = self.F(ins.rn, ins.ft)
        b = "0.0" if ins.zero else self.F(ins.rm, ins.ft)
        self.set_flags(ins, ("fcmp", 0), "(double)%s" % a, "(double)%s" % b)

    def i_fccmp(self, ins):
        a, b = self.F(ins.rn, ins.ft), self.F(ins.rm, ins.ft)
        c = self.cond(ins, ins.cond)
        self.emit("nzcv = (%s) ? flags_fcmp(%s, %s) : %du;" % (c, a, b, ins.nzcv))

    def i_fcsel(self, ins):
        c = self.cond(ins, ins.cond)
        n, m = self.V(ins.rn), self.V(ins.rm)
        if ins.ft == 64:
            self.setV(ins.rd, "vset_u64((%s) ? %s.d[0] : %s.d[0])" % (c, n, m))
        elif ins.ft == 32:
            self.setV(ins.rd, "vset_u32((%s) ? %s.s[0] : %s.s[0])" % (c, n, m))
        else:
            self.setV(ins.rd, "vset_u32((%s) ? %s.h[0] : %s.h[0])" % (c, n, m))

    def i_cvtf(self, ins):
        if ins.sf:
            src = "(int64_t)%s" % self.X(ins.rn, 1) if ins.signed else self.X(ins.rn, 1)
        else:
            src = "(int32_t)%s" % self.X(ins.rn, 0) if ins.signed else self.X(ins.rn, 0)
        if ins.ft == 64:
            expr = "(double)%s" % src
            if ins.fbits:
                expr = "(%s / %r)" % (expr, float(1 << ins.fbits))
        else:
            if ins.fbits:
                expr = "(float)((double)%s / %r)" % (src, float(1 << ins.fbits))
            else:
                expr = "(float)%s" % src
        self.setF(ins.rd, ins.ft, expr)

    def i_fcvti(self, ins):
        a = "(double)%s" % self.F(ins.rn, ins.ft)
        if ins.fbits:
            a = "(%s * %r)" % (a, float(1 << ins.fbits))
        if ins.sf:
            fn = "cvt_s64" if ins.signed else "cvt_u64"
            self.setX(ins.rd, 1, "(uint64_t)%s(%s, '%s')" % (fn, a, ins.rmode))
        else:
            fn = "cvt_s32" if ins.signed else "cvt_u32"
            self.setX(ins.rd, 0, "(uint32_t)%s(%s, '%s')" % (fn, a, ins.rmode))

    # -- Advanced SIMD ------------------------------------------------------------------

    CAT = {}

    def i_vec(self, ins):
        name = ins.name
        es = ins.esize
        q = ins.q
        lanes = 1 if ins.scalar else (64 << q) // es
        cat = VEC_CAT.get(name)
        d = ins.rd
        if cat == "bin":
            self.setV(d, "vop_%s(%s, %s, %d, %d)" % (name, self.V(ins.rn), self.V(ins.rm), es, lanes))
        elif cat == "sel":
            self.setV(d, "vop_%s(%s, %s, %s, %d)" % (name, self.V(d), self.V(ins.rn), self.V(ins.rm), 2 if q else 1))
        elif cat == "acc":
            self.setV(d, "vop_%s(%s, %s, %s, %d, %d)" % (name, self.V(d), self.V(ins.rn), self.V(ins.rm), es, lanes))
        elif cat == "un":
            self.setV(d, "vop_%s(%s, %d, %d)" % (name, self.V(ins.rn), es, lanes))
        elif cat == "shift":
            self.setV(d, "vop_%s(%s, %d, %d, %d)" % (name, self.V(ins.rn), ins.shift, es, lanes))
        elif cat == "shacc":
            self.setV(d, "vop_%s(%s, %s, %d, %d, %d)" % (name, self.V(d), self.V(ins.rn), ins.shift, es, lanes))
        elif cat == "widen":
            self.setV(d, "vop_%s(%s, %d, %d, %d)" % (name, self.V(ins.rn), ins.shift, es, ins.part))
        elif cat == "shll":
            self.setV(d, "vop_shll(%s, %d, %d)" % (self.V(ins.rn), es, ins.part))
        elif cat == "narrow":
            self.setV(d, "vop_shrn(%s, %s, %d, %d, %d)" % (self.V(d), self.V(ins.rn),
                                                        ins.f.get("shift", 0), es, ins.part))
        elif cat == "fcvtl":
            self.setV(d, "vop_fcvtl(%s, %d, %d)" % (self.V(ins.rn), es, q))
        elif cat == "fcvtn":
            self.setV(d, "vop_fcvtn(%s, %s, %d, %d)" % (self.V(d), self.V(ins.rn), es, q))
        elif cat == "long":
            self.setV(d, "vop_%s(%s, %s, %d, %d)" % (name, self.V(ins.rn), self.V(ins.rm), es, ins.part))
        elif cat == "across":
            self.setV(d, "vop_%s(%s, %d, %d)" % (name, self.V(ins.rn), es, (64 << q) // es))
        elif cat == "pairscalar":
            self.setV(d, "vop_%s(%s, %d)" % (name, self.V(ins.rn), es))
        elif cat == "elem":
            self.setV(d, "vop_%s(%s, %s, %d, %d, %d)" % (name, self.V(ins.rn), self.V(ins.rm), ins.idx, es, lanes))
        elif cat == "elemacc":
            self.setV(d, "vop_%s(%s, %s, %s, %d, %d, %d)" % (name, self.V(d), self.V(ins.rn), self.V(ins.rm),
                                                          ins.idx, es, lanes))
        elif name == "dup_elem":
            self.setV(d, "vop_dup_elem(%s, %d, %d, %d)" % (self.V(ins.rn), ins.idx, es, lanes))
        elif name == "dup_gen":
            self.setV(d, "vop_dup_gen(%s, %d, %d)" % (self.X(ins.rn, 1), es, lanes))
        elif name == "ins_gen":
            self.setV(d, "vop_ins_gen(%s, %s, %d, %d)" % (self.V(d), self.X(ins.rn, 1), ins.idx, es))
        elif name == "ins_elem":
            self.setV(d, "vop_ins_elem(%s, %s, %d, %d, %d)" % (self.V(d), self.V(ins.rn), ins.idx, ins.idx2, es))
        elif name == "umov":
            self.setX(d, 1 if es == 64 else 0, "vget(%s, %d, %d)" % (self.V(ins.rn), es, ins.idx))
        elif name == "smov":
            if q:
                self.setX(d, 1, "(uint64_t)vgets(%s, %d, %d)" % (self.V(ins.rn), es, ins.idx))
            else:
                self.setX(d, 0, "(uint32_t)vgets(%s, %d, %d)" % (self.V(ins.rn), es, ins.idx))
        elif name == "movi":
            self.setV(d, "vop_movi(0x%xull, %d)" % (ins.imm & mask(64), q))
        elif name in ("orr_imm", "bic_imm"):
            self.setV(d, "vop_%s(%s, 0x%xull, %d)" % (name, self.V(d), ins.imm & mask(64), q))
        elif name == "ext":
            self.setV(d, "vop_ext(%s, %s, %d, %d)" % (self.V(ins.rn), self.V(ins.rm), ins.imm, q))
        elif name in ("tbl", "tbx"):
            regs = ", ".join(self.V((ins.rn + k) % 32) for k in range(ins.imm))
            self.emit("{ const V128 t_[%d] = { %s };" % (ins.imm, regs))
            self.setV(d, "vop_tbl(t_, %d, %s, %s, %d, %d)" % (ins.imm, self.V(ins.rm), self.V(d), q,
                                                            1 if name == "tbx" else 0))
            self.emit("}")
        else:
            self.i_unknown(Insn_like(ins, "vec:" + name))

    def vbase(self, ins):
        return self.X(ins.rn, 1)

    def vpost(self, ins):
        if not ins.post:
            return
        if ins.rm == 31:
            self.setX(ins.rn, 1, "%s + %d" % (self.X(ins.rn, 1), ins.total))
        else:
            self.setX(ins.rn, 1, "%s + %s" % (self.X(ins.rn, 1), self.X(ins.rm, 1)))

    def i_vldst_multi(self, ins):
        es, q = ins.esize, ins.q
        lanes = (64 << q) // es
        nb = es // 8
        regs = [(ins.rt + k) % 32 for k in range(ins.nregs)]
        self.emit("{ uint64_t ea_ = %s;" % self.vbase(ins))
        if ins.selem == 1:
            step = 16 if q else 8
            for k, r in enumerate(regs):
                a = "ea_ + %d" % (k * step)
                if ins.load:
                    self.setV(r, "LD128(%s)" % a if q else "vset_u64(LD64(%s))" % a)
                else:
                    if q:
                        self.emit("ST128(%s, %s);" % (a, self.V(r)))
                    else:
                        self.emit("ST64(%s, %s.d[0]);" % (a, self.V(r)))
        else:
            s = ins.selem
            if ins.load:
                self.emit("V128 t_[4] = { vzero(), vzero(), vzero(), vzero() };")
                self.emit("for (int e_ = 0; e_ < %d; e_++) for (int r_ = 0; r_ < %d; r_++) "
                          "vput(&t_[r_], %d, e_, LD%d(ea_ + (uint64_t)((e_ * %d + r_) * %d)));"
                          % (lanes, s, es, es, s, nb))
                for k, r in enumerate(regs):
                    self.setV(r, "t_[%d]" % k)
            else:
                self.emit("const V128 t_[4] = { %s };" % ", ".join(
                    [self.V(r) for r in regs] + ["vzero()"] * (4 - len(regs))))
                self.emit("for (int e_ = 0; e_ < %d; e_++) for (int r_ = 0; r_ < %d; r_++) "
                          "ST%d(ea_ + (uint64_t)((e_ * %d + r_) * %d), vget(t_[r_], %d, e_));"
                          % (lanes, s, es, s, nb, es))
        self.vpost(ins)
        self.emit("}")

    def i_vld_rep(self, ins):
        es, q = ins.esize, ins.q
        lanes = (64 << q) // es
        nb = es // 8
        self.emit("{ uint64_t ea_ = %s;" % self.vbase(ins))
        for k in range(ins.selem):
            r = (ins.rt + k) % 32
            self.setV(r, "vop_dup_gen(LD%d(ea_ + %d), %d, %d)" % (es, k * nb, es, lanes))
        self.vpost(ins)
        self.emit("}")

    def i_vldst_one(self, ins):
        es = ins.esize
        nb = es // 8
        self.emit("{ uint64_t ea_ = %s;" % self.vbase(ins))
        for k in range(ins.selem):
            r = (ins.rt + k) % 32
            a = "ea_ + %d" % (k * nb)
            if ins.load:
                self.setV(r, "vop_ins_gen(%s, LD%d(%s), %d, %d)" % (self.V(r), es, a, ins.idx, es))
            else:
                self.emit("ST%d(%s, vget(%s, %d, %d));" % (es, a, self.V(r), es, ins.idx))
        self.vpost(ins)
        self.emit("}")


class Insn_like:
    def __init__(self, ins, op):
        self.addr, self.op, self.word = ins.addr, op, ins.word


VEC_CAT = {}
for _n in ("add sub mul and orr eor bic orn cmeq cmtst cmhi cmhs cmgt cmge umax umin smax smin "
           "uhadd shadd uabd sabd ushl sshl addp zip1 zip2 uzp1 uzp2 trn1 trn2 "
           "fadd fsub fmul fdiv fmax fmin fmaxnm fminnm fabd fmulx fcmeq fcmge fcmgt facge facgt "
           "faddp frecps frsqrts").split():
    VEC_CAT[_n] = "bin"
for _n in "bsl bit bif".split():
    VEC_CAT[_n] = "sel"
for _n in "mla mls fmla fmls".split():
    VEC_CAT[_n] = "acc"
for _n in ("not neg abs cmeqz cmgtz cmgez cmltz cmlez cnt clz rbit rev64 rev32 rev16 "
           "fneg fabs fsqrt frintm frintp frintz frinta frintn frintx frinti frecpe frsqrte "
           "fcmeqz fcmgez fcmgtz fcmlez fcmltz scvtf ucvtf fcvtzs fcvtzu fcvtns fcvtnu fcvtms fcvtmu "
           "fcvtps fcvtpu fcvtas fcvtau").split():
    VEC_CAT[_n] = "un"
for _n in "shl ushr sshr scvtf_fx ucvtf_fx fcvtzs_fx fcvtzu_fx".split():
    VEC_CAT[_n] = "shift"
for _n in "usra ssra sli sri".split():
    VEC_CAT[_n] = "shacc"
for _n in "ushll sshll".split():
    VEC_CAT[_n] = "widen"
VEC_CAT["shll"] = "shll"
VEC_CAT["shrn"] = "narrow"
VEC_CAT["xtn"] = "narrow"
VEC_CAT["fcvtl"] = "fcvtl"
VEC_CAT["fcvtn"] = "fcvtn"
for _n in "umull smull uaddl saddl usubl ssubl uaddw saddw usubw ssubw".split():
    VEC_CAT[_n] = "long"
for _n in "addv uaddlv saddlv umaxv uminv smaxv sminv fmaxv fminv fmaxnmv fminnmv".split():
    VEC_CAT[_n] = "across"
for _n in "faddp_scalar addp_scalar fmaxp_scalar fminp_scalar fmaxnmp_scalar fminnmp_scalar".split():
    VEC_CAT[_n] = "pairscalar"
for _n in "fmul_elem fmulx_elem mul_elem".split():
    VEC_CAT[_n] = "elem"
for _n in "fmla_elem fmls_elem mla_elem".split():
    VEC_CAT[_n] = "elemacc"


# ---------------------------------------------------------------------------
# Output


def native_ident(name):
    """C identifier of the native that replaces guest function `name`."""
    n = name[1:] if name.startswith("_") else name
    return "nat_" + "".join(ch if ch.isalnum() or ch == "_" else "_" for ch in n)


def gen_function(prog, start, end, natives=()):
    fn = Func(prog, start, end)
    fn.analyze()
    code = Lower(fn).run()
    name = prog.names.get(start)
    if name in natives:
        # prototypes here, not in funcs.h: changing the natives must not touch every file
        code = code.replace("void %s(Ctx *c)" % cname(start), "void %s_orig(Ctx *c);\nvoid %s(Ctx *c);\nvoid %s_orig(Ctx *c)"
                            % (cname(start), native_ident(name), cname(start)), 1)
        code += "void %s(Ctx *c) { %s(c); }\n" % (cname(start), native_ident(name))
    return code


def write_tables(prog, out):
    m = prog.m
    lines = ['#include "recomp.h"', '#include "funcs.h"', ""]
    lines.append("const GuestFunc g_guest_funcs[] = {")
    for a, _ in sorted(prog.funcs):
        lines.append("    { 0x%xu, %s }," % (rel(a), cname(a)))
    lines.append("};")
    lines.append("const uint32_t g_guest_func_count = %d;" % len(prog.funcs))
    lines.append("const char *const g_guest_func_names[] = {")
    for a, _ in sorted(prog.funcs):
        lines.append('    "%s",' % prog.names.get(a, "sub_%x" % a))
    lines.append("};")
    lines.append("")
    lines.append("const char *const g_import_names[] = {")
    for n in prog.import_names:
        lines.append('    "%s",' % n)
    lines.append("};")
    lines.append("const uint32_t g_import_count = %d;" % len(prog.import_names))
    lines.append("")
    # the part of ycAudio::Mixer::s_sources the smaller rings leave untouched
    src = next(sy.value for sy in m.symbols if sy.name == "__ZN7ycAudio5Mixer9s_sourcesE")
    if _patched != set(PATCH_HI16):
        raise SystemExit("patches not applied: %s" % ", ".join("%#x" % a for a in set(PATCH_HI16) - _patched))
    lines.append("/* untouched part of ycAudio::Mixer::s_sources (rings of %#x bytes, not 0x40000) */" % AUDIO_RING)
    lines.append("const uint32_t g_spare_lo = 0x%xu, g_spare_hi = 0x%xu;" %
                 (rel(src + 0xc00 + 256 * _STRIDE), rel(src + 0xc00 + 256 * _OLD_STRIDE)))
    lines.append("")
    lines.append("/* bind: slot (guest address) receives import + addend */")
    lines.append("const GuestBind g_binds[] = {")
    for b in m.binds:
        lines.append("    { 0x%xu, %d, %d }," % (rel(b.addr), prog.import_index[b.name], b.addend))
    lines.append("};")
    lines.append("const uint32_t g_bind_count = %d;" % len(m.binds))
    lines.append("")
    lines.append("/* rebase: 64-bit pointers to move from 0x1_0000_0000 to the guest image base */")
    lines.append("const uint32_t g_rebases[] = {")
    rb = sorted(rel(a) for a in m.rebases)
    for k in range(0, len(rb), 8):
        lines.append("    " + ", ".join("0x%xu" % a for a in rb[k:k + 8]) + ",")
    lines.append("};")
    lines.append("const uint32_t g_rebase_count = %d;" % len(rb))
    lines.append("")
    lines.append("/* stubs (for code that takes their address) */")
    lines.append("const GuestStub g_stubs[] = {")
    for a, i in sorted(prog.stub_import.items()):
        lines.append("    { 0x%xu, %d }," % (rel(a), i))
    lines.append("};")
    lines.append("const uint32_t g_stub_count = %d;" % len(prog.stub_import))
    lines.append("")
    segs = []
    for s in m.segments:
        if s.name == "__PAGEZERO" or s.name == "__LINKEDIT":
            continue
        segs.append(s)
    lines.append("/* segments: guest address, size, file offset (in the arm64 slice), file size */")
    lines.append("const GuestSegment g_segments[] = {")
    for s in segs:
        lines.append("    { 0x%xu, 0x%xu, 0x%xu, 0x%xu }, /* %s */" % (rel(s.vmaddr), s.vmsize, s.fileoff, s.filesize, s.name))
    lines.append("};")
    lines.append("const uint32_t g_segment_count = %d;" % len(segs))
    tdata = m.section("__DATA", "__thread_data")
    tbss = m.section("__DATA", "__thread_bss")
    tvars = m.section("__DATA", "__thread_vars")
    tlo = min(s.addr for s in (tdata, tbss) if s) if (tdata or tbss) else 0
    thi = max(s.end for s in (tdata, tbss) if s) if (tdata or tbss) else 0
    lines.append("")
    lines.append("/* thread-local template and descriptors */")
    lines.append("const uint32_t g_tls_template = 0x%xu, g_tls_size = 0x%xu;" % (rel(tlo) if tlo else 0, thi - tlo))
    lines.append("const uint32_t g_tls_vars = 0x%xu, g_tls_vars_size = 0x%xu;" % (
        rel(tvars.addr) if tvars else 0, tvars.size if tvars else 0))
    lines.append("const uint32_t g_entry = 0x%xu;" % rel(m.entry))
    mi = m.section("__DATA_CONST", "__mod_init_func")
    lines.append("const uint32_t g_mod_init = 0x%xu, g_mod_init_count = %d;" % (
        rel(mi.addr) if mi else 0, (mi.size // 8) if mi else 0))
    cl = m.section("__DATA_CONST", "__objc_classlist")
    mn = m.section("__TEXT", "__objc_methname")
    lines.append("const uint32_t g_objc_classlist = 0x%xu, g_objc_classlist_count = %d;" % (
        rel(cl.addr) if cl else 0, (cl.size // 8) if cl else 0))
    lines.append("const uint32_t g_objc_methname = 0x%xu, g_objc_methname_size = 0x%xu;" % (
        rel(mn.addr) if mn else 0, mn.size if mn else 0))
    with open(os.path.join(out, "tables.c"), "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("out")
    ap.add_argument("--only", help="comma-separated function addresses (debug)")
    ap.add_argument("--natives", help="file listing guest functions replaced by natives")
    args = ap.parse_args()
    prog = Program(args.exe)
    natives = set()
    if args.natives:
        with open(args.natives) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    natives.add(line)
        known = set(prog.names.values())
        for n in sorted(natives - known):
            print("warning: native %s matches no function" % n, file=sys.stderr)
    os.makedirs(args.out, exist_ok=True)

    funcs = sorted(prog.funcs)
    if args.only:
        want = {int(x, 16) for x in args.only.split(",")}
        funcs = [f for f in funcs if f[0] in want]

    import io
    with io.StringIO() as f:
        f.write("/* generated */\n#ifndef MINA_FUNCS_H\n#define MINA_FUNCS_H\n#include \"recomp.h\"\n")
        for a, _ in sorted(prog.funcs):
            f.write("void %s(Ctx *c);\n" % cname(a))
        f.write("#endif\n")
        header = f.getvalue()
    hp = os.path.join(args.out, "funcs.h")
    if not os.path.exists(hp) or open(hp).read() != header:
        with open(hp, "w") as f:
            f.write(header)

    errors = 0
    files = []
    for k in range(0, len(funcs), FUNCS_PER_FILE):
        chunk = funcs[k:k + FUNCS_PER_FILE]
        path = os.path.join(args.out, "funcs_%03d.c" % (k // FUNCS_PER_FILE))
        files.append(os.path.basename(path))
        parts = ['/* generated by tools/recomp/gen.py */\n#include "funcs.h"\n\n']
        for a, e in chunk:
            try:
                parts.append(gen_function(prog, a, e, natives))
            except GenError as ex:
                errors += 1
                print("error in %s (%#x): %s" % (prog.names.get(a, "?"), a, ex), file=sys.stderr)
                parts.append("void %s(Ctx *c) { rt_unimpl(c, 0x%xu, \"generator error\"); }\n" % (cname(a), rel(a)))
            parts.append("\n")
        text = "".join(parts)
        old = None
        if os.path.exists(path):
            with open(path) as f:
                old = f.read()
        if old != text:
            with open(path, "w") as f:
                f.write(text)
    write_tables(prog, args.out)
    with open(os.path.join(args.out, "files.txt"), "w") as f:
        f.write("\n".join(files) + "\n")
    print("%d functions, %d files, %d errors" % (len(funcs), len(files), errors))


if __name__ == "__main__":
    main()
