"""Differential test of the instruction lowering against the real CPU.

The Mac is arm64: every sampled instruction of the game runs natively (as
`.inst WORD` inside an asm harness) and through its generated C, from the
same random register/flag/memory state, and the results are compared bit for
bit. Instructions come from the executable itself, a few per semantic class.

    python3 tools/recomp/difftest.py <MinaTheHollower> <workdir> [--per-class N] [--states N]
"""

import argparse
import collections
import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "analysis"))
from a64 import SP, ZR, decode  # noqa: E402
from gen import Func, Lower, Program, cname  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

SKIP_OPS = {"b", "bl", "br", "blr", "ret", "bcond", "cbz", "cbnz", "tbz", "tbnz", "brk", "adr",
            "adrp", "ldr_lit", "mrs", "msr", "unk", "hint", "ldxr", "stxr", "adc"}
MEM_OPS = {"ldst", "ldstp", "ldar", "stlr", "cas", "ldop", "vldst_multi", "vld_rep", "vldst_one"}
FLAG_SETTERS = None
RET = 0xD65F03C0


def class_key(ins):
    f = ins.f
    if ins.op == "vec":
        return ("vec", f["name"], f["esize"], f["q"], f.get("scalar", 0))
    keep = ("sf", "sub", "s", "opc", "kind", "shtype", "option", "size", "signed", "fp", "mode",
            "load", "ft", "to", "zero", "to_fp", "top", "rmode", "is_imm", "cmn", "selem", "nregs",
            "esize", "q", "post")
    return (ins.op,) + tuple((k, f[k]) for k in keep if k in f)


def regs_of(ins):
    out = set()
    for k in ("rd", "rn", "rm", "ra", "rt", "rt2", "rs"):
        if k in ins.f:
            out.add(ins.f[k])
    return out


def usable(ins):
    if ins.op in SKIP_OPS:
        return False
    if ins.op == "vec" and ins.f["name"] in ("dup_gen", "ins_gen", "umov", "smov") and 18 in regs_of(ins):
        return False
    gp = regs_of(ins) if ins.op != "vec" else set()
    if ins.op in ("fp1", "fp2", "fp3", "fcmp", "fccmp", "fcsel", "fcvt", "fmov_imm") or \
            (ins.op == "vec" and ins.f["name"] not in ("dup_gen", "ins_gen", "umov", "smov")):
        gp = set()
    if ins.op in ("fmov_gen", "cvtf", "fcvti"):
        gp = {ins.f["rn"]} if ins.op in ("cvtf",) or (ins.op == "fmov_gen" and ins.f["to_fp"]) else {ins.f["rd"]}
    if ins.op in ("vldst_multi", "vld_rep", "vldst_one"):
        gp = {ins.f["rn"]} | ({ins.f["rm"]} if ins.f.get("post") and ins.f["rm"] != 31 else set())
    if SP in gp or 18 in gp:
        return False
    if ins.op in MEM_OPS:
        f = ins.f
        if f.get("mode") in ("pre", "post") and not f.get("fp") and \
                (f.get("rt") == f["rn"] or f.get("rt2") == f["rn"]):
            return False
        if ins.op == "ldstp" and f["load"] and not f["fp"] and f["rt"] == f["rt2"]:
            return False
        if ins.op in ("ldst",) and f.get("mode") == "reg" and f.get("rm") == f["rn"]:
            return False
        if ins.op in ("ldst", "ldstp") and f["load"] and not f.get("fp") and f["rt"] == f["rn"]:
            return False
        if ins.op in ("cas", "ldop", "ldar") and f.get("rt") == f["rn"]:
            return False
        if ins.op in ("cas", "ldop") and f.get("rs") == f["rn"]:
            return False
        if ins.op == "vldst_multi" and f.get("post") and f["rm"] == f["rn"]:
            return False
    return True


