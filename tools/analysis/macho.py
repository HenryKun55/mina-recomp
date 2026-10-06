"""Minimal Mach-O reader for the game's executable.

Reads the arm64 slice of a universal binary: segments and sections, the
symbol table, the imports behind every stub and GOT slot (dyld bind opcodes),
the rebase list (every pointer that the loader slides), function starts and
the entry point. Only what the recompiler and the loader need.
"""

import struct
from dataclasses import dataclass, field

FAT_MAGIC = 0xCAFEBABE
MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C
CPU_TYPE_X86_64 = 0x01000007

LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xB
LC_LOAD_DYLIB = 0xC
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_DYLD_INFO_ONLY = 0x80000022
LC_FUNCTION_STARTS = 0x26
LC_MAIN = 0x80000028

S_NON_LAZY_SYMBOL_POINTERS = 0x6
S_LAZY_SYMBOL_POINTERS = 0x7
S_SYMBOL_STUBS = 0x8
S_ZEROFILL = 0x1
S_THREAD_LOCAL_ZEROFILL = 0x12

INDIRECT_SYMBOL_LOCAL = 0x80000000
INDIRECT_SYMBOL_ABS = 0x40000000


@dataclass
class Section:
    segname: str
    sectname: str
    addr: int
    size: int
    offset: int
    flags: int
    reserved1: int
    reserved2: int

    @property
    def type(self):
        return self.flags & 0xFF

    @property
    def zerofill(self):
        return self.type in (S_ZEROFILL, S_THREAD_LOCAL_ZEROFILL)

    @property
    def end(self):
        return self.addr + self.size


@dataclass
class Segment:
    name: str
    vmaddr: int
    vmsize: int
    fileoff: int
    filesize: int
    maxprot: int
    initprot: int
    sections: list = field(default_factory=list)


@dataclass
class Symbol:
    name: str
    type: int
    sect: int
    desc: int
    value: int


@dataclass
class Bind:
    addr: int       # slot that receives the import's address
    name: str
    dylib: int      # 1-based ordinal into MachO.dylibs (0 = self, negatives = special)
    addend: int
    weak: bool
    lazy: bool


def _uleb(data, i):
    result = shift = 0
    while True:
        b = data[i]
        i += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if b < 0x80:
            return result, i


def _sleb(data, i):
    result = shift = 0
    while True:
        b = data[i]
        i += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if b < 0x80:
            if b & 0x40:
                result -= 1 << shift
            return result, i


def _cstr(data, i):
    j = data.index(b"\0", i)
    return data[i:j].decode("utf-8", "replace"), j + 1


