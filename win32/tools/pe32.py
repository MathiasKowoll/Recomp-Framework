"""PE32 (i386) reader for the win32 translator: sections, imports (IAT slots), exports, TLS."""
import struct

CRT_DLLS = ("api-ms-win-crt-", "ucrtbase", "vcruntime", "msvcp", "msvcr")


def dll_key(dll):
    """Short name used in shim symbols: kernel32.dll -> kernel32; every Universal CRT / VC runtime DLL -> crt."""
    d = dll.lower()
    if d.startswith(CRT_DLLS): return "crt"
    return d[:-4] if d.endswith(".dll") else d


class PE32:
    def __init__(self, path):
        self.path = path
        self.data = d = open(path, "rb").read()
        e = struct.unpack_from("<I", d, 0x3C)[0]
        if d[e:e + 4] != b"PE\0\0": raise ValueError("not a PE file")
        nsec, = struct.unpack_from("<H", d, e + 6)
        optsz, = struct.unpack_from("<H", d, e + 20)
        o = e + 24
        if struct.unpack_from("<H", d, o)[0] != 0x10B: raise ValueError("not a PE32 (i386) image")
        self.entry_rva, = struct.unpack_from("<I", d, o + 16)
        self.base, = struct.unpack_from("<I", d, o + 28)
        self.size, = struct.unpack_from("<I", d, o + 56)
        self.entry = self.base + self.entry_rva
        ndir, = struct.unpack_from("<I", d, o + 92)
        self.dirs = [struct.unpack_from("<II", d, o + 96 + 8 * k) for k in range(ndir)]
        self.sections = []   # (name, va, vsize, file offset, file size, characteristics)
        t = o + optsz
        for k in range(nsec):
            s = t + 40 * k
            name = d[s:s + 8].rstrip(b"\0").decode("latin-1")
            vs, va, rs, ro = struct.unpack_from("<IIII", d, s + 8)
            ch, = struct.unpack_from("<I", d, s + 36)
            self.sections.append((name, self.base + va, vs, ro, rs, ch))
        code = [s for s in self.sections if s[5] & 0x20000000 and s[0] != ".bind"]   # MEM_EXECUTE (not the DRM stub)
        self.code = code
        self.text_lo = min(s[1] for s in code)
        self.text_hi = max(s[1] + s[2] for s in code)
        self.iat = {}        # slot VA -> "dll!name" or "dll!#ord"
        self.imports = []    # (dll, name, slot)
        self.read_imports()
        self.exports = {}
        self.tls = None

    def is_code(self, a):
        return any(va <= a < va + vs for _, va, vs, _, _, _ in self.code)

    def code_end(self, a):
        for _, va, vs, _, _, _ in self.code:
            if va <= a < va + vs: return va + vs
        return a

    def off(self, a):
        for _, va, vs, fo, fs, _ in self.sections:
            if va <= a < va + max(vs, fs):
                if a - va >= fs: return None   # zero-fill tail
                return fo + a - va
        raise KeyError(a)

    def read(self, a, n):
        out = bytearray()
        while n > 0:
            o = self.off(a)
            sec = next(s for s in self.sections if s[1] <= a < s[1] + max(s[2], s[4]))
            chunk = min(n, sec[1] + max(sec[2], sec[4]) - a)
            if o is None: out += bytes(chunk)
            else:
                avail = max(0, min(chunk, sec[1] + sec[4] - a))
                out += self.data[o:o + avail] + bytes(chunk - avail)
            a += chunk; n -= chunk
        return bytes(out)

    def u32(self, a):
        return struct.unpack("<I", self.read(a, 4))[0]

    def cstr(self, a):
        o = self.off(a); e = self.data.index(b"\0", o)
        return self.data[o:e].decode("latin-1")

    def read_imports(self):
        rva, size = self.dirs[1]
        if not rva: return
        a = self.base + rva
        while True:
            ilt, _, _, name, iat = struct.unpack("<IIIII", self.read(a, 20))
            if not name: break
            dll = self.cstr(self.base + name)
            k = 0
            src = ilt or iat
            while True:
                v = self.u32(self.base + src + 4 * k)
                if not v: break
                slot = self.base + iat + 4 * k
                n = "#%d" % (v & 0xFFFF) if v & 0x80000000 else self.cstr(self.base + v + 2)
                self.iat[slot] = "%s!%s" % (dll, n)
                self.imports.append((dll, n, slot))
                k += 1
            a += 20

    def writable(self, a):
        for _, va, vs, _, fs, ch in self.sections:
            if va <= a < va + max(vs, fs): return bool(ch & 0x80000000)
        return True
