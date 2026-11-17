#!/usr/bin/env python3
"""Differential test of the translator's MMX support (mm0-7, 64-bit integer SIMD).

insn_fuzz.py compares general and XMM registers only, so MMX gets its own small harness: each form below is run
as real x86-64 under Rosetta and as translated arm64 code, with the same random mm0/mm1, rax and 16 bytes of memory
at [rdi]; mm0, mm1, rax and the memory are compared afterwards.

usage: tests/mmx_fuzz.py [--trials N]
"""
import os, sys, random, struct, subprocess
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools")); sys.path.insert(0, os.path.join(ROOT, "tests"))
import capstone as cs
import lief
import recomp64
from insn_fuzz import FakePE, FakeTR, CODE_BASE, SLOT

OUT = os.path.join(ROOT, "build", "mmx_fuzz")
FORMS = """movd mm0, eax|movd mm0, dword ptr [rdi]|movd eax, mm1|movd dword ptr [rdi], mm1|movq mm0, mm1|movq mm0, rax
movq rax, mm1|movq mm0, qword ptr [rdi]|movq qword ptr [rdi], mm1|packuswb mm0, mm1|packsswb mm0, mm1|packssdw mm0, mm1
paddsw mm0, mm1|paddsw mm0, qword ptr [rdi]|paddw mm0, mm1|paddb mm0, mm1|paddd mm0, mm1|psubw mm0, mm1|psubusw mm0, mm1
pmulhw mm0, mm1|pmullw mm0, mm1|pmaddwd mm0, mm1|pand mm0, qword ptr [rdi]|pandn mm0, mm1|por mm0, mm1|pxor mm0, mm1
pslld mm0, 5|pslld mm0, 33|pslld mm0, mm1|pslld mm0, qword ptr [rdi]|psllq mm0, 13|psllw mm0, 3|psllw mm0, qword ptr [rdi]
psrld mm0, 7|psrld mm0, mm1|psrlq mm0, 9|psrlw mm0, 4|psrlw mm0, qword ptr [rdi]|psraw mm0, 3|psrad mm0, mm1
punpckhbw mm0, mm1|punpckhbw mm0, qword ptr [rdi]|punpckhdq mm0, mm1|punpckhwd mm0, mm1|punpckhwd mm0, qword ptr [rdi]
punpcklbw mm0, mm1|punpcklbw mm0, dword ptr [rdi]|punpckldq mm0, mm1|punpcklwd mm0, mm1|punpcklwd mm0, dword ptr [rdi]"""
FORMS = [f.strip() for line in FORMS.splitlines() for f in line.split("|")]

X86_HARNESS = r"""
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
typedef struct { uint64_t mm0, mm1, rax, rdi; } St;
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); static uint8_t code[1 << 16]; size_t n = fread(code, 1, sizeof code, f); fclose(f);
    uint8_t *x = mmap(0, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    for (size_t k = 0; k < n; k++) x[k] = code[k];
    int ntests = atoi(argv[2]); uint64_t in[6]; uint8_t buf[16];
    while (fread(in, 8, 6, stdin) == 6) {
        int t = (int)in[0];
        St s = { in[1], in[2], in[3], (uint64_t)buf };
        __builtin_memcpy(buf, &in[4], 16);
        void *fn = x + (size_t)t * %d;
        __asm__ volatile(
            "movq (%%%%rbx), %%%%mm0\n movq 8(%%%%rbx), %%%%mm1\n movq 16(%%%%rbx), %%%%rax\n movq 24(%%%%rbx), %%%%rdi\n"
            "callq *%%%%rcx\n"
            "movq %%%%mm0, (%%%%rbx)\n movq %%%%mm1, 8(%%%%rbx)\n movq %%%%rax, 16(%%%%rbx)\n emms\n"
            :: "b"(&s), "c"(fn) : "rax", "rdi", "memory");
        fwrite(&s, 8, 3, stdout); fwrite(buf, 1, 16, stdout);
    }
    (void)ntests; return 0;
}
""" % SLOT

ARM_HARNESS = r"""
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "cpu.h"
typedef void (*fn_t)(CPU *);
extern const fn_t fuzz_fns[]; extern const int fuzz_n;
void bad_insn(CPU *c, uint64_t a, const char *s) { fprintf(stderr, "bad_insn %llx %s\n", a, s); }
int main(void) {
    uint64_t in[6]; uint8_t buf[16]; static uint64_t stack[64];
    while (fread(in, 8, 6, stdin) == 6) {
        CPU c; memset(&c, 0, sizeof c);
        c.mm[0] = in[1]; c.mm[1] = in[2]; c.rax = in[3]; memcpy(buf, &in[4], 16);
        c.rdi = (uint64_t)buf; c.rsp = (uint64_t)&stack[32];
        fuzz_fns[in[0]](&c);
        uint64_t o[3] = { c.mm[0], c.mm[1], c.rax };
        fwrite(o, 8, 3, stdout); fwrite(buf, 1, 16, stdout);
    }
    return 0;
}
"""


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, **kw)
    if r.returncode: sys.exit("failed: %s\n%s" % (" ".join(cmd), r.stderr.decode(errors="replace")))
    return r.stdout


