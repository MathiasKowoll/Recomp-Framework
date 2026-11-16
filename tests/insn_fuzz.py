#!/usr/bin/env python3
"""Differential test of the translator, one instruction at a time.

Every distinct instruction form used by the game's executable (mnemonic + operand kinds and sizes) is put in a
tiny function `insn; pushfq; pop <spare>; ret`. The same bytes run twice with the same random registers, XMM
registers and memory:
  - as real x86-64 code, under Rosetta (an x86_64 harness that maps the bytes executable), and
  - translated by tools/recomp64.py and compiled for arm64,
and every general register, XMM register, the flags (CF PF ZF SF OF) and the changed memory are compared.
Any difference is a translation bug (or, for floating point, a NaN/denormal convention to look at).

usage: tests/insn_fuzz.py GAME_EXE [--trials N] [--only mnemonic,...] [--max-forms N]
"""
import os, sys, re, random, struct, subprocess, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import capstone as cs
from capstone import x86 as X
import recomp64
from pe import PE

OUT = os.path.join(ROOT, "build", "fuzz")
CODE_BASE = 0x150000000          # where the translated snippets pretend to live
SLOT = 64
BUF = 0x2a0000000                # data buffer: the same fixed address in both harnesses
BUF_SIZE = 1 << 20
BUF_MID = BUF + BUF_SIZE // 2
GPRS = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"] + ["r%d" % i for i in range(8, 16)]
POP = {"rbx": b"\x5b", "rbp": b"\x5d", "rsi": b"\x5e", "rdi": b"\x5f",
       "r12": b"\x41\x5c", "r13": b"\x41\x5d", "r14": b"\x41\x5e", "r15": b"\x41\x5f"}
SKIP = {"push", "pop", "pushfq", "popfq", "leave", "enter", "syscall", "cpuid", "rdtsc", "rdtscp", "xgetbv",
        "pause", "nop", "int3", "ud2", "hlt", "lfence", "sfence", "mfence", "prefetcht0", "prefetcht1", "prefetcht2",
        "prefetchnta", "prefetchw", "clflush", "xchg", "cmpxchg16b", "stmxcsr", "ldmxcsr", "fxsave", "fxrstor",
        "xsave", "xrstor", "xsaveopt", "rdrand", "rdseed", "lea", "endbr64", "int", "into", "cld", "std", "sti", "cli"}

def reg64(name):
    """canonical 64-bit register name for a capstone register name, or None"""
    n = name.lower()
    if n in recomp64.REG: return recomp64.REG[n][0]
    return None

STRING_OP = re.compile(r"^(rep |repe |repne |repz |repnz )?(movs|stos|lods|cmps|scas)[bwdq]$")

def collect(pe, md, max_per=2):
    forms = collections.OrderedDict()
    for start in pe.func_starts:
        end = pe.pdata[start][0]
        data = pe.read(start, min(end - start, 0x4000))
        for i in md.disasm(data, start):
            m = i.mnemonic
            if i.group(cs.CS_GRP_JUMP) or i.group(cs.CS_GRP_CALL) or i.group(cs.CS_GRP_RET) or i.group(cs.CS_GRP_INT): continue
            if STRING_OP.match(m) and not any(op.type == X.X86_OP_REG and i.reg_name(op.reg).startswith("xmm") for op in i.operands):
                # string instructions (rep movs/stos/lods/cmps/scas): rsi/rdi point into the buffer, rcx is the count
                lst = forms.setdefault((m, "str"), [])
                if len(lst) < max_per and not any(b == bytes(i.bytes) for b, _ in lst): lst.append((bytes(i.bytes), i))
                continue
            if m in SKIP or m.startswith(("v", "f", "rep", "loop")): continue
            ok, key = True, [m]
            regs_read, regs_write = i.regs_access()
            touched = {i.reg_name(r) for r in list(regs_read) + list(regs_write)}
            if touched & {"rsp", "esp", "sp", "rip", "fs", "gs", "cs", "ds", "es", "ss"}: continue
            if m in ("lds", "les", "lfs", "lgs", "lss", "arpl", "lar", "lsl", "verr", "verw", "sldt", "str", "ltr", "lldt",
                     "in", "out", "insb", "insw", "insd", "outsb", "outsw", "outsd", "iretd", "iretq", "sgdt", "sidt", "lgdt", "lidt",
                     "smsw", "lmsw", "clts", "invlpg", "wbinvd", "invd", "rdmsr", "wrmsr", "rdpmc", "swapgs", "sysret", "sysenter", "sysexit"): continue
            for op in i.operands:
                if op.type == X.X86_OP_MEM:
                    if op.mem.base == X.X86_REG_RIP or op.mem.segment != 0 or op.mem.base == X.X86_REG_RSP: ok = False; break
                    if abs(op.mem.disp) > 0x40000 or not op.mem.base or op.mem.base == op.mem.index: ok = False; break
                    key.append("m%d%s%s" % (op.size, "b" if op.mem.base else "", "i" if op.mem.index else ""))
                elif op.type == X.X86_OP_REG:
                    rn = i.reg_name(op.reg)
                    key.append(("x" if rn.startswith("xmm") else "y" if rn.startswith("ymm") else "r") + str(op.size)
                               + ("h" if rn in ("ah", "bh", "ch", "dh") else ""))
                    if rn.startswith("ymm") or rn.startswith("st") or rn.startswith("mm"): ok = False
                else:
                    key.append("i%d" % op.size)
            if not ok: continue
            k = tuple(key)
            lst = forms.setdefault(k, [])
            if len(lst) < max_per and not any(b == bytes(i.bytes) for b, _ in lst): lst.append((bytes(i.bytes), i))
    return forms