NATIVE_TMPL = """
    .text
    .p2align 2
    .globl _{name}
_{name}:
    stp x29, x30, [sp, #-16]!
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp d8, d9, [sp, #80]
    stp d10, d11, [sp, #96]
    stp d12, d13, [sp, #112]
    stp d14, d15, [sp, #128]
    str x0, [sp, #144]
    ldr w1, [x0, #768]
    lsl w1, w1, #28
    msr nzcv, x1
    add x1, x0, #256
    ldp q0, q1, [x1, #0]
    ldp q2, q3, [x1, #32]
    ldp q4, q5, [x1, #64]
    ldp q6, q7, [x1, #96]
    ldp q8, q9, [x1, #128]
    ldp q10, q11, [x1, #160]
    ldp q12, q13, [x1, #192]
    ldp q14, q15, [x1, #224]
    ldp q16, q17, [x1, #256]
    ldp q18, q19, [x1, #288]
    ldp q20, q21, [x1, #320]
    ldp q22, q23, [x1, #352]
    ldp q24, q25, [x1, #384]
    ldp q26, q27, [x1, #416]
    ldp q28, q29, [x1, #448]
    ldp q30, q31, [x1, #480]
    ldp x1, x2, [x0, #8]
    ldp x3, x4, [x0, #24]
    ldp x5, x6, [x0, #40]
    ldp x7, x8, [x0, #56]
    ldp x9, x10, [x0, #72]
    ldp x11, x12, [x0, #88]
    ldp x13, x14, [x0, #104]
    ldp x15, x16, [x0, #120]
    ldr x17, [x0, #136]
    ldp x19, x20, [x0, #152]
    ldp x21, x22, [x0, #168]
    ldp x23, x24, [x0, #184]
    ldp x25, x26, [x0, #200]
    ldp x27, x28, [x0, #216]
    ldp x29, x30, [x0, #232]
    ldr x0, [x0]
{body}
    str x0, [sp, #152]
    ldr x0, [sp, #144]
    stp x1, x2, [x0, #8]
    stp x3, x4, [x0, #24]
    stp x5, x6, [x0, #40]
    stp x7, x8, [x0, #56]
    stp x9, x10, [x0, #72]
    stp x11, x12, [x0, #88]
    stp x13, x14, [x0, #104]
    stp x15, x16, [x0, #120]
    str x17, [x0, #136]
    stp x19, x20, [x0, #152]
    stp x21, x22, [x0, #168]
    stp x23, x24, [x0, #184]
    stp x25, x26, [x0, #200]
    stp x27, x28, [x0, #216]
    stp x29, x30, [x0, #232]
    ldr x1, [sp, #152]
    str x1, [x0]
    mrs x1, nzcv
    lsr w1, w1, #28
    str w1, [x0, #768]
    add x1, x0, #256
    stp q0, q1, [x1, #0]
    stp q2, q3, [x1, #32]
    stp q4, q5, [x1, #64]
    stp q6, q7, [x1, #96]
    stp q8, q9, [x1, #128]
    stp q10, q11, [x1, #160]
    stp q12, q13, [x1, #192]
    stp q14, q15, [x1, #224]
    stp q16, q17, [x1, #256]
    stp q18, q19, [x1, #288]
    stp q20, q21, [x1, #320]
    stp q22, q23, [x1, #352]
    stp q24, q25, [x1, #384]
    stp q26, q27, [x1, #416]
    stp q28, q29, [x1, #448]
    stp q30, q31, [x1, #480]
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp d8, d9, [sp, #80]
    ldp d10, d11, [sp, #96]
    ldp d12, d13, [sp, #112]
    ldp d14, d15, [sp, #128]
    add sp, sp, #176
    ldp x29, x30, [sp], #16
    ret
"""


