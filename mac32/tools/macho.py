"""Load the original i386 Mach-O: sections, functions (symbols and LC_FUNCTION_STARTS), import stubs."""
import bisect
import lief

INDIRECT_LOCAL = 0x80000000


class Image:
    def __init__(self, path):
        b = self.bin = lief.parse(path)
        self.segments = [(s.name, s.virtual_address, s.virtual_size, s.file_offset, s.file_size)
                         for s in b.segments if s.virtual_size and s.name != "__PAGEZERO"
                         and s.name != "__LINKEDIT"]
        self.raw = open(path, "rb").read()
        sec = {s.name: s for s in b.sections}
        self.text = sec["__text"]
        ind = b.dynamic_symbol_command
        # raw indirect symbol indices (lief resolves names; locals have no symbol)
        off = ind.indirect_symbol_offset
        import struct
        nidx = ind.nb_indirect_symbols
        raw_idx = struct.unpack_from("<%dI" % nidx, self.raw, off)
        syms = list(b.symbols)
        def name_of(i):
            v = raw_idx[i]
            if v & (INDIRECT_LOCAL | 0x40000000):
                return None
            return syms[v].name

        # classic external relocations: pointer slots dyld fills with symbol address + stored addend
        self.extrel = []
        for k in range(ind.nb_external_relocations):
            ra, info = struct.unpack_from("<iI", self.raw, ind.external_relocation_offset + 8 * k)
            if ((info >> 25) & 3) == 2 and (info >> 27) & 1:
                self.extrel.append((ra & 0xFFFFFFFF, syms[info & 0xFFFFFF].name))
        self.stubs = {}     # stub addr -> import name
        s = sec["__symbol_stub"]
        for k in range(s.size // s.reserved2):
            self.stubs[s.virtual_address + k * s.reserved2] = name_of(s.reserved1 + k)
        self.ptrs = {}      # pointer-slot addr -> import name (None = local)
        for nm in ("__nl_symbol_ptr", "__la_symbol_ptr"):
            s = sec[nm]
            for k in range(s.size // 4):
                self.ptrs[s.virtual_address + 4 * k] = name_of(s.reserved1 + k)

        # A stub or a pointer slot may name a symbol the image defines itself: C++ functions and variables that
        # every image may carry a copy of (inline functions, templates, guard variables) are reached through them
        # so that dyld can pick one copy. Here there is one image, so the copy is its own: such a stub is the
        # function, and such a slot keeps what the file holds, which is the definition's address.
        import subprocess
        defined = {}
        for line in subprocess.run(["nm", "-n", path], capture_output=True, text=True, env={"LC_ALL": "C"}).stdout.splitlines():
            p = line.split(" ", 2)
            if len(p) == 3 and p[1] not in "Uu": defined.setdefault(p[2], int(p[0], 16))
        self.stub_alias = {a: defined[n] for a, n in self.stubs.items() if n in defined}
        for a in self.stub_alias: del self.stubs[a]
        for a, n in self.ptrs.items():
            if n in defined: self.ptrs[a] = None

        # LC_DYLD_INFO binds (executables linked for 10.6 and later have these and no classic relocations): pointer
        # slots anywhere in the data that dyld fills with symbol address + addend. The slots of the two pointer
        # sections are in self.ptrs already; the rest are C++ vtable and type_info pointers, constant CFStrings
        # and function pointers in tables.
        self.binds = [(a, n, add) for a, n, add in self.dyld_binds() if a not in self.ptrs]

        # Functions: the entries of LC_FUNCTION_STARTS, which the linker writes whether or not the symbols were
        # kept (a stripped executable has few symbols and all of these), and every text symbol. The table is a list
        # of ULEB128 distances, the first from the start of __TEXT and each of the rest from the entry before it.
        lo, hi = self.text.virtual_address, self.text.virtual_address + self.text.size
        starts = []
        fs = b.function_starts if b.has_function_starts else None
        if fs:
            a, p, end = self.text.segment.virtual_address, fs.data_offset, fs.data_offset + fs.data_size
            while p < end:
                d = shift = 0
                while True:
                    c = self.raw[p]; p += 1
                    d |= (c & 0x7F) << shift; shift += 7
                    if not c & 0x80: break
                if not d: break
                a += d
                starts.append(a)
        # Code is __text. A program may keep functions in a section of its own too (tcmalloc's entry points are in
        # one called google_malloc, with no instruction flags, no symbols and no entries in LC_FUNCTION_STARTS):
        # the translator finds those as the targets of calls and adds them with add_function().
        self.code = [(lo, hi)]
        self.other_text = sorted((x.virtual_address, x.virtual_address + x.size) for x in b.sections
                                 if x.segment.name == "__TEXT" and x.size and str(x.type).endswith("REGULAR")
                                 and x.name not in ("__text", "__stub_helper", "__const", "__gcc_except_tab",
                                                    "__unwind_info", "__eh_frame", "__ustring"))
        def code(a): return any(l <= a < h for l, h in self.code)
        import subprocess
        fn = {}
        out = subprocess.run(["nm", "-n", path], capture_output=True, text=True,
                             env={"LC_ALL": "C"}).stdout
        for line in out.splitlines():
            p = line.split(" ", 2)
            if len(p) == 3 and p[1] in ("T", "t", "S", "s"):
                v = int(p[0], 16)
                if code(v) and v not in fn and (p[1] in "Tt" or not lo <= v < hi):
                    fn[v] = p[2]
        for a in starts:
            if code(a) and a not in fn:
                fn[a] = "sub_%x" % a
        self.func_unnamed = {a for a, n in fn.items() if n == "sub_%x" % a}
        self.func_addrs = sorted(fn)
        self.func_names = fn
        self.name_to_addr = {v: k for k, v in fn.items()}
        self.text_end = hi

    def dyld_binds(self):
        """(address, symbol, addend) of every non-lazy bind in LC_DYLD_INFO's opcode stream."""
        import struct
        raw, segs, bind = self.raw, [], None
        ncmds = struct.unpack_from("<I", raw, 16)[0]
        o = 28
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", raw, o)
            if cmd == 1: segs.append(struct.unpack_from("<I", raw, o + 24)[0])          # LC_SEGMENT: vmaddr
            if cmd & 0x7FFFFFFF == 0x22: bind = struct.unpack_from("<II", raw, o + 16)  # bind_off, bind_size
            o += size
        if not bind or not bind[1]: return []
        p, end = bind[0], bind[0] + bind[1]
        def uleb():
            nonlocal p
            v = shift = 0
            while True:
                c = raw[p]; p += 1
                v |= (c & 0x7F) << shift; shift += 7
                if not c & 0x80: return v
        def sleb():
            nonlocal p
            v = shift = 0
            while True:
                c = raw[p]; p += 1
                v |= (c & 0x7F) << shift; shift += 7
                if not c & 0x80: return v - (1 << shift) if c & 0x40 else v
        out, addr, name, addend, kind = [], 0, None, 0, 1
        def do():
            nonlocal addr
            if kind == 1 and name: out.append((addr & 0xFFFFFFFF, name, addend))
            addr += 4
        while p < end:
            op, imm = raw[p] & 0xF0, raw[p] & 0x0F
            p += 1
            if op == 0x00: continue                       # DONE: also the padding at the end
            elif op == 0x10 or op == 0x30: pass           # dylib ordinal: names are looked up flat
            elif op == 0x20: uleb()
            elif op == 0x40:
                e = raw.index(b"\0", p); name = raw[p:e].decode(); p = e + 1
            elif op == 0x50: kind = imm
            elif op == 0x60: addend = sleb()
            elif op == 0x70: addr = segs[imm] + uleb()
            elif op == 0x80: addr += uleb()
            elif op == 0x90: do()
            elif op == 0xA0: do(); addr += uleb()
            elif op == 0xB0: do(); addr += imm * 4
            elif op == 0xC0:
                count, skip = uleb(), uleb()
                for _ in range(count): do(); addr += skip
            else: raise ValueError("bind opcode 0x%x" % (op | imm))
        return out

    def read(self, addr, n):
        for name, va, vs, fo, fs in self.segments:
            if va <= addr < va + vs:
                o = addr - va
                if o >= fs:
                    return b"\0" * n
                return self.raw[fo + o: fo + o + n]
        raise KeyError(hex(addr))

    def add_function(self, addr):
        """A function found by the translator in a section of __TEXT other than __text. False if it is not one."""
        if addr in self.func_names: return False
        sec = next(((l, h) for l, h in self.other_text if l <= addr < h), None)
        if not sec: return False
        if sec not in self.code: self.code.append(sec)
        bisect.insort(self.func_addrs, addr)
        self.func_names[addr] = "sub_%x" % addr
        self.func_unnamed.add(addr)
        return True

    def func_end(self, addr):
        i = bisect.bisect_right(self.func_addrs, addr)
        ends = [h for l, h in self.code if l <= addr < h]   # the last function of a section ends with it
        if i < len(self.func_addrs): ends.append(self.func_addrs[i])
        return min(ends) if ends else self.text_end

    def func_containing(self, addr):
        i = bisect.bisect_right(self.func_addrs, addr) - 1
        return self.func_addrs[i] if i >= 0 else None
