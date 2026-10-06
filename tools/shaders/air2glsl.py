"""Translate the game's Metal shaders (AIR bitcode in metallibs) to GLSL.

    python3 tools/shaders/air2glsl.py <shaders.pak.yc> <out.c> [--dump DIR]

Every metallib in the pak is disassembled with llvm-dis (Homebrew LLVM) and
its single entry point translated to GLSL 3.30. The output is a C table of
{FNV-1a 64 of the metallib, stage, GLSL} that runtime/src/metal.c looks up
in newLibraryWithData:.

Conventions shared with the runtime:
  uniform vec4 vcb<N>[K] / fcb<N>[K]     constant buffer at Metal index N
  uniform sampler2D vtex<T>_s<S> / ftex<T>_s<S>   texture T sampled with sampler S
  uniform sampler2D ftex<T>_r             texture T read with texelFetch
  uniform sampler2DMS ftex<T>_ms          multisampled texture T
  in a<L>                                 vertex attribute L
  v_<NAME>                                varyings, from user(NAME)
  o<K>                                    render target K
Vertex positions are flipped in y and remapped from [0,1] to [-1,1] in z.
"""

import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------
# types


class T:
    def __init__(self, kind, elem=None, n=0, fields=None, name=None, addrspace=0):
        self.kind, self.elem, self.n, self.fields, self.name, self.addrspace = kind, elem, n, fields, name, addrspace

    def __repr__(self):
        return "T(%s,%s,%s)" % (self.kind, self.elem, self.n)


SCALARS = {"float": ("f", 4), "half": ("h", 2), "double": ("d", 8), "i1": ("b", 1), "i8": ("i", 1),
           "i16": ("i", 2), "i32": ("i", 4), "i64": ("i", 8)}


def split_top(s, sep=","):
    out, depth, cur, q = [], 0, "", False
    for ch in s:
        if ch == '"':
            q = not q
        if not q:
            if ch in "<{[(":
                depth += 1
            elif ch in ">}])":
                depth -= 1
            elif ch == sep and depth == 0:
                out.append(cur.strip())
                cur = ""
                continue
        cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


class Types:
    def __init__(self):
        self.structs = {}

    def parse(self, s):
        """Parse a type at the start of s; returns (type, rest)."""
        s = s.lstrip()
        m = re.match(r"(float|half|double|i1|i8|i16|i32|i64|void)\b", s)
        if m:
            return T("s", m.group(1)), s[m.end():]
        if s.startswith("ptr"):
            m = re.match(r"ptr( addrspace\((\d+)\))?", s)
            return T("ptr", addrspace=int(m.group(2) or 0)), s[m.end():]
        if s.startswith("<{") or s.startswith("{"):
            packed = s.startswith("<{")
            close = "}>" if packed else "}"
            depth, i = 0, 0
            while True:
                ch = s[i]
                if ch in "<{[(":
                    depth += 1
                elif ch in ">}])":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            inner = s[2:i - 1] if packed else s[1:i]
            fields = [self.parse(x)[0] for x in split_top(inner)] if inner.strip() else []
            return T("struct", fields=fields, name="packed" if packed else None), s[i + 1:]
        if s.startswith("<"):
            m = re.match(r"<(\d+) x ", s)
            el, rest = self.parse(s[m.end():])
            rest = rest.lstrip()
            assert rest.startswith(">"), rest
            return T("v", el, int(m.group(1))), rest[1:]
        if s.startswith("["):
            m = re.match(r"\[(\d+) x ", s)
            el, rest = self.parse(s[m.end():])
            rest = rest.lstrip()
            assert rest.startswith("]"), rest
            return T("a", el, int(m.group(1))), rest[1:]
        m = re.match(r'%("[^"]+"|[\w.]+)', s)
        if m:
            return T("named", name=m.group(1)), s[m.end():]
        raise ValueError("type? " + s[:60])

    def resolve(self, t):
        while t.kind == "named":
            t = self.structs[t.name]
        return t

    def size_align(self, t):
        t = self.resolve(t)
        if t.kind == "s":
            sz = SCALARS[t.elem][1]
            return sz, sz
        if t.kind == "v":
            es, _ = self.size_align(t.elem)
            raw = es * t.n
            al = 1
            while al < raw:
                al *= 2
            return al, al          # vectors: size rounded to a power of two (v96 -> 16)
        if t.kind == "a":
            es, ea = self.size_align(t.elem)
            return es * t.n, ea
        if t.kind == "struct":
            off, al = 0, 1
            for f in t.fields:
                fs, fa = self.size_align(f)
                if t.name != "packed":
                    off = (off + fa - 1) // fa * fa
                    al = max(al, fa)
                off += fs
            if t.name != "packed":
                off = (off + al - 1) // al * al
            return off, al
        if t.kind == "ptr":
            return 8, 8
        raise ValueError(t)

    def field_offset(self, t, i):
        t = self.resolve(t)
        off = 0
        for k, f in enumerate(t.fields):
            fs, fa = self.size_align(f)
            if t.name != "packed":
                off = (off + fa - 1) // fa * fa
            if k == i:
                return off, f
            off += fs
        raise IndexError