def cset(cond, rd=0):
    """csinc wRD, wzr, wzr, invert(cond) == cset wRD, cond."""
    return 0x1A9F07E0 | ((cond ^ 1) << 12) | rd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("work")
    ap.add_argument("--per-class", type=int, default=6)
    ap.add_argument("--states", type=int, default=48)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    rnd = random.Random(args.seed)
    prog = Program(args.exe)
    os.makedirs(args.work, exist_ok=True)

    # sample instructions per class
    classes = collections.defaultdict(list)
    text = prog.text
    for a in range(text.addr, text.end, 4):
        w = prog.word(a)
        ins = decode(w, a)
        if not usable(ins):
            continue
        k = class_key(ins)
        lst = classes[k]
        if len(lst) < 64 and w not in [x[1] for x in lst]:
            lst.append((a, w))
    tests = []      # (name, words, mem, materialize_all, desc)
    for k, lst in sorted(classes.items(), key=lambda kv: str(kv[0])):
        pick = rnd.sample(lst, min(args.per_class, len(lst)))
        for a, w in pick:
            ins = decode(w, a)
            tests.append(([w], ins.op in MEM_OPS, True, "%#x %s" % (a, k)))
            fk = Func.flag_kind(None, ins)
            if fk and fk[0] != "nzcv":
                for cond in range(14):
                    tests.append(([w, cset(cond, 0 if 0 not in regs_of(ins) else 1)], False, False,
                                  "%#x %s + cset %d" % (a, k, cond)))
            elif fk:
                for cond in (0, 1, 2, 3, 8, 9, 10, 11, 12, 13):
                    tests.append(([w, cset(cond, 0 if 0 not in regs_of(ins) else 1)], False, True,
                                  "%#x %s + cset %d" % (a, k, cond)))

    print("%d classes, %d tests" % (len(classes), len(tests)))

    c_parts = ["#include <stdio.h>", "#include <stdlib.h>", "#define GA(a) ((uint8_t *)(uintptr_t)(a))",
               '#include "recomp.h"',
               "uint8_t *g_mem;",
               "GuestFn g_import_fns[1];",
               "void rt_call(Ctx *c, uint64_t a) { (void)c; (void)a; __builtin_trap(); }",
               "void rt_unimpl(Ctx *c, uint32_t pc, const char *w) { (void)c; fprintf(stderr, \"unimpl %x %s\\n\", pc, w); exit(3); }",
               "void rt_trap(Ctx *c, uint32_t pc, uint32_t i) { (void)c; (void)pc; (void)i; __builtin_trap(); }",
               "void rt_bad_jump(Ctx *c, uint32_t pc, uint64_t t) { (void)c; (void)pc; (void)t; __builtin_trap(); }",
               ""]
    asm_parts = []
    table = []
    for i, (words, mem, mat, desc) in enumerate(tests):
        start = 0x100000000 + 0x10000 + i * 64
        fn = Func(prog, start, start + 4 * (len(words) + 1), words=words + [RET])
        fn.analyze()
        if mat:
            fn.materialize = {ins.addr for ins in fn.insns if fn.flag_kind(ins)}
        lw = Lower(fn, test_mode=True)
        code = lw.run().replace("void %s(Ctx *c)" % cname(start), "static void t_%d(Ctx *c)" % i)
        c_parts.append(code)
        body = "\n".join("    .inst 0x%08x" % w for w in words)
        asm_parts.append(NATIVE_TMPL.format(name="n_%d" % i, body=body))
        ins0 = decode(words[0], start)
        f = ins0.f
        base = f.get("rn", -1) if mem else -1
        index = -1
        if mem and ins0.op == "ldst" and f.get("mode") == "reg":
            index = f["rm"]
        if mem and ins0.op in ("vldst_multi", "vld_rep", "vldst_one") and f.get("post") and f["rm"] != 31:
            index = f["rm"]
        table.append('    { t_%d, n_%d, %d, %d, %d, "%s" },' % (i, i, base, index, 1 if mat else 0, desc))
        c_parts.append("void n_%d(Ctx *c);" % i)

    c_parts.append("""
#include <stdio.h>
#include <stdlib.h>
typedef struct { void (*t)(Ctx *); void (*n)(Ctx *); int base, index, flags; const char *desc; } Test;
static const Test tests[] = {
%s
};
""" % "\n".join(table))
    c_parts.append(DRIVER)
    with open(os.path.join(args.work, "difftest.c"), "w") as f:
        f.write("\n".join(c_parts))
    with open(os.path.join(args.work, "native.s"), "w") as f:
        f.write("\n".join(asm_parts))

    exe = os.path.join(args.work, "difftest")
    cmd = ["clang", "-O1", "-std=gnu11", "-I", os.path.join(ROOT, "runtime", "include"),
           "-Wno-unused-label", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
           "-ffp-contract=off",
           os.path.join(args.work, "difftest.c"), os.path.join(args.work, "native.s"), "-o", exe]
    subprocess.check_call(cmd)
    r = subprocess.run([exe, str(args.states), str(args.seed)])
    sys.exit(r.returncode)


