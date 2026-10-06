"""Call graph queries over the executable.

    callgraph.py EXE callees NAME [--depth N]    direct calls (bl/b to functions and imports)
    callgraph.py EXE callers NAME                functions that call NAME
    callgraph.py EXE find SUBSTRING              functions whose (demangled) name matches
    callgraph.py EXE imports-of NAME [--depth N] imports reached from NAME

NAME may be a mangled name, a demangled prefix or a hex address.
"""

import argparse
import os
import pickle
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "recomp"))


def load(exe):
    cache = os.path.join(HERE, "..", "..", "out", "callgraph.pickle")
    st = os.stat(exe)
    if os.path.exists(cache):
        with open(cache, "rb") as f:
            data = pickle.load(f)
        if data["mtime"] == st.st_mtime and data.get("v") == 2:
            return data
    from gen import Program
    from a64 import decode
    p = Program(exe)
    calls = {}
    for a, e in p.funcs:
        out = []
        for x in range(a, e, 4):
            ins = decode(p.word(x), x)
            if ins.op in ("bl", "b") and not (a <= ins.target < e):
                t = ins.target
                if t in p.stub_import:
                    out.append(("imp", p.import_names[p.stub_import[t]]))
                elif t in p.func_starts:
                    out.append(("fn", t))
            elif ins.op in ("blr",):
                out.append(("ind", 0))
        calls[a] = out
    names = dict(p.names)
    mangled = sorted(set(names.values()))
    dem = subprocess.run(["c++filt"], input="\n".join(n[1:] if n.startswith("__Z") else n for n in mangled),
                         capture_output=True, text=True).stdout.split("\n")
    demangled = dict(zip(mangled, dem))
    data = {"mtime": st.st_mtime, "v": 2, "calls": calls, "names": names, "dem": demangled}
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    with open(cache, "wb") as f:
        pickle.dump(data, f)
    return data


def pretty(data, a):
    n = data["names"].get(a)
    if not n:
        return "sub_%x" % a
    return data["dem"].get(n, n)


def resolve(data, name):
    try:
        a = int(name, 16)
        if a in data["calls"]:
            return [a]
    except ValueError:
        pass
    out = [a for a, n in data["names"].items() if n == name or n == "_" + name]
    if out:
        return out
    return [a for a, n in data["names"].items() if data["dem"].get(n, n).startswith(name)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("cmd")
    ap.add_argument("name")
    ap.add_argument("--depth", type=int, default=1)
    args = ap.parse_args()
    data = load(args.exe)
    if args.cmd == "find":
        for a, n in sorted(data["names"].items()):
            d = data["dem"].get(n, n)
            if args.name in d or args.name in n:
                print("%#x %s" % (a, d))
        return
    roots = resolve(data, args.name)
    if not roots:
        print("not found")
        return
    if args.cmd == "callers":
        for r in roots:
            print("== %s" % pretty(data, r))
            for a, out in sorted(data["calls"].items()):
                if ("fn", r) in out:
                    print("  %#x %s" % (a, pretty(data, a)))
        return
    if args.cmd == "callees":
        seen = set()

        def walk(a, depth, indent):
            for kind, t in dict.fromkeys(data["calls"].get(a, [])):
                if kind == "fn":
                    print("%s%s" % (indent, pretty(data, t)))
                    if depth > 1 and t not in seen:
                        seen.add(t)
                        walk(t, depth - 1, indent + "  ")
                elif kind == "imp":
                    print("%s[import] %s" % (indent, t))
                else:
                    print("%s[indirect]" % indent)
        for r in roots:
            print("== %#x %s" % (r, pretty(data, r)))
            walk(r, args.depth, "  ")
        return
    if args.cmd == "imports-of":
        seen, imps = set(), {}
        stack = [(r, 0) for r in roots]
        while stack:
            a, d = stack.pop()
            if a in seen or d > args.depth:
                continue
            seen.add(a)
            for kind, t in data["calls"].get(a, []):
                if kind == "fn":
                    stack.append((t, d + 1))
                elif kind == "imp":
                    imps.setdefault(t, pretty(data, a))
        for t, via in sorted(imps.items()):
            print("%-40s via %s" % (t, via))


if __name__ == "__main__":
    main()
