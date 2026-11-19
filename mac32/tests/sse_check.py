#!/usr/bin/env python3
"""Differential test of the mac32 translator's SSE, multiplies and x87 conversions, one instruction at a time.

An i386 executable cannot run on an Apple Silicon Mac, under Rosetta or otherwise, so the instruction is not run
as the translator sees it. Each line of TESTS is assembled twice from the same text:
  - for i386, and the bytes are translated by mac32/tools/recomp32.py and compiled for arm64;
  - for x86-64, where the same instruction on the same registers means the same thing, and run under Rosetta.
Both get the same pseudo-random xmm0 (destination), xmm1 or memory (source) and eax, a quarter of the float lanes
taken from a list of awkward values, and xmm0, eax and edx are compared afterwards, and for the multiplies the
carry and overflow flags.

What is not compared exactly:
  - NaNs: any NaN equals any NaN. x86 makes a negative default NaN and arm64 a positive one, and the two choose
    differently between two NaN operands.
  - rsqrtss, rsqrtps, rcpss, rcpps: the processor's are approximations to 12 bits and the translation is exact, so
    they are compared to 1 part in 2048.

usage: mac32/tests/sse_check.py [--trials N] [--only mnemonic,...] [--keep]
Needs clang (it still assembles i386, though the linker no longer links it), Rosetta, capstone and lief.
"""
import math, os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
M32 = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(M32, "tools"))
import capstone as cs
import lief
import recomp32