def glsl_type(t, types):
    t = types.resolve(t)
    if t.kind == "s":
        return {"float": "float", "half": "float", "double": "float", "i1": "bool", "i8": "int", "i16": "int",
                "i32": "int", "i64": "int"}[t.elem]
    if t.kind == "v":
        e = types.resolve(t.elem).elem
        pre = {"float": "", "half": "", "double": "", "i1": "b", "i8": "i", "i16": "i", "i32": "i", "i64": "i"}[e]
        if t.n == 1:
            return glsl_type(t.elem, types)
        return "%svec%d" % (pre, t.n)
    raise ValueError("no GLSL type for %r" % (t,))


def is_int(t, types):
    t = types.resolve(t)
    if t.kind == "v":
        t = types.resolve(t.elem)
    return t.kind == "s" and t.elem.startswith("i") and t.elem != "i1"


def is_bool(t, types):
    t = types.resolve(t)
    if t.kind == "v":
        t = types.resolve(t.elem)
    return t.kind == "s" and t.elem == "i1"


def nlanes(t, types):
    t = types.resolve(t)
    return t.n if t.kind == "v" else 1


SW = "xyzw"


def fconst(s):
    s = s.strip()
    if s.startswith("0x"):
        h = s[2:]
        if s.startswith("0xH"):
            raise ValueError("half hex constant")
        v = struct.unpack(">d", bytes.fromhex(h.rjust(16, "0")))[0]
    else:
        v = float(s)
    r = repr(float(v))
    if r == "inf":
        return "(1.0/0.0)"
    if r == "-inf":
        return "(-1.0/0.0)"
    if r == "nan":
        return "(0.0/0.0)"
    if "e" in r and "." not in r.split("e")[0]:
        r = r.replace("e", ".0e")
    return r


# ---------------------------------------------------------------------------
# metadata


def parse_metadata(text):
    md = {}
    for m in re.finditer(r"^!(\d+) = (?:distinct )?!\{(.*)\}$", text, re.M):
        md[int(m.group(1))] = split_top(m.group(2))
    named = {}
    for m in re.finditer(r"^!([\w.]+) = !\{(.*)\}$", text, re.M):
        named[m.group(1)] = split_top(m.group(2))
    return md, named


def md_strings(items):
    out = []
    for x in items:
        x = x.strip()
        if x.startswith('!"'):
            out.append(x[2:-1])
        elif x.startswith("i32 "):
            out.append(int(x[4:]))
        else:
            out.append(x)
    return out


# ---------------------------------------------------------------------------
# translation