def main():
    trials = int(sys.argv[sys.argv.index("--trials") + 1]) if "--trials" in sys.argv else 2000
    os.makedirs(OUT, exist_ok=True)
    # assemble every form into its own 64-byte slot (insn; ret)
    asm = ".intel_syntax noprefix\n.text\n" + "".join(".p2align 6\n%s\nret\n" % f for f in FORMS)
    open(os.path.join(OUT, "forms.s"), "w").write(asm)
    run(["clang", "-arch", "x86_64", "-c", os.path.join(OUT, "forms.s"), "-o", os.path.join(OUT, "forms.o")])
    obj = lief.parse(os.path.join(OUT, "forms.o"))   # keep it alive: section content is a view into it
    code = bytes(obj.get_section("__text").content)
    code += b"\xcc" * (SLOT * len(FORMS) - len(code))
    open(os.path.join(OUT, "code.bin"), "wb").write(code)

    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_64); md.detail = True
    addrs = [CODE_BASE + SLOT * k for k in range(len(FORMS))]
    tr = FakeTR(FakePE(bytearray(code)), md, addrs)
    recomp64.TR = tr
    bodies = [recomp64.FuncGen(tr, a).gen() for a in addrs]
    if tr.unsupported: sys.exit("unsupported: %s" % dict(tr.unsupported))
    with open(os.path.join(OUT, "arm_code.c"), "w") as f:
        f.write('#include "cpu.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n')
        f.write("".join("void F_%x(CPU *c);\n" % a for a in addrs))
        f.write("\n\n".join(bodies))
        f.write("\ntypedef void (*fn_t)(CPU *);\nconst fn_t fuzz_fns[] = {%s};\nconst int fuzz_n = %d;\n"
                % (",".join("F_%x" % a for a in addrs), len(addrs)))
    open(os.path.join(OUT, "x86h.c"), "w").write(X86_HARNESS)
    open(os.path.join(OUT, "armh.c"), "w").write(ARM_HARNESS)
    rt = os.path.join(ROOT, "runtime")
    run(["clang", "-O1", "-arch", "x86_64", "-w", os.path.join(OUT, "x86h.c"), "-o", os.path.join(OUT, "x86h")])
    run(["clang", "-O1", "-arch", "arm64", "-w", "-I" + rt, os.path.join(OUT, "armh.c"), os.path.join(OUT, "arm_code.c"),
         "-o", os.path.join(OUT, "armh")])

    rnd = random.Random(7)
    def v64():
        r = rnd.random()
        if r < 0.25: return rnd.randrange(0, 70)                                   # small: shift counts
        if r < 0.4: return rnd.choice([0, 0xFFFFFFFFFFFFFFFF, 0x8000800080008000, 0x7FFF7FFF7FFF7FFF, 0x00FF00FF00FF00FF])
        return rnd.getrandbits(64)
    tests = [(t, [v64() for _ in range(5)]) for _ in range(trials) for t in range(len(FORMS))]
    inp = b"".join(struct.pack("<6Q", t, *v) for t, v in tests)
    x = run(["arch", "-x86_64", os.path.join(OUT, "x86h"), os.path.join(OUT, "code.bin"), str(len(FORMS))], input=inp)
    a = run(["arch", "-arm64", os.path.join(OUT, "armh")], input=inp)
    rec = 40
    bad = {}
    for k, (t, v) in enumerate(tests):
        xs, as_ = x[k * rec:(k + 1) * rec], a[k * rec:(k + 1) * rec]
        if xs != as_ and FORMS[t] not in bad:
            bad[FORMS[t]] = (v, struct.unpack("<3Q", xs[:24]), struct.unpack("<3Q", as_[:24]), xs[24:].hex(), as_[24:].hex())
    print("%d forms x %d trials: %d forms differ" % (len(FORMS), trials, len(bad)))
    for f, (v, xr, ar, xm, am) in bad.items():
        print("  %-32s in mm0=%016x mm1=%016x rax=%016x\n    x86 mm0=%016x mm1=%016x rax=%016x mem=%s\n    arm mm0=%016x mm1=%016x rax=%016x mem=%s"
              % (f, v[0], v[1], v[2], *xr, xm, *ar, am))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