# instruction, how its result lanes are compared: i exact, f floats, d doubles, ~ floats to 1 part in 2048,
# m exact and with the carry and overflow flags
# xmm0 is the destination and xmm1 the source; MEM stands for the source in memory.
TESTS = """
pshufd xmm0, xmm1, 0x1b : i
pshufd xmm0, xmm1, 0xe4 : i
pshufd xmm0, xmm1, 0x4e : i
pshufd xmm0, MEM, 0x93 : i
pshuflw xmm0, xmm1, 0x1b : i
pshufhw xmm0, xmm1, 0x6c : i
shufps xmm0, xmm1, 0x1b : i
shufps xmm0, xmm1, 0x88 : i
shufps xmm0, xmm1, 0xdd : i
shufps xmm0, MEM, 0x4e : i
shufpd xmm0, xmm1, 1 : i
shufpd xmm0, xmm1, 2 : i
unpcklps xmm0, xmm1 : i
unpcklps xmm0, MEM : i
unpckhps xmm0, xmm1 : i
unpcklpd xmm0, xmm1 : i
unpckhpd xmm0, xmm1 : i
movhlps xmm0, xmm1 : i
movlhps xmm0, xmm1 : i
punpcklbw xmm0, xmm1 : i
punpckhbw xmm0, xmm1 : i
punpcklwd xmm0, xmm1 : i
punpckhwd xmm0, xmm1 : i
punpckldq xmm0, xmm1 : i
punpckhdq xmm0, xmm1 : i
punpcklqdq xmm0, xmm1 : i
punpckhqdq xmm0, xmm1 : i
pinsrw xmm0, eax, 5 : i
pextrw eax, xmm1, 3 : i
movmskps eax, xmm1 : i
movmskpd eax, xmm1 : i
pmovmskb eax, xmm1 : i
movupd xmm0, xmm1 : i
pandn xmm0, xmm1 : i
pmuludq xmm0, xmm1 : i
pmaddwd xmm0, xmm1 : i
paddb xmm0, xmm1 : i
paddw xmm0, xmm1 : i
paddd xmm0, xmm1 : i
paddq xmm0, xmm1 : i
psubb xmm0, xmm1 : i
psubw xmm0, xmm1 : i
psubd xmm0, xmm1 : i
psubq xmm0, xmm1 : i
paddusb xmm0, xmm1 : i
paddusw xmm0, xmm1 : i
psubusb xmm0, xmm1 : i
psubusw xmm0, xmm1 : i
pmullw xmm0, xmm1 : i
pmulhw xmm0, xmm1 : i
pmulhuw xmm0, xmm1 : i
pminub xmm0, xmm1 : i
pmaxub xmm0, xmm1 : i
pminsw xmm0, xmm1 : i
pmaxsw xmm0, xmm1 : i
pavgb xmm0, xmm1 : i
pavgw xmm0, xmm1 : i
pcmpeqb xmm0, xmm1 : i
pcmpeqw xmm0, xmm1 : i
pcmpeqd xmm0, xmm1 : i
pcmpgtb xmm0, xmm1 : i
pcmpgtw xmm0, xmm1 : i
pcmpgtd xmm0, xmm1 : i
packsswb xmm0, xmm1 : i
packuswb xmm0, xmm1 : i
packssdw xmm0, xmm1 : i
pshufb xmm0, xmm1 : i
palignr xmm0, xmm1, 3 : i
palignr xmm0, xmm1, 9 : i
palignr xmm0, xmm1, 20 : i
pslldq xmm0, 3 : i
pslldq xmm0, 17 : i
psrldq xmm0, 5 : i
psllw xmm0, 3 : i
psrlw xmm0, 3 : i
psraw xmm0, 3 : i
psraw xmm0, 20 : i
pslld xmm0, 5 : i
psrld xmm0, 5 : i
psrad xmm0, 5 : i
psllq xmm0, 9 : i
psrlq xmm0, 9 : i
psllw xmm0, xmm1 : i
psrlw xmm0, xmm1 : i
psraw xmm0, xmm1 : i
pslld xmm0, xmm1 : i
psrld xmm0, xmm1 : i
psrad xmm0, xmm1 : i
psllq xmm0, xmm1 : i
psrlq xmm0, xmm1 : i
phaddd xmm0, xmm1 : i
phaddw xmm0, xmm1 : i
phsubd xmm0, xmm1 : i
phsubw xmm0, xmm1 : i
addps xmm0, xmm1 : f
mulps xmm0, MEM : f
maxps xmm0, xmm1 : f
minps xmm0, xmm1 : f
maxpd xmm0, xmm1 : d
minpd xmm0, xmm1 : d
sqrtps xmm0, xmm1 : f
sqrtpd xmm0, xmm1 : d
rsqrtps xmm0, xmm1 : ~
rcpps xmm0, xmm1 : ~
rsqrtss xmm0, xmm1 : ~
rcpss xmm0, xmm1 : ~
haddps xmm0, xmm1 : f
haddpd xmm0, xmm1 : d
hsubps xmm0, xmm1 : f
hsubpd xmm0, xmm1 : d
cvtdq2ps xmm0, xmm1 : f
cvtps2dq xmm0, xmm1 : i
cvttps2dq xmm0, xmm1 : i
cvtps2pd xmm0, xmm1 : d
cvtps2pd xmm0, MEM8 : d
cvtpd2ps xmm0, xmm1 : f
cvtpd2dq xmm0, xmm1 : i
cvtdq2pd xmm0, xmm1 : d
cmpeqps xmm0, xmm1 : i
cmpltps xmm0, xmm1 : i
cmpleps xmm0, xmm1 : i
cmpunordps xmm0, xmm1 : i
cmpneqps xmm0, xmm1 : i
cmpnltps xmm0, xmm1 : i
cmpnleps xmm0, xmm1 : i
cmpordps xmm0, xmm1 : i
cmpeqpd xmm0, xmm1 : i
cmpltpd xmm0, xmm1 : i
cmplepd xmm0, xmm1 : i
cmpunordpd xmm0, xmm1 : i
cmpneqpd xmm0, xmm1 : i
cmpnltpd xmm0, xmm1 : i
cmpnlepd xmm0, xmm1 : i
cmpordpd xmm0, xmm1 : i
mul dl : m
mul dx : m
mul edx : m
imul dl : m
imul dx : m
imul edx : m
imul ax, dx : m
imul ax, dx, 0x1234 : m
imul ax, dx, -3 : m
imul eax, edx : m
imul eax, edx, -77 : m
fld80 : x
fstp80 : x
"""