class MachO:
    def __init__(self, path, arch=CPU_TYPE_ARM64):
        with open(path, "rb") as f:
            blob = f.read()
        self.data = self._thin(blob, arch)
        self.segments = []
        self.sections = []
        self.symbols = []
        self.dylibs = []
        self.binds = []
        self.rebases = []
        self.function_starts = []
        self.entry = None
        self._parse()

    @staticmethod
    def _thin(blob, arch):
        magic = struct.unpack_from(">I", blob)[0]
        if magic != FAT_MAGIC:
            return blob
        n = struct.unpack_from(">I", blob, 4)[0]
        for k in range(n):
            cputype, _, off, size, _ = struct.unpack_from(">iiIII", blob, 8 + 20 * k)
            if cputype & 0xFFFFFFFF == arch:
                return blob[off:off + size]
        raise ValueError("architecture not found in universal binary")

    # -- parsing -----------------------------------------------------------

    def _parse(self):
        d = self.data
        magic, cputype, _, filetype, ncmds, _, flags, _ = struct.unpack_from("<IiiIIIII", d)
        if magic != MH_MAGIC_64:
            raise ValueError("not a 64-bit Mach-O")
        self.cputype = cputype & 0xFFFFFFFF
        off = 32
        symtab = dysymtab = dyld_info = fstarts = None
        main_off = None
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<II", d, off)
            if cmd == LC_SEGMENT_64:
                self._segment(off)
            elif cmd == LC_SYMTAB:
                symtab = struct.unpack_from("<IIII", d, off + 8)
            elif cmd == LC_DYSYMTAB:
                dysymtab = struct.unpack_from("<18I", d, off + 8)
            elif cmd in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB):
                name_off = struct.unpack_from("<I", d, off + 8)[0]
                name, _ = _cstr(d, off + name_off)
                self.dylibs.append(name)
            elif cmd == LC_DYLD_INFO_ONLY:
                dyld_info = struct.unpack_from("<10I", d, off + 8)
            elif cmd == LC_FUNCTION_STARTS:
                fstarts = struct.unpack_from("<II", d, off + 8)
            elif cmd == LC_MAIN:
                main_off = struct.unpack_from("<Q", d, off + 8)[0]
            off += cmdsize

        text = self.segment("__TEXT")
        self.base = text.vmaddr
        if main_off is not None:
            self.entry = text.vmaddr + main_off - text.fileoff
        if symtab:
            self._symtab(*symtab)
        if fstarts:
            self._function_starts(*fstarts)
        if dyld_info:
            rebase_off, rebase_size, bind_off, bind_size, wbind_off, wbind_size, \
                lbind_off, lbind_size, _, _ = dyld_info
            self._rebase(rebase_off, rebase_size)
            self._bind(bind_off, bind_size, lazy=False, weak=False)
            self._bind(wbind_off, wbind_size, lazy=False, weak=True)
            self._bind(lbind_off, lbind_size, lazy=True, weak=False)
        self.indirect = []
        if dysymtab and symtab:
            indirectsymoff, nindirect = dysymtab[12], dysymtab[13]
            self.indirect = list(struct.unpack_from("<%dI" % nindirect, d, indirectsymoff))

    def _segment(self, off):
        d = self.data
        segname = d[off + 8:off + 24].rstrip(b"\0").decode()
        vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, _ = \
            struct.unpack_from("<QQQQiiII", d, off + 24)
        seg = Segment(segname, vmaddr, vmsize, fileoff, filesize, maxprot, initprot)
        so = off + 72
        for _ in range(nsects):
            sectname = d[so:so + 16].rstrip(b"\0").decode()
            sseg = d[so + 16:so + 32].rstrip(b"\0").decode()
            addr, size, offset, _, _, _, flags, r1, r2, _ = \
                struct.unpack_from("<QQIIIIIIII", d, so + 32)
            s = Section(sseg, sectname, addr, size, offset, flags, r1, r2)
            seg.sections.append(s)
            self.sections.append(s)
            so += 80
        self.segments.append(seg)

    def _symtab(self, symoff, nsyms, stroff, strsize):
        d = self.data
        for k in range(nsyms):
            strx, ntype, sect, desc, value = struct.unpack_from("<IBBhQ", d, symoff + 16 * k)
            name, _ = _cstr(d, stroff + strx)
            self.symbols.append(Symbol(name, ntype, sect, desc, value))

    def _function_starts(self, off, size):
        d = self.data
        i, end = off, off + size
        addr = self.base
        while i < end:
            delta, i = _uleb(d, i)
            if delta == 0:
                break
            addr += delta
            self.function_starts.append(addr)

    def _seg_addr(self, segidx, segoff):
        return self.segments[segidx].vmaddr + segoff

    def _rebase(self, off, size):
        d = self.data
        i, end = off, off + size
        seg = segoff = 0
        out = self.rebases
        while i < end:
            b = d[i]
            i += 1
            op, imm = b & 0xF0, b & 0x0F
            if op == 0x00:          # DONE
                break
            elif op == 0x10:        # SET_TYPE_IMM
                pass
            elif op == 0x20:        # SET_SEGMENT_AND_OFFSET_ULEB
                seg = imm
                segoff, i = _uleb(d, i)
            elif op == 0x30:        # ADD_ADDR_ULEB
                v, i = _uleb(d, i)
                segoff += v
            elif op == 0x40:        # ADD_ADDR_IMM_SCALED
                segoff += imm * 8
            elif op == 0x50:        # DO_REBASE_IMM_TIMES
                for _ in range(imm):
                    out.append(self._seg_addr(seg, segoff))
                    segoff += 8
            elif op == 0x60:        # DO_REBASE_ULEB_TIMES
                n, i = _uleb(d, i)
                for _ in range(n):
                    out.append(self._seg_addr(seg, segoff))
                    segoff += 8
            elif op == 0x70:        # DO_REBASE_ADD_ADDR_ULEB
                out.append(self._seg_addr(seg, segoff))
                v, i = _uleb(d, i)
                segoff += 8 + v
            elif op == 0x80:        # DO_REBASE_ULEB_TIMES_SKIPPING_ULEB
                n, i = _uleb(d, i)
                skip, i = _uleb(d, i)
                for _ in range(n):
                    out.append(self._seg_addr(seg, segoff))
                    segoff += 8 + skip
            else:
                raise ValueError("bad rebase opcode %#x" % b)

    def _bind(self, off, size, lazy, weak):
        d = self.data
        i, end = off, off + size
        seg = segoff = addend = 0
        dylib = 0
        name = ""
        out = self.binds

        def emit():
            out.append(Bind(self._seg_addr(seg, segoff), name, dylib, addend, weak, lazy))

        while i < end:
            b = d[i]
            i += 1
            op, imm = b & 0xF0, b & 0x0F
            if op == 0x00:          # DONE (lazy streams keep going past it)
                if not lazy:
                    break
            elif op == 0x10:        # SET_DYLIB_ORDINAL_IMM
                dylib = imm
            elif op == 0x20:        # SET_DYLIB_ORDINAL_ULEB
                dylib, i = _uleb(d, i)
            elif op == 0x30:        # SET_DYLIB_SPECIAL_IMM
                dylib = (imm | 0xF0) - 0x100 if imm else 0
            elif op == 0x40:        # SET_SYMBOL_TRAILING_FLAGS_IMM
                name, i = _cstr(d, i)
            elif op == 0x50:        # SET_TYPE_IMM
                pass
            elif op == 0x60:        # SET_ADDEND_SLEB
                addend, i = _sleb(d, i)
            elif op == 0x70:        # SET_SEGMENT_AND_OFFSET_ULEB
                seg = imm
                segoff, i = _uleb(d, i)
            elif op == 0x80:        # ADD_ADDR_ULEB
                v, i = _uleb(d, i)
                segoff = (segoff + v) & 0xFFFFFFFFFFFFFFFF
            elif op == 0x90:        # DO_BIND
                emit()
                segoff += 8
            elif op == 0xA0:        # DO_BIND_ADD_ADDR_ULEB
                emit()
                v, i = _uleb(d, i)
                segoff = (segoff + 8 + v) & 0xFFFFFFFFFFFFFFFF
            elif op == 0xB0:        # DO_BIND_ADD_ADDR_IMM_SCALED
                emit()
                segoff += 8 + imm * 8
            elif op == 0xC0:        # DO_BIND_ULEB_TIMES_SKIPPING_ULEB
                n, i = _uleb(d, i)
                skip, i = _uleb(d, i)
                for _ in range(n):
                    emit()
                    segoff += 8 + skip
            else:
                raise ValueError("bad bind opcode %#x" % b)

    # -- queries -----------------------------------------------------------

    def segment(self, name):
        for s in self.segments:
            if s.name == name:
                return s
        return None

    def section(self, segname, sectname):
        for s in self.sections:
            if s.segname == segname and s.sectname == sectname:
                return s
        return None

    def section_at(self, addr):
        for s in self.sections:
            if s.addr <= addr < s.end:
                return s
        return None

    def read(self, addr, size):
        """Bytes of the image at a virtual address (zeros for zerofill)."""
        for seg in self.segments:
            if seg.vmaddr <= addr < seg.vmaddr + seg.vmsize:
                off = addr - seg.vmaddr
                if off >= seg.filesize:
                    return bytes(size)
                start = seg.fileoff + off
                chunk = self.data[start:start + min(size, seg.filesize - off)]
                return chunk + bytes(size - len(chunk))
        raise ValueError("address %#x not mapped" % addr)

    def u64(self, addr):
        return struct.unpack("<Q", self.read(addr, 8))[0]

    def cstring(self, addr, limit=4096):
        raw = self.read(addr, limit)
        return raw.split(b"\0", 1)[0].decode("utf-8", "replace")

    def stub_imports(self):
        """Map of stub / GOT / lazy pointer address -> imported symbol name."""
        out = {}
        for s in self.sections:
            t = s.type
            if t == S_SYMBOL_STUBS:
                stride = s.reserved2
            elif t in (S_NON_LAZY_SYMBOL_POINTERS, S_LAZY_SYMBOL_POINTERS):
                stride = 8
            else:
                continue
            for k in range(s.size // stride):
                idx = self.indirect[s.reserved1 + k]
                if idx & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS):
                    continue
                out[s.addr + k * stride] = self.symbols[idx].name
        return out
