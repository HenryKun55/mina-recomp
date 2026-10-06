#!/usr/bin/env python3
"""
Symbolizes a Vita3K guest profile (the patched fork's speed mode,
VITA3K_PROFILE=<file>, written by tools/run_vita3k.sh) against the Vita ELF,
with the recompiled functions (f_<offset>) named after the game's symbols.

usage: tools/vita/profile.py [-b baseline.txt] [-t thread] [profile.txt] [top]
   -b  subtracts an earlier copy of the (cumulative) profile, to count only
       what happened after it was taken
Weights are guest instructions at Vita speed (plus billed HLE time): shares
of CPU time on the device.
"""
import bisect
import os
import re
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VITASDK = os.environ.get("VITASDK", os.path.expanduser("~/vitasdk"))
ELF = os.path.join(ROOT, "build-vita", "mina")
MACHO = os.path.join(ROOT, "gamedata", "app", "Contents", "MacOS", "MinaTheHollower")

args = sys.argv[1:]
baseline = None
only_thread = None
while args[:1] in (["-b"], ["-t"]):
    if args[0] == "-b":
        baseline = args[1]
    else:
        only_thread = args[1]          # -t ID: that thread only
    args = args[2:]
prof = args[0] if args else os.path.join(ROOT, "build-vita", "vita3k-profile.txt")
top = int(args[1]) if len(args) > 1 else 40


def nm(cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout.splitlines()


# game symbols: original address -> demangled name
game = {}
for line in nm(["nm", "-arch", "arm64", "-n", MACHO]):
    p = line.split(" ", 2)
    if len(p) == 3 and p[0] and p[1] in ("t", "T"):
        game[int(p[0], 16)] = p[2]
names = sorted(game)


def game_name(off):
    a = 0x100000000 + off
    i = bisect.bisect_right(names, a) - 1
    return game[names[i]] if i >= 0 else "?"


# ELF symbols
syms = []
for line in nm([os.path.join(VITASDK, "bin", "arm-vita-eabi-nm"), "-n", ELF]):
    p = line.split(" ", 2)
    if len(p) == 3 and p[0] and p[1] in ("t", "T", "w", "W"):
        syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]


def sym(pc):
    i = bisect.bisect_right(addrs, pc & ~1) - 1
    if i < 0:
        return "?%x" % pc
    s = syms[i][1]
    m = re.match(r"f_([0-9a-f]+)(_orig)?$", s)
    return ("g:" + game_name(int(m.group(1), 16))) if m else s


def load(path):
    rows = {}
    for line in open(path):
        t, pc, lr, w = line.split()
        rows[(t, int(pc, 16), int(lr, 16))] = int(w)
    return rows


rows = load(prof)
if baseline:
    for k, w0 in load(baseline).items():
        if k in rows:
            rows[k] -= w0

self_w, thread_w, caller_w = defaultdict(int), defaultdict(int), defaultdict(int)
total = 0
for (t, pc, lr), w in rows.items():
    if w <= 0 or (only_thread and t != only_thread):
        continue
    total += w
    thread_w[t] += w
    f = sym(pc)
    self_w[f] += w
    caller_w[(f, sym(lr))] += w

# demangle the names shown
shown = [f for f, _ in sorted(self_w.items(), key=lambda x: -x[1])[:top]]
shown += [x for (f, c), _ in sorted(caller_w.items(), key=lambda x: -x[1])[:top] for x in (f, c)]
raw = [s[2:] if s.startswith("g:") else s for s in shown]
dem = dict(zip(shown, subprocess.run(["c++filt", "-_"], input="\n".join(raw), capture_output=True,
                                     text=True).stdout.splitlines()))


def nice(f):
    return ("game " if f.startswith("g:") else "") + dem.get(f, f)


print("total weight %s guest instructions" % format(total, ","))
print("\n== threads ==")
for t, w in sorted(thread_w.items(), key=lambda x: -x[1])[:10]:
    print("%6.2f%%  thread %s" % (100 * w / total, t))
print("\n== top %d functions (self) ==" % top)
for f, w in sorted(self_w.items(), key=lambda x: -x[1])[:top]:
    print("%6.2f%%  %s" % (100 * w / total, nice(f)[:120]))
print("\n== top %d function <- caller ==" % top)
for (f, c), w in sorted(caller_w.items(), key=lambda x: -x[1])[:top]:
    print("%6.2f%%  %s  <-  %s" % (100 * w / total, nice(f)[:60], nice(c)[:60]))