class FakePE:
    def __init__(self, code): self.code, self.base, self.size, self.iat = code, CODE_BASE, len(code), {}
    def read(self, a, n): o = a - CODE_BASE; return bytes(self.code[o:o + n])
    def contains(self, a): return CODE_BASE <= a < CODE_BASE + len(self.code)
    def is_code(self, a): return self.contains(a)

class FakeTR:
    def __init__(self, pe, md, addrs):
        self.pe, self.md, self.funcs = pe, md, set(addrs)
        self.regions = {a: [(a, a + SLOT)] for a in addrs}
        self.unsupported, self.imports_used, self.noreturn = collections.Counter(), set(), set()

def main():
    exe = sys.argv[1]
    trials = int(sys.argv[sys.argv.index("--trials") + 1]) if "--trials" in sys.argv else 24
    only = set(sys.argv[sys.argv.index("--only") + 1].split(",")) if "--only" in sys.argv else None
    max_forms = int(sys.argv[sys.argv.index("--max-forms") + 1]) if "--max-forms" in sys.argv else 100000
    os.makedirs(OUT, exist_ok=True)
    pe = PE(exe)
    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_64); md.detail = True
    forms = collect(pe, md)
    tests = []   # (key, bytes, scratch reg, base/index regs)
    for key, samples in forms.items():
        if only and key[0] not in only: continue
        for b, i in samples:
            regs_read, regs_write = i.regs_access()
            touched = {reg64(i.reg_name(r)) for r in list(regs_read) + list(regs_write)} - {None}
            for op in i.operands:
                if op.type == X.X86_OP_REG and reg64(i.reg_name(op.reg)): touched.add(reg64(i.reg_name(op.reg)))
            spare = next((r for r in POP if r not in touched), None)
            if not spare: continue
            addr_regs = set()
            if key[1:] == ("str",):   # string instruction: source rsi, destination rdi, count rcx
                touched |= {"rsi", "rdi", "rcx", "rax"}
                spare = next((r for r in POP if r not in touched), None)
                addr_regs = {("s", "rsi"), ("d", "rdi")}
                tests.append((key, b, spare, addr_regs, i.mnemonic + " " + i.op_str))
                continue
            for op in i.operands:
                if op.type == X.X86_OP_MEM:
                    if op.mem.base: addr_regs.add(("b", reg64(i.reg_name(op.mem.base))))
                    if op.mem.index: addr_regs.add(("i", reg64(i.reg_name(op.mem.index))))
            if any(r is None for _, r in addr_regs): continue
            tests.append((key, b, spare, addr_regs, i.mnemonic + " " + i.op_str))
            tests.append((key, b"\x9e" + b, spare, addr_regs, "sahf; " + i.mnemonic + " " + i.op_str))
        if len(tests) >= max_forms: break
    print("%d instruction forms, %d tests x %d trials" % (len(forms), len(tests), trials))

    code = bytearray()
    for _, b, spare, _, _ in tests:
        snip = b + b"\x9c" + POP[spare] + b"\xc3"
        code += snip + b"\xcc" * (SLOT - len(snip))
    addrs = [CODE_BASE + SLOT * k for k in range(len(tests))]

    # arm64 side: the translator's own code generator
    tr = FakeTR(FakePE(code), md, addrs)
    recomp64.TR = tr
    bodies = []
    for a in addrs:
        try: bodies.append(recomp64.FuncGen(tr, a).gen())
        except Exception as ex: bodies.append('void F_%x(CPU *c) { bad_insn(c, 0x%xull, "translator error"); }' % (a, a))
    with open(os.path.join(OUT, "arm_code.c"), "w") as f:
        f.write('#include "%s/runtime/cpu.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n' % ROOT)
        f.write("".join("void F_%x(CPU *c);\n" % a for a in addrs))
        f.write("\n\n".join(bodies))
        f.write("\ntypedef void (*fn_t)(CPU *);\nconst fn_t fuzz_fns[] = {%s};\nconst int fuzz_n = %d;\n" % (",".join("F_%x" % a for a in addrs), len(addrs)))
    with open(os.path.join(OUT, "code.bin"), "wb") as f: f.write(code)

    # inputs
    rnd = random.Random(1234)
    def fval():
        r = rnd.random()
        if r < 0.1: return rnd.choice([0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 1e-30, 65536.0, 2147483648.0, -2147483649.0, 1e10, 3.5, -2.5])
        if r < 0.55: return rnd.uniform(-1000, 1000)
        return rnd.uniform(-1, 1)
    def xmm_rand():
        k = rnd.random()
        if k < 0.4: return struct.pack("<4f", *[fval() for _ in range(4)])
        if k < 0.6: return struct.pack("<2d", fval(), fval())
        if k < 0.8: return bytes(rnd.getrandbits(8) for _ in range(16))
        return struct.pack("<4i", *[rnd.randint(-5000, 5000) for _ in range(4)])
    def gpr_rand():
        k = rnd.random()
        if k < 0.25: return rnd.getrandbits(64)
        if k < 0.5: return rnd.getrandbits(32)
        if k < 0.7: return rnd.randint(0, 70)
        if k < 0.8: return rnd.choice([0, 1, 0xFFFFFFFFFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x8000000000000000, 0xFFFFFFFF])
        return (-rnd.randint(1, 1000)) & 0xFFFFFFFFFFFFFFFF
    with open(os.path.join(OUT, "inputs.bin"), "wb") as f:
        f.write(struct.pack("<III", len(tests), trials, SLOT))
        for t, (key, b, spare, addr_regs, text) in enumerate(tests):
            for _ in range(trials):
                g = [gpr_rand() for _ in range(16)]
                locked = text.replace("sahf; ", "").startswith("lock") or key[0] in ("xadd", "cmpxchg")   # atomics need aligned addresses on ARM (real data is aligned)
                if key[1:] == ("str",):   # string instruction: overlapping ranges a third of the time
                    n = {"b": 1, "w": 2, "d": 4, "q": 8}[key[0][-1]]
                    src = BUF_MID + rnd.randint(-256, 256)
                    over = rnd.random() < 0.34
                    g[GPRS.index("rsi")] = src
                    g[GPRS.index("rdi")] = src + n * rnd.randint(1, 4) if over else BUF_MID + rnd.randint(-256, 256)
                    g[GPRS.index("rcx")] = rnd.randint(0, 64 // n)
                    if key[0].startswith(("scas", "repe", "repne", "repz", "repnz")) and rnd.random() < 0.5:
                        g[GPRS.index("rax")] &= 0xFF   # a value likely to occur in the buffer, so the scan can stop early
                    g[GPRS.index("rsp")] = 0
                    x = b"".join(xmm_rand() for _ in range(16))
                    f.write(struct.pack("<16Q", *g) + x + struct.pack("<I", rnd.getrandbits(32)))
                    continue
                for kind, r in addr_regs:
                    idx = GPRS.index(r)
                    g[idx] = (BUF_MID + 64 * rnd.randint(-2, 2)) if kind == "b" and locked else BUF_MID + rnd.randint(-64, 64) if kind == "b" else (0 if locked else rnd.randint(0, 16))
                    if kind == "b" and any(k2 == "i" and r2 == r for k2, r2 in addr_regs): g[idx] = rnd.randint(0, 16)
                m = key[0]
                if m in ("div", "idiv"):   # no #DE: small dividend, non-zero divisor
                    g[0] = rnd.randint(0, 1 << 20); g[2] = 0
                    for op in [o for o in key[1:] if o.startswith("r")]: pass
                    src = re.split(r",\s*", text.split(" ", 1)[1])[0] if " " in text else ""
                    rr = reg64(src.strip()) if src.strip() else None
                    if rr and rr in GPRS: g[GPRS.index(rr)] = rnd.randint(1, 1000)
                g[GPRS.index("rsp")] = 0
                x = b"".join(xmm_rand() for _ in range(16))
                f.write(struct.pack("<16Q", *g) + x + struct.pack("<I", rnd.getrandbits(32)))
    with open(os.path.join(OUT, "tests.txt"), "w") as f:
        for key, b, spare, _, text in tests: f.write("%s\t%s\t%s\n" % (" ".join(key), spare, text))

    # harnesses
    harness = os.path.join(ROOT, "tests", "fuzz_harness.c")
    rt = os.path.join(ROOT, "runtime")
    def run(cmd):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode: print(" ".join(cmd)); print(r.stdout[-3000:], r.stderr[-3000:]); sys.exit(1)
    run(["clang", "-O1", "-arch", "x86_64", "-DX86", harness, os.path.join(ROOT, "tests", "fuzz_x86.S"), "-o", os.path.join(OUT, "x86h")])
    run(["clang", "-O1", "-arch", "arm64", "-I" + rt, "-w", "-mmacosx-version-min=26.0", harness, os.path.join(OUT, "arm_code.c"), "-o", os.path.join(OUT, "armh")])
    for side in ("x86h", "armh"):
        with open(os.path.join(OUT, side + ".out"), "wb") as o:
            r = subprocess.run(["arch", "-x86_64" if side == "x86h" else "-arm64", os.path.join(OUT, side), OUT], stdout=o, stderr=subprocess.PIPE)
            if r.returncode: print(side, "failed:", r.stderr.decode()[-2000:]); sys.exit(1)
    compare(tests, trials)

FLAGS_MASK = 0x8c5   # CF PF ZF SF OF (AF is not tracked: the game never reads it)
# flags x86 leaves undefined after an instruction (any value is correct): not compared
UNDEF = {
    "imul": 0xc4, "mul": 0xc4,                                   # SF ZF PF
    "div": 0x8c5, "idiv": 0x8c5,                                 # all
    "bsf": 0x885, "bsr": 0x885,                                  # all but ZF
    "bt": 0x8c4, "bts": 0x8c4, "btr": 0x8c4, "btc": 0x8c4,       # all but CF
    "and": 0, "or": 0, "xor": 0, "test": 0,
    "popcnt": 0, "lzcnt": 0, "tzcnt": 0,
}
def undef_mask(text):
    text = text.replace("sahf; ", "")
    m = text.split(" ")[0]
    if m in UNDEF: return UNDEF[m]
    if m in ("shl", "shr", "sar", "sal", "rol", "ror", "rcl", "rcr", "shld", "shrd"):
        ops = text.split(" ", 1)[1] if " " in text else ""
        last = ops.split(",")[-1].strip()
        # OF is only defined for 1-bit shifts/rotates; a zero count leaves every flag alone
        try: n = int(last, 0)
        except ValueError: n = None
        return 0x800 if n != 1 else 0
    return 0

def compare(tests, trials):
    rec = 16 * 8 + 16 * 16 + 8 + 4 + 64 * 5
    a, b = open(os.path.join(OUT, "x86h.out"), "rb").read(), open(os.path.join(OUT, "armh.out"), "rb").read()
    bad = collections.OrderedDict()
    for t, (key, _, spare, _, text) in enumerate(tests):
        for k in range(trials):
            o = (t * trials + k) * rec
            ra, rb = a[o:o + rec], b[o:o + rec]
            if ra == rb: continue
            if struct.unpack_from("<I", ra, 392)[0] == 0xFFFFFFFF: continue   # faulted on real x86 (data, privileged)
            ga, gb = struct.unpack_from("<16Q", ra), struct.unpack_from("<16Q", rb)
            diffs = []
            for r in range(16):
                if r == 4: continue
                va, vb = ga[r], gb[r]
                if GPRS[r] == spare:
                    mask = FLAGS_MASK & ~undef_mask(text); va &= mask; vb &= mask; name = "flags"
                else: name = GPRS[r]
                if va != vb: diffs.append("%s x86=%x arm=%x" % (name, va, vb))
            xa, xb = ra[128:384], rb[128:384]
            for r in range(16):
                if xa[16 * r:16 * r + 16] != xb[16 * r:16 * r + 16]:
                    diffs.append("xmm%d x86=%s arm=%s" % (r, xa[16 * r:16 * r + 16].hex(), xb[16 * r:16 * r + 16].hex()))
            if ra[384:] != rb[384:]: diffs.append("memory differs")
            if diffs: bad.setdefault(text, []).append(diffs)
    by_m = collections.Counter(t.split(" ")[0] for t in bad)
    print("%d of %d tests differ" % (len(bad), len(tests)))
    for text, d in bad.items():
        print("  %-40s %2d/%d  %s" % (text[:40], len(d), trials, "; ".join(d[0])[:220]))
    print("by mnemonic:", dict(by_m.most_common()))

if __name__ == "__main__":
    main()
