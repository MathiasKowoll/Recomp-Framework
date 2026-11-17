"""Load the original Win64 PE: sections, imports (IAT), relocations, functions from .pdata."""
import bisect
import struct
import lief


class PE:
    def __init__(self, path, base=None):
        """base: load (and translate) the image at this address instead of its preferred base. Every address
        is derived from self.base, and relocated slots in the image get the difference added (as the
        Windows loader would), so two DLLs with the same preferred base can coexist."""
        b = self.bin = lief.parse(path)
        self.path = path
        self.preferred_base = b.optional_header.imagebase
        self.base = base or self.preferred_base
        self.delta = self.base - self.preferred_base
        self.entry = self.base + b.optional_header.addressof_entrypoint
        self.size = b.optional_header.sizeof_image
        # sections: (name, va, vsize, file offset, raw size, characteristics)
        self.sections = [(s.name, self.base + s.virtual_address, s.virtual_size, s.offset, s.size, s.characteristics)
                         for s in b.sections]
        self.mem = {}
        for name, va, vs, fo, fs, ch in self.sections:
            buf = bytearray(open(path, "rb").read()[fo:fo + min(fs, vs)]) if False else None
        raw = open(path, "rb").read()
        for name, va, vs, fo, fs, ch in self.sections:
            data = bytearray(raw[fo:fo + min(fs, vs)])
            data += b"\0" * (vs - len(data))
            self.mem[va] = data
        sec = {s.name: s for s in b.sections}
        self.text = sec[".text"]
        # every executable section is code (some images link extra code sections, e.g. a math library in
        # "text" after .data); text_lo/text_hi span them all, code_ranges says which parts really are code
        self.code_ranges = sorted((va, va + vs) for name, va, vs, fo, fs, ch in self.sections if ch & 0x20000000)
        self.text_lo = self.code_ranges[0][0]
        self.text_hi = self.code_ranges[-1][1]

        # imports: IAT slot address -> "dll!name" (or "dll!#ordinal")
        self.iat = {}
        for lib in b.imports:
            dll = lib.name.lower()
            for e in lib.entries:
                n = e.name if not e.is_ordinal else "#%d" % e.ordinal
                self.iat[self.base + e.iat_address] = "%s!%s" % (dll, n)

        # exports (DLLs): name -> address; exported functions are translation roots
        self.exports = {}
        if b.has_exports:
            for e in b.get_export().entries:
                if e.address and not e.is_extern: self.exports[e.name or "#%d" % e.ordinal] = self.base + e.address

        # base relocations (DIR64 slots); the image is loaded at its preferred base, so these are informational
        self.relocs = []
        if b.has_relocations:
            for blk in b.relocations:
                for en in blk.entries:
                    if str(en.type).endswith("DIR64"):
                        self.relocs.append(self.base + en.address)
        if self.delta:
            for slot in self.relocs:
                m, o = self.seg(slot)
                if m is not None and o + 8 <= len(m):
                    struct.pack_into("<Q", m, o, (struct.unpack_from("<Q", m, o)[0] + self.delta) & 0xFFFFFFFFFFFFFFFF)

        # functions: primary .pdata entries; chained entries (funclet/cold parts) belong to a parent
        pd = sec[".pdata"]
        d = self.read(self.base + pd.virtual_address, pd.virtual_size)
        self.pdata = {}       # start -> (end, unwind rva)
        self.handler = {}     # function start -> (handler addr, handler data addr)
        self.chain_parent = {}  # chained range start -> primary function start
        for i in range(0, len(d) - 11, 12):
            s, e, u = struct.unpack_from("<III", d, i)
            if not s: continue
            self.pdata[self.base + s] = (self.base + e, u)
        for s, (e, u) in self.pdata.items():
            if u & 1:   # indirect (shares another entry's unwind info)
                continue
            parent = self.unwind_root(u)
            if parent is not None:
                self.chain_parent[s] = parent
                continue
            h = self.read(self.base + u, 4)
            fl, cnt = h[0] >> 3, h[2]
            if fl & 3:
                off = self.base + u + 4 + ((cnt + 1) & ~1) * 2
                hr = struct.unpack("<I", self.read(off, 4))[0]
                self.handler[s] = (self.base + hr, off + 4)
        self.func_starts = sorted(s for s in self.pdata if s not in self.chain_parent)

        # TLS directory
        self.tls = None
        if b.has_tls:
            t = b.tls
            self.tls = dict(start=t.addressof_raw_data[0] + self.delta, end=t.addressof_raw_data[1] + self.delta,
                            index=t.addressof_index + self.delta, callbacks=[x + self.delta for x in t.callbacks], zero=t.sizeof_zero_fill)

    def unwind_root(self, u):
        """For a chained unwind info, the primary function it ultimately belongs to (None if primary)."""
        seen = 0
        root = None
        while seen < 32:
            h = self.read(self.base + u, 4)
            fl, cnt = h[0] >> 3, h[2]
            if not (fl & 4):
                return root
            off = self.base + u + 4 + ((cnt + 1) & ~1) * 2
            s, e, u2 = struct.unpack("<III", self.read(off, 12))
            root, u = self.base + s, u2
            seen += 1
        return root

    def seg(self, addr):
        for va, m in self.mem.items():
            if va <= addr < va + len(m):
                return m, addr - va
        return None, 0

    def read(self, addr, n):
        m, o = self.seg(addr)
        if m is None:
            raise KeyError(hex(addr))
        r = bytes(m[o:o + n])
        return r + b"\0" * (n - len(r))

    def contains(self, addr):
        return self.seg(addr)[0] is not None

    def is_code(self, addr):
        return any(lo <= addr < hi for lo, hi in self.code_ranges)

    def code_end(self, addr):
        """End of the executable section containing addr (text_hi if none)."""
        for lo, hi in self.code_ranges:
            if lo <= addr < hi: return hi
        return self.text_hi

    def read_code(self):
        """Bytes of [text_lo, text_hi), with the code sections filled in and zeros between them."""
        buf = bytearray(self.text_hi - self.text_lo)
        for lo, hi in self.code_ranges:
            buf[lo - self.text_lo:hi - self.text_lo] = self.read(lo, hi - lo)
        return bytes(buf)

    def func_end(self, addr):
        """End of the function region starting at addr: the next known function start."""
        i = bisect.bisect_right(self.func_starts, addr)
        return self.func_starts[i] if i < len(self.func_starts) else self.text_hi
