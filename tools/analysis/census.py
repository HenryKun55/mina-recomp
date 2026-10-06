"""Instruction census of the executable's arm64 code.

Disassembles every function (bounded by LC_FUNCTION_STARTS) and counts the
mnemonics, so the recompiler's coverage can be planned and tracked.

    python3 tools/analysis/census.py gamedata/app/Contents/MacOS/MinaTheHollower
"""

import collections
import os
import sys

import capstone

sys.path.insert(0, os.path.dirname(__file__))
from macho import MachO  # noqa: E402


def main():
    m = MachO(sys.argv[1])
    text = m.section("__TEXT", "__text")
    code = m.read(text.addr, text.size)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.skipdata = True

    counts = collections.Counter()
    funcs_with = collections.defaultdict(set)
    bad = 0
    starts = sorted(a for a in m.function_starts if text.addr <= a < text.end)
    bounds = starts[1:] + [text.end]
    for start, end in zip(starts, bounds):
        off = start - text.addr
        for insn in md.disasm_lite(code[off:end - text.addr], start):
            mnem = insn[2]
            if mnem == ".byte":
                bad += 1
                continue
            counts[mnem] += 1
            funcs_with[mnem].add(start)

    total = sum(counts.values())
    print("functions %d, instructions %d, undecodable words %d, distinct mnemonics %d"
          % (len(starts), total, bad, len(counts)))
    for mnem, n in counts.most_common():
        print("%-12s %9d  %6.3f%%  in %d funcs" % (mnem, n, 100.0 * n / total, len(funcs_with[mnem])))


if __name__ == "__main__":
    main()