DRIVER = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void (*test_fn)(uint8_t *a, uint8_t *s, uint32_t *eax);   // eax[0] eax, eax[1] edx, eax[2] carry | overflow << 1
extern const test_fn tests[]; extern const char kinds[]; extern const int ntests;
static uint64_t st = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
static const float F[] = {0.0f, -0.0f, 1.0f, -1.0f, 1.0f / 0.0f, -1.0f / 0.0f, 0.0f / 0.0f, 1e-40f, 3.5f, -2.5f,
                          2147483648.0f, -2147483904.0f, 0.5f, 1.5f, 2.5f, -0.5f, 1e30f, -1e30f, 16777217.0f};
static const double D[] = {0.0, -0.0, 1.0, -1.0, 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0, 1e-310, 3.5, -2.5, 2147483648.0,
                           -2147483649.0, 0.5, 1.5, 2.5, -0.5, 1e300, -1e300, 2147483647.5};
static void fill(uint8_t *p, char kind, int it) {
    for (int k = 0; k < 16; k += 8) { uint64_t v = rnd(); memcpy(p + k, &v, 8); }
    if (kind == 'f' || kind == '~' || (kind == 'i' && (it & 1)))
        for (int k = 0; k < 4; k++) if (!(rnd() & 3)) memcpy(p + 4 * k, &F[rnd() % (sizeof F / sizeof *F)], 4);
    if (kind == 'd' || (kind == 'i' && (it & 2)))
        for (int k = 0; k < 2; k++) if (!(rnd() & 3)) memcpy(p + 8 * k, &D[rnd() % (sizeof D / sizeof *D)], 8);
}
int main(int argc, char **argv) {
    int trials = argc > 1 ? atoi(argv[1]) : 64;
    for (int t = 0; t < ntests; t++)
        for (int it = 0; it < trials; it++) {
            _Alignas(16) uint8_t a[16], s[16]; uint32_t eax[3] = {(uint32_t)rnd(), (uint32_t)rnd(), 0};
            if (kinds[t] == 'm' && (it & 3) == 1) { eax[0] = (int32_t)(int8_t)eax[0]; eax[1] = (int32_t)(int8_t)eax[1]; }   // small, of either sign
            fill(a, kinds[t], it); fill(s, kinds[t], it >> 2);
            if (kinds[t] == 'i' && (it & 4)) { uint64_t n = rnd() % 70; memcpy(s, &n, 8); }   // a shift count
            if (kinds[t] == 'x') {   // a ten-byte extended that is a number; and a double that is no NaN
                uint64_t m = rnd() | 1ull << 63; uint16_t se = (uint16_t)(16383 - 80 + rnd() % 160) | (uint16_t)(rnd() & 1) << 15;
                if (it % 9 == 0) { m = 0; se &= 0x8000; }
                if (it % 9 == 1) { m = 1ull << 63; se |= 0x7FFF; }
                memcpy(a, &m, 8); memcpy(a + 8, &se, 2);
                double d; uint64_t b = rnd(); memcpy(&d, &b, 8);
                if (d != d) d = 1.5; if (it % 7 == 0) d = D[it / 7 % 6]; if (it % 7 == 1) d = 1e-310;
                memcpy(s, &d, 8);
            }
            tests[t](a, s, eax);
            if (kinds[t] != 'm') eax[2] = 0;
            printf("%d %d ", t, it);
            for (int k = 0; k < 16; k++) printf("%02x", a[k]);
            printf(" %08x%08x%x\n", eax[0], eax[1], eax[2]);
        }
    return 0;
}
"""
DRIVER = "#include <stdlib.h>\n" + DRIVER


def main():
    args = sys.argv[1:]
    trials = int(args[args.index("--trials") + 1]) if "--trials" in args else 256
    only = set(args[args.index("--only") + 1].split(",")) if "--only" in args else None
    tests = []
    for line in TESTS.strip().splitlines():
        text, kind = [x.strip() for x in line.split(":")]
        if not only or text.split()[0] in only: tests.append((text, kind))
    out = tempfile.mkdtemp(prefix="sse_check.")
    def path(n): return os.path.join(out, n)
    def run(cmd, **kw):
        r = subprocess.run(cmd, capture_output=True, text=True, **kw)
        if r.returncode: sys.exit("%s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
        return r.stdout
    sse = [(t, k) for t, k in tests if k != "x"]

    # ---- the translator's side: i386 bytes -> C ----
    with open(path("i386.s"), "w") as f:
        f.write(".intel_syntax noprefix\n.text\n")
        for t, _ in sse:
            f.write(t.replace("MEM8", "qword ptr [ecx]").replace("MEM", "xmmword ptr [ecx]") + "\n")
    run(["clang", "-arch", "i386", "-c", path("i386.s"), "-o", path("i386.o")])
    obj = lief.parse(path("i386.o"))   # kept: a section does not keep its file alive
    code = bytes(next(s for s in obj.sections if s.name == "__text").content)
    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32); md.detail = True
    insns = list(md.disasm(code, 0x1000))
    if len(insns) != len(sse): sys.exit("assembled %d instructions for %d tests" % (len(insns), len(sse)))
    bodies, rejected = [], []
    for (t, k), i in zip(sse, insns):
        g = recomp32.FuncGen.__new__(recomp32.FuncGen)
        g.out, g.cur = [], i
        try:
            if i.mnemonic in ("mul", "imul"): g.muldiv(i.mnemonic, i.operands)
            elif not g.sse(i.mnemonic, i.operands): raise recomp32.Unsupported(i.mnemonic)
            bodies.append("\n".join(g.out))
        except Exception as ex:
            rejected.append("%s (%s)" % (t, ex)); bodies.append("    /* rejected */")
    with open(path("native.c"), "w") as f:
        f.write('#include "rt.h"\nuint8_t *M;\n'
                "int32_t cvtt_i32(double v) { return (v >= -2147483648.0 && v < 2147483648.0) ? (int32_t)v : INT32_MIN; }\n"
                "typedef void (*test_fn)(uint8_t *a, uint8_t *s, uint32_t *eax);\n")
        for n, b in enumerate(bodies):
            # guest address 0x1000 is the source, for the tests that read it from memory through ecx
            f.write("static void t%d(uint8_t *a, uint8_t *s, uint32_t *eax) {\n    CPU cpu = {0}, *c = &cpu; M = s - 0x1000; C.ecx = 0x1000;\n"
                    "    uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0;\n"
                    "    memcpy(&C.xmm[0], a, 16); memcpy(&C.xmm[1], s, 16); C.eax = eax[0]; C.edx = eax[1];\n%s\n"
                    "    memcpy(a, &C.xmm[0], 16); eax[0] = C.eax; eax[1] = C.edx; eax[2] = (cf != 0) | (of != 0) << 1;\n}\n" % (n, b))
        nx = len(bodies)
        f.write("static void t%d(uint8_t *a, uint8_t *s, uint32_t *eax) { M = a - 0x1000; double v = rdt(0x1000); memset(a, 0, 16); memcpy(a, &v, 8); }\n" % nx)
        f.write("static void t%d(uint8_t *a, uint8_t *s, uint32_t *eax) { double v; memcpy(&v, s, 8); M = a - 0x1000; memset(a, 0, 16); wrt(0x1000, v); }\n" % (nx + 1))
        xs = [t for t, k in tests if k == "x"]
        order = list(range(nx)) + [nx + ("fld80", "fstp80").index(t) for t in xs]
        f.write("const test_fn tests[] = {%s};\n" % ", ".join("t%d" % n for n in order))
        f.write('const char kinds[] = "%s";\nconst int ntests = %d;\n' % ("".join(k for _, k in tests), len(tests)))
    # ---- the processor's side: the same text, x86-64 ----
    with open(path("real.s"), "w") as f:
        f.write(".intel_syntax noprefix\n.text\n")
        for n, (t, _) in enumerate(sse):
            f.write(".globl _r%d\n_r%d:\n mov r8, rdx\n movdqu xmm0, [rdi]\n movdqu xmm1, [rsi]\n mov eax, [r8]\n mov edx, [r8 + 4]\n %s\n"
                    " setc r9b\n seto r10b\n movdqu [rdi], xmm0\n mov [r8], eax\n mov [r8 + 4], edx\n movzx r9d, r9b\n movzx r10d, r10b\n"
                    " lea r9d, [r9 + r10*2]\n mov [r8 + 8], r9d\n ret\n"
                    % (n, n, t.replace("MEM8", "qword ptr [rsi]").replace("MEM", "xmmword ptr [rsi]")))
        f.write(".globl _r%d\n_r%d:\n fld tbyte ptr [rdi]\n pxor xmm0, xmm0\n movdqu [rdi], xmm0\n fstp qword ptr [rdi]\n ret\n" % (nx, nx))
        f.write(".globl _r%d\n_r%d:\n fld qword ptr [rsi]\n pxor xmm0, xmm0\n movdqu [rdi], xmm0\n fstp tbyte ptr [rdi]\n ret\n" % (nx + 1, nx + 1))
    with open(path("real.c"), "w") as f:
        f.write("#include <stdint.h>\ntypedef void (*test_fn)(uint8_t *a, uint8_t *s, uint32_t *eax);\n")
        f.write("".join("void r%d(uint8_t *, uint8_t *, uint32_t *);\n" % n for n in range(nx + 2)))
        f.write("const test_fn tests[] = {%s};\n" % ", ".join("r%d" % n for n in order))
        f.write('const char kinds[] = "%s";\nconst int ntests = %d;\n' % ("".join(k for _, k in tests), len(tests)))
    with open(path("driver.c"), "w") as f: f.write(DRIVER)
    # -ffp-contract=off: clang must not fuse a multiply and an add that the guest did separately
    run(["clang", "-arch", "arm64", "-O1", "-ffp-contract=off", "-w", "-I", os.path.join(M32, "runtime"), path("native.c"), path("driver.c"), "-o", path("native")])
    run(["clang", "-arch", "x86_64", "-O1", "-w", path("real.s"), path("real.c"), path("driver.c"), "-o", path("real")])
    got = run([path("native"), str(trials)]).splitlines()
    want = run([path("real"), str(trials)]).splitlines()

    def same(kind, a, b, text):
        if a == b: return True
        if kind == "x" and text == "fstp80": return False
        ha, hb = bytes.fromhex(a.split()[2]), bytes.fromhex(b.split()[2])
        if a.split()[3] != b.split()[3]: return False
        fmt, n = ("<d", 8) if kind in ("d", "x") else ("<f", 4)
        if kind in ("i", "m"): return False
        for k in range(0, 16, n):
            x, y = struct.unpack_from(fmt, ha, k)[0], struct.unpack_from(fmt, hb, k)[0]
            if ha[k:k + n] == hb[k:k + n] or (x != x and y != y): continue
            if kind == "~" and (x == y or (math.isfinite(x) and math.isfinite(y) and abs(x - y) <= abs(y) / 2048)): continue
            return False
        return True

    bad = {}
    for a, b in zip(got, want):
        t = int(a.split()[0])
        if not same(tests[t][1], a, b, tests[t][0]):
            bad.setdefault(t, []).append((a, b))
    for t, pairs in sorted(bad.items()):
        print("DIFFERENT  %-28s %d of %d" % (tests[t][0], len(pairs), trials))
        for a, b in pairs[:2]: print("    translated %s\n    processor  %s" % (" ".join(a.split()[2:]), " ".join(b.split()[2:])))
    for r in rejected: print("REJECTED   " + r)
    print("%d instructions, %d trials each: %d the same as the processor, %d different, %d rejected by the translator"
          % (len(tests), trials, len(tests) - len(bad) - len(rejected), len(bad), len(rejected)))
    if "--keep" in args: print("kept in " + out)
    else:
        import shutil; shutil.rmtree(out)
    return 1 if bad or rejected else 0


if __name__ == "__main__":
    sys.exit(main())