DRIVER = r"""
#define BUF_SIZE (192 * 1024)
#define BUF_BASE (16 * 1024)

static uint64_t rng = 88172645463325252ull;
static uint64_t rnd64(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static uint64_t interesting_x(void)
{
    static const uint64_t special[] = {
        0, 1, 2, 3, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000ull,
        0xFFFFFFFFull, 0x100000000ull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull,
        0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFF80000000ull, 31, 32, 63, 64,
    };
    switch (rnd64() % 4) {
    case 0: return special[rnd64() % (sizeof special / sizeof special[0])];
    case 1: return rnd64() % 64;
    case 2: return (uint64_t)(int64_t)(int32_t)rnd64();
    default: return rnd64();
    }
}

static uint32_t interesting_f32(void)
{
    static const float vals[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 1.5f, 2.5f, -2.5f, 3.0f, 1e-40f,
                                  1e30f, -1e30f, 3.4e38f, 0.1f, 100.25f, -7.75f, 2147483648.0f,
                                  4294967296.0f, -2147483904.0f, 1e10f };
    uint32_t u;
    switch (rnd64() % 5) {
    case 0: { float f = vals[rnd64() % (sizeof vals / sizeof vals[0])]; memcpy(&u, &f, 4); return u; }
    case 1: return 0x7F800000u | (rnd64() % 3 == 0 ? 0 : (uint32_t)(rnd64() & 0x7FFFFF)) | (uint32_t)(rnd64() & 1) << 31;
    case 2: { float f = (float)((int64_t)(rnd64() % 2001) - 1000) / 8.0f; memcpy(&u, &f, 4); return u; }
    case 3: { float f = (float)((double)(int64_t)rnd64() / 9.2e18 * 1000.0); memcpy(&u, &f, 4); return u; }
    default: return (uint32_t)rnd64();
    }
}

static uint64_t interesting_f64(void)
{
    static const double vals[] = { 0.0, -0.0, 1.0, -1.0, 0.5, 1.5, 2.5, -2.5, 1e-310, 1e300, -1e300,
                                   0.1, 9.2e18, -9.3e18, 1.8e19, 4294967296.5, -2147483648.5 };
    uint64_t u;
    switch (rnd64() % 5) {
    case 0: { double f = vals[rnd64() % (sizeof vals / sizeof vals[0])]; memcpy(&u, &f, 8); return u; }
    case 1: return 0x7FF0000000000000ull | (rnd64() % 3 == 0 ? 0 : (rnd64() & 0xFFFFFFFFFFFFFull)) | (rnd64() & 1) << 63;
    case 2: { double f = (double)((int64_t)(rnd64() % 2001) - 1000) / 8.0; memcpy(&u, &f, 8); return u; }
    case 3: { double f = (double)(int64_t)rnd64() / 9.2e18 * 1e6; memcpy(&u, &f, 8); return u; }
    default: return rnd64();
    }
}

static void random_state(Ctx *c)
{
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 31; i++) c->x[i] = interesting_x();
    for (int i = 0; i < 32; i++) {
        switch (rnd64() % 4) {
        case 0: for (int k = 0; k < 4; k++) c->v[i].s[k] = interesting_f32(); break;
        case 1: for (int k = 0; k < 2; k++) c->v[i].d[k] = interesting_f64(); break;
        case 2: for (int k = 0; k < 2; k++) c->v[i].d[k] = interesting_x(); break;
        default: for (int k = 0; k < 2; k++) c->v[i].d[k] = rnd64(); break;
        }
    }
    c->nzcv = (uint32_t)(rnd64() & 0xF);
}

/* Equal, or NaN in both at the same lane (the payload of a NaN result built
 * from two NaN operands depends on operand order, which C does not keep). */
static int nan32(uint32_t u) { return (u & 0x7F800000u) == 0x7F800000u && (u & 0x7FFFFFu); }
static int nan64(uint64_t u) { return (u & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (u & 0xFFFFFFFFFFFFFull); }
static int same_v(V128 a, V128 b)
{
    for (int k = 0; k < 2; k++) {
        if (a.d[k] == b.d[k]) continue;
        if (nan64(a.d[k]) && nan64(b.d[k])) continue;
        for (int h = 0; h < 2; h++) {
            uint32_t x = a.s[2 * k + h], y = b.s[2 * k + h];
            if (x != y && !(nan32(x) && nan32(y))) return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int nstates = argc > 1 ? atoi(argv[1]) : 32;
    rng ^= argc > 2 ? (uint64_t)atoll(argv[2]) * 0x9E3779B97F4A7C15ull : 0;
    static uint8_t buf[BUF_SIZE] __attribute__((aligned(64)));
    static uint8_t init[BUF_SIZE], after_n[BUF_SIZE];
    int ntests = (int)(sizeof tests / sizeof tests[0]);
    int failed = 0, failed_tests = 0;
    for (int t = 0; t < ntests; t++) {
        const Test *T = &tests[t];
        int bad = 0;
        for (int s = 0; s < nstates && !bad; s++) {
            Ctx a, b;
            random_state(&a);
            a.x[18] = 0;
            a.sp = 0;
            if (T->base >= 0) {
                for (int k = 0; k < BUF_SIZE; k++) init[k] = (uint8_t)rnd64();
                a.x[T->base] = (uint64_t)(uintptr_t)(buf + BUF_BASE) + (rnd64() % 8) * 16;
                if (T->index >= 0 && T->index != T->base) a.x[T->index] = rnd64() % 64;
                memcpy(buf, init, BUF_SIZE);
            }
            b = a;
            T->n(&a);
            if (T->base >= 0) {
                memcpy(after_n, buf, BUF_SIZE);
                memcpy(buf, init, BUF_SIZE);
            }
            Ctx in = b;
            T->t(&b);
            for (int i = 0; i < 31; i++) {
                if (i == 18) continue;
                if (a.x[i] != b.x[i]) {
                    printf("FAIL %s: x%d native %016llx recomp %016llx\n", T->desc, i,
                           (unsigned long long)a.x[i], (unsigned long long)b.x[i]);
                    bad = 1;
                }
            }
            for (int i = 0; i < 32; i++) {
                if (!same_v(a.v[i], b.v[i])) {
                    printf("FAIL %s: v%d native %016llx:%016llx recomp %016llx:%016llx (in v %016llx:%016llx)\n",
                           T->desc, i, (unsigned long long)a.v[i].d[1], (unsigned long long)a.v[i].d[0],
                           (unsigned long long)b.v[i].d[1], (unsigned long long)b.v[i].d[0],
                           (unsigned long long)in.v[i].d[1], (unsigned long long)in.v[i].d[0]);
                    bad = 1;
                }
            }
            if (T->flags && a.nzcv != b.nzcv) {
                printf("FAIL %s: nzcv native %x recomp %x\n", T->desc, a.nzcv, b.nzcv);
                bad = 1;
            }
            if (T->base >= 0 && memcmp(after_n, buf, BUF_SIZE) != 0) {
                for (int k = 0; k < BUF_SIZE; k++)
                    if (after_n[k] != buf[k]) {
                        printf("FAIL %s: memory at +%d native %02x recomp %02x\n", T->desc, k - BUF_BASE,
                               after_n[k], buf[k]);
                        break;
                    }
                bad = 1;
            }
            if (bad) {
                printf("  inputs:");
                for (int i = 0; i < 8; i++) printf(" x%d=%llx", i, (unsigned long long)in.x[i]);
                printf(" nzcv=%x\n", in.nzcv);
            }
        }
        if (bad) failed_tests++;
        failed += bad;
    }
    printf("%d tests, %d failed\n", ntests, failed_tests);
    return failed_tests ? 1 : 0;
}
"""


if __name__ == "__main__":
    main()
