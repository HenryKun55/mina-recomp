"""Decode every instruction of the executable and report what the decoder
does not know yet (grouped by Capstone's mnemonic), plus the op histogram.

    python3 tools/recomp/decode_check.py gamedata/app/Contents/MacOS/MinaTheHollower
"""

import collections
import os
import struct
import sys

import capstone

HERE = os.path.dirname(__file__)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "analysis"))
from a64 import decode  # noqa: E402
from macho import MachO  # noqa: E402


def main():
    m = MachO(sys.argv[1])
    text = m.section("__TEXT", "__text")
    code = m.read(text.addr, text.size)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    ops = collections.Counter()
    unk = collections.Counter()
    sample = {}
    for off in range(0, len(code), 4):
        w = struct.unpack_from("<I", code, off)[0]
        a = text.addr + off
        ins = decode(w, a)
        key = ins.op if ins.op != "vec" else "vec:" + ins.name
        ops[key] += 1
        if ins.op == "unk":
            dis = list(md.disasm_lite(code[off:off + 4], a))
            mn = dis[0][2] + " " + dis[0][3] if dis else ".word"
            k = mn.split()[0]
            unk[k] += 1
            sample.setdefault(k, (a, mn))
    print("unknown: %d" % sum(unk.values()))
    for k, n in unk.most_common():
        print("  %-10s %7d  e.g. %#x %s" % (k, n, *sample[k]))
    print()
    for k, n in ops.most_common():
        print("%-22s %d" % (k, n))


if __name__ == "__main__":
    main()