class Shader:
    def __init__(self, ll):
        self.ll = ll
        self.types = Types()
        for m in re.finditer(r'^%("[^"]+"|[\w.]+) = type (.*)$', ll, re.M):
            self.types.structs[m.group(1)] = self.types.parse(m.group(2))[0]
        self.md, self.named = parse_metadata(ll)
        if "air.vertex" in self.named:
            self.stage = 0
            entry = self.named["air.vertex"]
        elif "air.fragment" in self.named:
            self.stage = 1
            entry = self.named["air.fragment"]
        else:
            raise ValueError("no entry point")
        node = self.md[int(entry[0][1:])]
        self.outputs = [md_strings(self.md[int(x[1:])]) for x in self.md[int(node[1][1:])]]
        self.inputs = [md_strings(self.md[int(x[1:])]) for x in self.md[int(node[2][1:])]]
        self.pre = "v" if self.stage == 0 else "f"
        self.parse_function()

    # -- function ----------------------------------------------------------------

    def parse_function(self):
        m = re.search(r"^define (.*?)@(\w+)\((.*)\) [^{]*\{$", self.ll, re.M)
        self.ret_type = self.types.parse(re.sub(r"^(internal |dso_local |local_unnamed_addr )+", "", m.group(1)).strip())[0]
        params = split_top(m.group(3))
        self.params = []
        for p in params:
            t, rest = self.types.parse(p)
            name = re.findall(r"%(\d+)", rest)[-1]
            self.params.append((t, name))
        body = self.ll[m.end():self.ll.index("\n}\n", m.end())]
        self.blocks = []        # (label, [lines])
        cur = (self.entry_label(), [])
        body = re.sub(r"\[\s*\n((?:\s+i\d+ -?\d+, label %\d+\n)*)\s*\]", lambda m: "[ " + " ".join(m.group(1).split()) + " ]", body)
        for line in body.split("\n"):
            line = line.split(";")[0].rstrip() if not line.lstrip().startswith(";") else ""
            if not line.strip():
                continue
            lm = re.match(r"^(\d+):", line)
            if lm:
                self.blocks.append(cur)
                cur = (lm.group(1), [])
                continue
            cur[1].append(line.strip())
        self.blocks.append(cur)
        self.blocks = [b for b in self.blocks if b[1]]

    def entry_label(self):
        return str(len(self.params))

    # -- values ------------------------------------------------------------------

    def setup_params(self):
        self.vals = {}          # %name -> (glsl expr, type) or ('ptr', ...)
        self.decls = []
        self.textures = {}      # param name -> (location, kind)
        self.samplers = {}      # param name -> location
        self.uniforms = []
        self.ins = []
        self.outs = []
        for spec in self.inputs:
            idx = spec[0]
            kind = spec[1]
            t, pname = self.params[idx]
            if kind == "air.vertex_input":
                loc = spec[spec.index("air.location_index") + 1]
                gt = glsl_type(t, self.types)
                self.ins.append("layout(location = %d) in %s a%d;" % (loc, gt, loc))
                self.vals[pname] = ("a%d" % loc, t)
            elif kind == "air.buffer":
                loc = spec[spec.index("air.location_index") + 1]
                size = spec[spec.index("air.buffer_size") + 1] if "air.buffer_size" in spec else 256
                n = max(1, (size + 15) // 16)
                self.uniforms.append("uniform vec4 %scb%d[%d];" % (self.pre, loc, n))
                self.vals[pname] = ("ptr", "%scb%d" % (self.pre, loc), 0)
            elif kind == "air.texture":
                loc = spec[spec.index("air.location_index") + 1]
                tn = spec[spec.index("air.arg_type_name") + 1]
                self.textures[pname] = (loc, tn)
            elif kind == "air.sampler":
                loc = spec[spec.index("air.location_index") + 1]
                self.samplers[pname] = loc
            elif kind == "air.position":
                self.vals[pname] = ("gl_FragCoord", t)
            elif kind == "air.fragment_input":
                nm = re.match(r"user\((\w+)\)", spec[2]).group(1)
                gt = glsl_type(t, self.types)
                q = ""
                if "air.flat" in spec:
                    q = "flat "
                elif "air.no_perspective" in spec:
                    q = "noperspective "
                if is_int(t, self.types):
                    q = "flat "
                self.ins.append("%sin %s v_%s;" % (q, gt, nm))
                self.vals[pname] = ("v_%s" % nm, t)
            elif kind == "air.vertex_id":
                self.vals[pname] = ("gl_VertexID", t)
            elif kind == "air.instance_id":
                self.vals[pname] = ("gl_InstanceID", t)
            elif kind == "air.front_facing":
                self.vals[pname] = ("gl_FrontFacing", t)
            elif kind == "air.sample_id":
                self.vals[pname] = ("gl_SampleID", t)
            elif kind == "air.point_coord":
                self.vals[pname] = ("gl_PointCoord", t)
            else:
                raise ValueError("input kind %s" % kind)
        self.tex_uniforms = {}  # (tex loc, sampler loc or 'r' / 'ms') -> uniform name

    def tex_uniform(self, tparam, sparam=None, mode=None):
        loc, tn = self.textures[tparam]
        if tn.startswith("texture2d_ms") or tn.startswith("depth2d_ms"):
            key = (loc, "ms")
            name = "%stex%d_ms" % (self.pre, loc)
            decl = "uniform sampler2DMS %s;" % name
        elif sparam is not None:
            s = self.samplers[sparam]
            key = (loc, s)
            name = "%stex%d_s%d" % (self.pre, loc, s)
            kind = "samplerCube" if tn.startswith("texturecube") else "sampler2D"
            decl = "uniform %s %s;" % (kind, name)
        else:
            # read / size query: reuse any uniform of this texture
            for (l, s), n in self.tex_uniforms.items():
                if l == loc:
                    return n
            key = (loc, "r")
            name = "%stex%d_r" % (self.pre, loc)
            decl = "uniform sampler2D %s;" % name
        if key not in self.tex_uniforms:
            self.tex_uniforms[key] = name
            self.uniforms.append(decl)
        return self.tex_uniforms[key]

    def value(self, tok, t):
        """GLSL expression of an operand of type t."""
        tok = tok.strip()
        if tok.startswith("%"):
            v = self.vals[tok[1:]]
            return v[0]
        return self.const(tok, t)

    def const(self, tok, t):
        t = self.types.resolve(t)
        tok = tok.strip()
        if tok in ("undef", "poison", "zeroinitializer"):
            if t.kind == "v":
                return "%s(0)" % glsl_type(t, self.types)
            return {"float": "0.0", "half": "0.0", "double": "0.0", "i1": "false"}.get(t.elem, "0")
        if tok in ("true", "false"):
            return tok
        if tok.startswith("splat ("):
            inner = tok[len("splat ("):-1]
            et, rest = self.types.parse(inner)
            return "%s(%s)" % (glsl_type(t, self.types), self.const(rest, et))
        if tok.startswith("<"):
            items = split_top(tok[1:-1])
            vals = []
            for it in items:
                et, rest = self.types.parse(it)
                vals.append(self.const(rest, et))
            return "%s(%s)" % (glsl_type(t, self.types), ", ".join(vals))
        if t.kind == "s" and t.elem in ("float", "half", "double"):
            return fconst(tok)
        if t.kind == "s" and t.elem == "i1":
            return "true" if tok in ("1", "true") else "false"
        return str(int(tok))

    def tmp(self, name, t, expr):
        gt = glsl_type(t, self.types)
        var = "t%s" % name
        self.decls.append("%s %s;" % (gt, var))
        self.emit("%s = %s;" % (var, expr))
        self.vals[name] = (var, t)

    def emit(self, s):
        self.code.append("    " * self.indent + s)

    # -- cbuffer loads ------------------------------------------------------------

    def cb_load(self, arr, off, t):
        t = self.types.resolve(t)
        n = nlanes(t, self.types)
        es = 4
        comps = []
        for k in range(n):
            o = off + k * es
            comps.append("%s[%d].%s" % (arr, o // 16, SW[(o % 16) // 4]))
        if n > 1 and (off % 16) // 4 + n <= 4:
            expr = "%s[%d].%s" % (arr, off // 16, SW[(off % 16) // 4:(off % 16) // 4 + n])
        elif n == 1:
            expr = comps[0]
        else:
            expr = "vec%d(%s)" % (n, ", ".join(comps))
        if is_int(t, self.types):
            expr = "floatBitsToInt(%s)" % expr
        return expr

    def gep(self, line):
        m = re.match(r"(?:inbounds |nuw |nusw )*(.*)$", line)
        parts = split_top(m.group(1))
        base_t, _ = self.types.parse(parts[0])
        ptr_tok = parts[1].split()[-1]
        base = self.vals[ptr_tok[1:]]
        if base[0] != "ptr":
            raise ValueError("gep on non-buffer pointer")
        off = base[2]
        cur = base_t
        first = True
        for p in parts[2:]:
            it, rest = self.types.parse(p)
            rest = rest.strip()
            if not re.match(r"^-?\d+$", rest):
                raise ValueError("dynamic gep index")
            i = int(rest)
            if first:
                off += i * self.types.size_align(cur)[0]
                first = False
                continue
            c = self.types.resolve(cur)
            if c.kind == "struct":
                fo, ft = self.types.field_offset(c, i)
                off += fo
                cur = ft
            elif c.kind in ("a", "v"):
                off += i * self.types.size_align(c.elem)[0]
                cur = c.elem
            else:
                raise ValueError("gep into scalar")
        return ("ptr", base[1], off)

    # -- instructions ---------------------------------------------------------------

    BIN = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/", "add": "+", "sub": "-", "mul": "*",
           "sdiv": "/", "udiv": "/", "and": "&", "or": "|", "xor": "^", "shl": "<<", "lshr": ">>", "ashr": ">>"}
    FCMP = {"oeq": "==", "ueq": "==", "one": "!=", "une": "!=", "olt": "<", "ult": "<", "ole": "<=", "ule": "<=",
            "ogt": ">", "ugt": ">", "oge": ">=", "uge": ">="}
    VCMP = {"==": "equal", "!=": "notEqual", "<": "lessThan", "<=": "lessThanEqual", ">": "greaterThan",
            ">=": "greaterThanEqual"}
    ICMP = {"eq": "==", "ne": "!=", "slt": "<", "sle": "<=", "sgt": ">", "sge": ">=", "ult": "<", "ule": "<=",
            "ugt": ">", "uge": ">="}

    def instr(self, line, block):
        m = re.match(r"^%(\d+) = (.*)$", line)
        if m:
            name, rest = m.group(1), m.group(2)
        else:
            name, rest = None, line
        rest = re.sub(r",\s*![\w.]+ !\d+", "", rest)                # metadata attachments
        rest = re.sub(r"(tail |musttail |notail )?call ", "call ", rest)
        op = rest.split()[0]
        args = rest[len(op):].strip()
        flags = []
        while True:
            fm = re.match(r"^(fast|nnan|ninf|nsz|arcp|contract|afn|reassoc|nuw|nsw|exact|disjoint|inbounds)\s+", args)
            if not fm:
                break
            flags.append(fm.group(1))
            args = args[fm.end():]

        if op in self.BIN:
            t, rest2 = self.types.parse(args)
            a, b = split_top(rest2)
            ea, eb = self.value(a, t), self.value(b, t)
            if op in ("and", "or", "xor") and is_bool(t, self.types):
                sym = {"and": "&&", "or": "||", "xor": "^^"}[op]
                if nlanes(t, self.types) > 1:
                    fn = {"and": "and_", "or": "or_", "xor": "notEqual"}[op]
                    if op == "xor":
                        self.tmp(name, t, "notEqual(%s, %s)" % (ea, eb))
                    else:
                        conv = glsl_type(t, self.types).replace("b", "")
                        self.tmp(name, t, "%s(%s(%s) %s %s(%s))" % (glsl_type(t, self.types), conv, ea,
                                                                    "*" if op == "and" else "+", conv, eb))
                    return
                self.tmp(name, t, "(%s %s %s)" % (ea, sym, eb))
                return
            self.tmp(name, t, "(%s %s %s)" % (ea, self.BIN[op], eb))
            return
        if op == "frem":
            t, rest2 = self.types.parse(args)
            a, b = split_top(rest2)
            ea, eb = self.value(a, t), self.value(b, t)
            self.tmp(name, t, "(%s - %s * trunc(%s / %s))" % (ea, eb, ea, eb))
            return
        if op == "fneg":
            t, rest2 = self.types.parse(args)
            self.tmp(name, t, "(-%s)" % self.value(rest2, t))
            return
        if op in ("fcmp", "icmp"):
            pred, args2 = args.split(None, 1)
            t, rest2 = self.types.parse(args2)
            a, b = split_top(rest2)
            ea, eb = self.value(a, t), self.value(b, t)
            n = nlanes(t, self.types)
            rt = T("v", T("s", "i1"), n) if n > 1 else T("s", "i1")
            if pred in ("true", "false"):
                self.tmp(name, rt, pred if n == 1 else "bvec%d(%s)" % (n, pred))
                return
            if pred == "ord":
                self.tmp(name, rt, "!(isnan(%s) || isnan(%s))" % (ea, eb) if n == 1 else
                         "not(%s)" % "bvec%d(ivec%d(isnan(%s)) + ivec%d(isnan(%s)))" % (n, n, ea, n, eb))
                return
            if pred == "uno":
                self.tmp(name, rt, "(isnan(%s) || isnan(%s))" % (ea, eb) if n == 1 else
                         "bvec%d(ivec%d(isnan(%s)) + ivec%d(isnan(%s)))" % (n, n, ea, n, eb))
                return
            sym = (self.FCMP if op == "fcmp" else self.ICMP)[pred]
            if n == 1:
                self.tmp(name, rt, "(%s %s %s)" % (ea, sym, eb))
            else:
                self.tmp(name, rt, "%s(%s, %s)" % (self.VCMP[sym], ea, eb))
            return
        if op == "select":
            parts = split_top(args)
            ct, c = self.types.parse(parts[0])
            t, a = self.types.parse(parts[1])
            _, b = self.types.parse(parts[2])
            ec, ea, eb = self.value(c, ct), self.value(a, t), self.value(b, t)
            if nlanes(ct, self.types) > 1:
                self.tmp(name, t, "mix(%s, %s, %s)" % (eb, ea, ec))
            else:
                self.tmp(name, t, "(%s ? %s : %s)" % (ec, ea, eb))
            return
        if op == "extractelement":
            parts = split_top(args)
            vt, v = self.types.parse(parts[0])
            it, i = self.types.parse(parts[1])
            ev = self.value(v, vt)
            i = i.strip()
            et = self.types.resolve(vt).elem
            if re.match(r"^\d+$", i):
                self.tmp(name, et, "%s.%s" % (self.paren(ev), SW[int(i)]))
            else:
                self.tmp(name, et, "%s[%s]" % (self.paren(ev), self.value(i, it)))
            return
        if op == "insertelement":
            parts = split_top(args)
            vt, v = self.types.parse(parts[0])
            st, s = self.types.parse(parts[1])
            it, i = self.types.parse(parts[2])
            self.tmp(name, vt, self.value(v, vt))
            var = self.vals[name][0]
            i = i.strip()
            if re.match(r"^\d+$", i):
                self.emit("%s.%s = %s;" % (var, SW[int(i)], self.value(s, st)))
            else:
                self.emit("%s[%s] = %s;" % (var, self.value(i, it), self.value(s, st)))
            return
        if op == "shufflevector":
            parts = split_top(args)
            at, a = self.types.parse(parts[0])
            bt, b = self.types.parse(parts[1])
            mt, mask = self.types.parse(parts[2])
            na = nlanes(at, self.types)
            mask = mask.strip()
            n = nlanes(mt, self.types)
            if mask in ("zeroinitializer",):
                idx = [0] * n
            elif mask.startswith("splat"):
                idx = [int(re.search(r"(\d+)\)", mask).group(1))] * n
            else:
                idx = []
                for it in split_top(mask[1:-1]):
                    v = it.split()[-1]
                    idx.append(None if v in ("undef", "poison") else int(v))
            ea, eb = self.paren(self.value(a, at)), self.paren(self.value(b, bt)) if b.strip() not in ("undef", "poison") else None
            et = self.types.resolve(at).elem
            rt = T("v", et, n) if n > 1 else et
            zero = self.const("zeroinitializer", et)
            comps = []
            for k in idx:
                if k is None:
                    comps.append(zero)
                elif k < na:
                    comps.append("%s.%s" % (ea, SW[k]) if na > 1 else ea)
                else:
                    comps.append("%s.%s" % (eb, SW[k - na]) if na > 1 else eb)
            # one swizzle when all come from a
            if all(k is not None and k < na for k in idx) and na > 1:
                expr = "%s.%s" % (ea, "".join(SW[k] for k in idx))
            else:
                expr = "%s(%s)" % (glsl_type(rt, self.types), ", ".join(comps)) if n > 1 else comps[0]
            self.tmp(name, rt, expr)
            return
        if op == "getelementptr":
            self.vals[name] = self.gep(args)
            return
        if op == "load":
            parts = split_top(args)
            t, _ = self.types.parse(parts[0])
            p = parts[1].split()[-1]
            ptr = self.vals[p[1:]]
            if ptr[0] != "ptr":
                raise ValueError("load from non-buffer pointer")
            self.tmp(name, t, self.cb_load(ptr[1], ptr[2], t))
            return
        if op in ("sitofp", "uitofp", "fptosi", "fptoui", "fpext", "fptrunc", "zext", "sext", "trunc", "bitcast"):
            m2 = re.match(r"(.*) to (.*)$", args)
            st, v = self.types.parse(m2.group(1))
            dt = self.types.parse(m2.group(2))[0]
            ev = self.value(v, st)
            gt = glsl_type(dt, self.types)
            if op == "bitcast":
                if is_int(dt, self.types) and not is_int(st, self.types):
                    ev = "floatBitsToInt(%s)" % ev
                elif not is_int(dt, self.types) and is_int(st, self.types):
                    ev = "intBitsToFloat(%s)" % ev
                self.tmp(name, dt, ev)
            elif op == "zext" and is_bool(st, self.types):
                self.tmp(name, dt, "%s(%s)" % (gt, ev))
            else:
                self.tmp(name, dt, "%s(%s)" % (gt, ev))
            return
        if op == "insertvalue":
            parts = split_top(args)
            at, agg = self.types.parse(parts[0])
            vt, v = self.types.parse(parts[1])
            idx = int(parts[2])
            prev = self.vals.get(agg.strip()[1:], ("agg", {}))[1] if agg.strip().startswith("%") else {}
            fields = dict(prev)
            fields[idx] = (self.value(v, vt), vt)
            self.vals[name] = ("agg", fields)
            return
        if op == "extractvalue":
            parts = split_top(args)
            at, agg = self.types.parse(parts[0])
            idx = int(parts[1])
            e, t = self.vals[agg.strip()[1:]][1][idx]
            self.tmp(name, t, e)
            return
        if op == "phi":
            t, rest2 = self.types.parse(args)
            var = "t%s" % name
            self.decls.append("%s %s;" % (glsl_type(t, self.types), var))
            self.vals[name] = (var, t)
            # assignments happen on the incoming edges (see edge())
            for inc in split_top(rest2):
                mm = re.match(r"\[\s*(.*),\s*%(\d+)\s*\]", inc)
                self.phis.setdefault(mm.group(2), {}).setdefault(block, []).append((var, mm.group(1), t))
            return
        if op == "call":
            self.call(name, args)
            return
        if op == "br":
            m3 = re.match(r"i1 (.*), label %(\d+), label %(\d+)", args)
            if m3:
                c = self.value(m3.group(1), T("s", "i1"))
                self.emit("if (%s) {" % c)
                self.indent += 1
                self.edge(block, m3.group(2))
                self.indent -= 1
                self.emit("} else {")
                self.indent += 1
                self.edge(block, m3.group(3))
                self.indent -= 1
                self.emit("}")
            else:
                self.edge(block, re.match(r"label %(\d+)", args).group(1))
            return
        if op == "ret":
            self.ret(args)
            return
        if op == "switch":
            m3 = re.match(r"(i\d+) (%?\w+), label %(\d+) \[(.*)\]", args)
            st = T("s", m3.group(1))
            v = self.value(m3.group(2), st)
            cases = re.findall(r"i\d+ (-?\d+), label %(\d+)", m3.group(4))
            for k, (val, lab) in enumerate(cases):
                self.emit("%sif (%s == %s) {" % ("" if k == 0 else "} else ", v, val))
                self.indent += 1
                self.edge(block, lab)
                self.indent -= 1
            self.emit("} else {" if cases else "{")
            self.indent += 1
            self.edge(block, m3.group(3))
            self.indent -= 1
            self.emit("}")
            return
        if op == "unreachable":
            return
        raise ValueError("instruction %s: %s" % (op, line))

    def paren(self, e):
        return e if re.match(r"^[\w.\[\]]+$", e) else "(%s)" % e

    def edge(self, src, dst):
        for var, val, t in self.phis.get(src, {}).get(dst, []):
            self.emit("%s = %s;" % (var, self.value(val, t)))
        self.emit(self.goto(dst))

    INTR = {
        "air.fast_pow": "pow", "air.pow": "pow", "air.dot": "dot", "air.fast_dot": "dot", "air.mix": "mix",
        "air.fast_mix": "mix", "air.fast_clamp": "clamp", "air.clamp": "clamp", "air.fast_fmax": "max",
        "air.fmax": "max", "air.fast_fmin": "min", "air.fmin": "min", "air.fast_floor": "floor",
        "air.floor": "floor", "air.fast_ceil": "ceil", "air.ceil": "ceil", "air.fast_sin": "sin", "air.sin": "sin",
        "air.fast_cos": "cos", "air.cos": "cos", "air.dfdx": "dFdx", "air.dfdy": "dFdy", "air.fwidth": "fwidth",
        "air.fast_fabs": "abs", "air.fabs": "abs", "air.sign": "sign", "air.fast_rsqrt": "inversesqrt",
        "air.rsqrt": "inversesqrt", "air.fast_sqrt": "sqrt", "air.sqrt": "sqrt", "air.fast_exp2": "exp2",
        "air.exp2": "exp2", "air.fast_log2": "log2", "air.log2": "log2", "air.fast_fract": "fract",
        "air.fract": "fract", "air.fast_trunc": "trunc", "air.trunc": "trunc", "air.fast_round": "round",
        "air.fast_step": "step", "air.step": "step", "air.smoothstep": "smoothstep",
        "air.fast_smoothstep": "smoothstep", "air.fast_atan2": "atan", "air.fast_length": "length",
        "air.length": "length", "air.fast_normalize": "normalize", "air.normalize": "normalize",
        "air.fast_exp": "exp", "air.fast_log": "log", "air.min": "min", "air.max": "max",
        "air.fast_tan": "tan", "air.fast_atan": "atan", "air.fast_asin": "asin", "air.fast_acos": "acos",
        "llvm.fabs": "abs", "llvm.floor": "floor", "llvm.ceil": "ceil", "llvm.sqrt": "sqrt",
        "llvm.minnum": "min", "llvm.maxnum": "max", "llvm.trunc": "trunc", "llvm.fma": "fma_",
        "air.fma": "fma_", "air.fast_fma": "fma_",
    }

    def call(self, name, args):
        m = re.match(r"^(?:fast )?(.*?)@([\w.]+)\((.*)\)\s*(#\d+)?$", args)
        rt = self.types.parse(m.group(1).strip())[0]
        fn = m.group(2)
        argl = split_top(m.group(3))
        targs = []
        for a in argl:
            t, rest = self.types.parse(a)
            rest = re.sub(r"^(\s*(readonly|captures\(none\)|nocapture|noundef|dereferenceable\(\d+\)))+", "", rest).strip()
            targs.append((t, rest))
        base = re.sub(r"\.(v\d+)?[fi]\d+$", "", fn)
        base = re.sub(r"\.(v\d+)?[fi]\d+$", "", base)
        if fn == "air.discard_fragment":
            self.emit("discard;")
            return
        if base == "air.sample_texture_2d" or base == "air.sample_texture_cube":
            u = self.tex_uniform(targs[0][1][1:], targs[1][1][1:])
            self.tmp(name, rt, "texture(%s, %s)" % (u, self.value(targs[2][1], targs[2][0])))
            return
        if base == "air.read_texture_2d":
            u = self.tex_uniform(targs[0][1][1:])
            self.tmp(name, rt, "texelFetch(%s, %s, %s)" % (u, self.value(targs[1][1], targs[1][0]),
                                                          self.value(targs[2][1], targs[2][0])))
            return
        if base == "air.read_texture_2d_ms":
            u = self.tex_uniform(targs[0][1][1:])
            self.tmp(name, rt, "texelFetch(%s, %s, %s)" % (u, self.value(targs[1][1], targs[1][0]),
                                                          self.value(targs[2][1], targs[2][0])))
            return
        if base in ("air.get_width_texture_2d", "air.get_height_texture_2d", "air.get_width_texture_2d_ms",
                    "air.get_height_texture_2d_ms"):
            u = self.tex_uniform(targs[0][1][1:])
            lod = ", %s" % self.value(targs[1][1], targs[1][0]) if "_ms" not in base else ""
            self.tmp(name, rt, "textureSize(%s%s).%s" % (u, lod, "x" if "width" in base else "y"))
            return
        if base == "air.get_num_samples_texture_2d_ms":
            self.tmp(name, rt, "4")
            return
        if fn.startswith("air.convert."):
            gt = glsl_type(rt, self.types)
            self.tmp(name, rt, "%s(%s)" % (gt, self.value(targs[0][1], targs[0][0])))
            return
        if base in ("air.fast_saturate", "air.saturate"):
            self.tmp(name, rt, "clamp(%s, 0.0, 1.0)" % self.value(targs[0][1], targs[0][0]))
            return
        if base in ("air.fast_fmod", "air.fmod"):
            a, b = (self.value(x[1], x[0]) for x in targs)
            self.tmp(name, rt, "(%s - %s * trunc(%s / %s))" % (a, b, a, b))
            return
        g = self.INTR.get(base)
        if not g:
            raise ValueError("intrinsic %s" % fn)
        vals = [self.value(x[1], x[0]) for x in targs]
        if g == "fma_":
            self.tmp(name, rt, "(%s * %s + %s)" % tuple(vals))
            return
        self.tmp(name, rt, "%s(%s)" % (g, ", ".join(vals)))

    def ret(self, args):
        args = args.strip()
        if args == "void":
            self.emit("return;")
            return
        t, v = self.types.parse(args)
        v = v.strip()
        tr = self.types.resolve(t)
        if tr.kind == "struct":
            fields = self.vals[v[1:]][1] if v.startswith("%") else {}
        else:
            fields = {0: (self.value(v, t), t)}
        for i, spec in enumerate(self.outputs):
            if i not in fields:
                continue
            e, ft = fields[i]
            kind = spec[0]
            if kind == "air.position":
                self.emit("gl_Position = flip_pos(%s);" % e)
            elif kind == "air.vertex_output":
                nm = re.match(r"user\((\w+)\)", spec[1]).group(1)
                self.emit("v_%s = %s;" % (nm, e))
            elif kind == "air.point_size":
                self.emit("gl_PointSize = %s;" % e)
            elif kind == "air.render_target":
                self.emit("o%d = %s;" % (spec[1], self.to_vec4(e, ft)))
            elif kind == "air.depth":
                self.emit("gl_FragDepth = %s;" % e)
            elif kind == "air.sample_mask":
                pass
            else:
                raise ValueError("output %s" % kind)
        self.emit("return;")

    def to_vec4(self, e, t):
        n = nlanes(t, self.types)
        if n == 4:
            return e
        pad = {1: "%s, 0.0, 0.0, 1.0", 2: "%s, 0.0, 1.0", 3: "%s, 1.0"}[n]
        return "vec4(%s)" % (pad % e)

    def declare_outputs(self):
        for spec in self.outputs:
            kind = spec[0]
            if kind == "air.vertex_output":
                nm = re.match(r"user\((\w+)\)", spec[1]).group(1)
                tn = spec[spec.index("air.arg_type_name") + 1]
                gt = {"float": "float", "float2": "vec2", "float3": "vec3", "float4": "vec4", "half": "float",
                      "half2": "vec2", "half3": "vec3", "half4": "vec4", "int": "int", "uint": "int",
                      "int2": "ivec2", "uint2": "ivec2"}.get(tn, "vec4")
                q = "flat " if gt.startswith("i") or gt == "int" else ""
                self.outs.append("%sout %s v_%s;" % (q, gt, nm))
            elif kind == "air.render_target":
                self.outs.append("layout(location = %d) out vec4 o%d;" % (spec[1], spec[1]))

    # -- control flow ------------------------------------------------------------------

    def translate(self):
        self.setup_params()
        self.declare_outputs()
        self.phis = {}
        self.code = []
        self.indent = 1
        labels = [b[0] for b in self.blocks]
        order = {l: i for i, l in enumerate(labels)}
        loops = False
        for i, (l, lines) in enumerate(self.blocks):
            for ln in lines:
                for tgt in re.findall(r"label %(\d+)", ln):
                    if order[tgt] <= i:
                        loops = True
        # phis first (they reference later blocks)
        for l, lines in self.blocks:
            for ln in lines:
                if " = phi " in ln:
                    pass
        if len(self.blocks) == 1:
            self.goto = lambda dst: ""
            for ln in self.blocks[0][1]:
                self.instr(ln, self.blocks[0][0])
        elif not loops:
            # acyclic: blocks in order, each guarded by a reached-flag
            for l in labels[1:]:
                self.decls.append("bool r%s = false;" % l)
            self.goto = lambda dst: "r%s = true;" % dst
            # phi declarations must exist before the edges assign them: pre-scan
            self.prescan_phis()
            for i, (l, lines) in enumerate(self.blocks):
                if i:
                    self.emit("if (r%s) {" % l)
                    self.indent += 1
                for ln in lines:
                    if " = phi " in ln:
                        continue
                    self.instr(ln, l)
                if i:
                    self.indent -= 1
                    self.emit("}")
        else:
            self.decls.append("int bb = %s;" % labels[0])
            self.goto = lambda dst: "bb = %s; continue;" % dst
            self.prescan_phis()
            self.emit("for (;;) {")
            self.indent += 1
            for i, (l, lines) in enumerate(self.blocks):
                self.emit("%sif (bb == %s) {" % ("" if i == 0 else "else ", l))
                self.indent += 1
                for ln in lines:
                    if " = phi " in ln:
                        continue
                    self.instr(ln, l)
                self.indent -= 1
                self.emit("}")
            self.indent -= 1
            self.emit("}")
        return self.render()

    def prescan_phis(self):
        for l, lines in self.blocks:
            for ln in lines:
                if " = phi " in ln:
                    self.instr(ln, l)

    def render(self):
        out = ["#version 330 core"]
        if self.stage == 0:
            out.append("vec4 flip_pos(vec4 p) { return vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w); }")
        out += self.uniforms + self.ins + self.outs
        out.append("void main() {")
        seen = set()
        for d in self.decls:
            if d not in seen:
                out.append("    " + d)
                seen.add(d)
        out += self.code
        out.append("}")
        return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------


def find_llvm_dis():
    for c in sorted(glob.glob("/opt/homebrew/opt/llvm*/bin/llvm-dis"), reverse=True) + \
            [shutil.which("llvm-dis") or ""]:
        if c and os.path.exists(c):
            return c
    sys.exit("llvm-dis not found (brew install llvm)")


def metallibs(pak):
    d = open(pak, "rb").read()
    out = []
    for m in re.finditer(re.escape(b"MTLB"), d):
        o = m.start()
        size = struct.unpack_from("<Q", d, o + 16)[0]
        if 0 < size < len(d) - o + 1:
            out.append(d[o:o + size])
    return out


def fnv64(b):
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def c_string(s):
    return "\n".join('    "%s\\n"' % l.replace("\\", "\\\\").replace('"', '\\"') for l in s.rstrip("\n").split("\n"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pak")
    ap.add_argument("out")
    ap.add_argument("--dump")
    args = ap.parse_args()
    dis = find_llvm_dis()
    libs = metallibs(args.pak)
    entries = []
    errors = 0
    with tempfile.TemporaryDirectory() as tmp:
        for i, lib in enumerate(libs):
            m = re.search(re.escape(b"\xde\xc0\x17\x0b"), lib)
            o = m.start()
            _, _, off, size, _ = struct.unpack_from("<5I", lib, o)
            bc = os.path.join(tmp, "s%d.bc" % i)
            with open(bc, "wb") as f:
                f.write(lib[o:o + off + size])
            ll = subprocess.run([dis, bc, "-o", "-"], capture_output=True, text=True, check=True).stdout
            try:
                sh = Shader(ll)
                glsl = sh.translate()
            except Exception as ex:      # keep going: report every failure
                errors += 1
                print("shader %d: %s" % (i, ex), file=sys.stderr)
                continue
            entries.append((fnv64(lib), sh.stage, glsl, i))
            if args.dump:
                os.makedirs(args.dump, exist_ok=True)
                with open(os.path.join(args.dump, "s%03d.%s" % (i, "vert" if sh.stage == 0 else "frag")), "w") as f:
                    f.write(glsl)
    lines = ["/* generated by tools/shaders/air2glsl.py */", '#include "metal_gl.h"', "",
             "const ShaderSrc g_shaders[] = {"]
    for h, stage, glsl, i in entries:
        lines.append("    /* metallib %d */ { 0x%016xull, %d,\n%s }," % (i, h, stage, c_string(glsl)))
    lines.append("};")
    lines.append("const int g_shader_count = %d;" % len(entries))
    with open(args.out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("%d shaders translated, %d errors" % (len(entries), errors))
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
