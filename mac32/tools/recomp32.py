#!/usr/bin/env python3
"""Static recompiler: i386 Mach-O functions -> C operating on the guest CPU state.

usage: recomp32.py BINARY LO HI OUTDIR [--hle FILE]   (hex addresses; translates functions in [LO,HI))
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
from macho import Image
import capstone as cs
from capstone import x86 as X

R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
R16 = {r[1:]: r for r in R32}
R8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
R8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
UT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
ST = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}
MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}

CC = {"e": "zf", "ne": "!zf", "b": "cf", "ae": "!cf", "be": "(cf|zf)", "a": "(!cf&&!zf)",
      "l": "(sf!=of)", "ge": "(sf==of)", "le": "(zf||sf!=of)", "g": "(!zf&&sf==of)",
      "s": "sf", "ns": "!sf", "p": "pf", "np": "!pf", "o": "of", "no": "!of"}

STUB_SPECIAL = {"_setjmp"}


def cname(s):
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


class Unsupported(Exception):
    pass


class rewrite:
    """Write a file only if what it would hold differs from what it holds: a translation after a small change
    leaves most of its output as it was, and make then compiles only the rest."""
    def __init__(self, path): self.path = path
    def __enter__(self):
        import io
        self.f = io.StringIO()
        return self.f
    def __exit__(self, kind, *rest):
        if kind: return False
        new = self.f.getvalue()
        try:
            with open(self.path) as f: same = f.read() == new
        except OSError: same = False
        if not same:
            with open(self.path, "w") as f: f.write(new)
        return False


class FuncGen:
    def __init__(self, tr, addr):
        self.tr, self.img, self.addr = tr, tr.img, addr
        self.end = self.img.func_end(addr)
        self.code = self.img.read(addr, self.end - addr)
        # C++ exceptions: the call sites of the function's table, and the landing pads they lead to, which are code
        # of the function that nothing jumps to (the unwinder does) and are translated with it.
        self.lsda, self.sites = self.img.eh_sites(addr) if tr.eh else (None, [])
        self.pads = {lp for _, _, lp, _ in self.sites if lp and addr <= lp < self.end}
        tr.pads_out += sum(1 for _, _, lp, _ in self.sites if lp and not (addr <= lp < self.end))
        self.ins = self.descend()
        self.addrs = {i.address for i in self.ins}
        self.labels = set()
        self.out = []
        self.reg = None

    # ---------- operands ----------
    def rn(self, r):
        return self.cur.reg_name(r)

    def mem_addr(self, op):
        m = op.mem
        if m.segment not in (0, X.X86_REG_ES, X.X86_REG_DS):
            raise Unsupported("segment " + self.rn(m.segment))
        for r in (m.base, m.index):   # an address-size prefix: bx+si and the like, which no compiler emits
            if r and self.rn(r) not in R32: raise Unsupported("16-bit address")
        parts = []
        if m.base:
            parts.append("C." + self.rn(m.base))
        if m.index:
            parts.append("C.%s*%du" % (self.rn(m.index), m.scale))
        if m.disp or not parts:
            parts.append("0x%Xu" % (m.disp & 0xFFFFFFFF))
        return "(uint32_t)(%s)" % " + ".join(parts)

    def rd(self, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            n = self.rn(op.reg)
            if n in R32: return "C." + n
            if n in R16: return "(uint16_t)C." + R16[n]
            if n in R8L: return "(uint8_t)C." + R8L[n]
            if n in R8H: return "(uint8_t)(C.%s>>8)" % R8H[n]
            raise Unsupported("reg " + n)
        if op.type == X.X86_OP_IMM:
            return "(%s)0x%Xu" % (UT[size], op.imm & ((1 << (8 * size)) - 1))
        if op.type == X.X86_OP_MEM:
            return "rd%d(%s)" % (8 * size, self.mem_addr(op))
        raise Unsupported("op")

    def wr(self, op, v):
        if op.type == X.X86_OP_REG:
            n = self.rn(op.reg)
            if n in R32: return "C.%s = (uint32_t)(%s);" % (n, v)
            if n in R16: return "C.{0} = (C.{0} & 0xFFFF0000u) | (uint16_t)({1});".format(R16[n], v)
            if n in R8L: return "C.{0} = (C.{0} & 0xFFFFFF00u) | (uint8_t)({1});".format(R8L[n], v)
            if n in R8H: return "C.{0} = (C.{0} & 0xFFFF00FFu) | ((uint32_t)(uint8_t)({1}) << 8);".format(R8H[n], v)
            raise Unsupported("wreg " + n)
        if op.type == X.X86_OP_MEM:
            return "wr%d(%s, (%s)(%s));" % (8 * op.size, self.mem_addr(op), UT[op.size], v)
        raise Unsupported("wop")

    def xmm(self, op):
        n = self.rn(op.reg)
        if not n.startswith("xmm"): raise Unsupported(n)
        return "C.xmm[%s]" % n[3:]

    def e(self, s):
        self.out.append("    " + s)

    def szf(self, r, size):
        return "zf = ((%s) & %s) == 0; sf = ((%s) >> %d) & 1;" % (r, MASK[size], r, 8 * size - 1)

    # ---------- function ----------
    def decode(self, a):
        if not (self.addr <= a < self.end): return None
        for i in self.tr.md.disasm(self.code[a - self.addr:a - self.addr + 16], a, 1):
            return i
        return None

    def table_limit(self, k, ins_list, reg):
        # look back for "cmp reg, N" guarding the table -> N+1 entries
        for j in range(k - 1, max(-1, k - 6), -1):
            p = ins_list[j]
            if p.mnemonic == "cmp" and len(p.operands) == 2 and p.operands[1].type == X.X86_OP_IMM \
                    and p.operands[0].type == X.X86_OP_REG and p.operands[0].reg == reg:
                return p.operands[1].imm + 1
        return None

    PC_THUNK = {0x04, 0x0c, 0x14, 0x1c, 0x2c, 0x34, 0x3c}   # mov reg, [esp] ; ret (___i686.get_pc_thunk.*)

    def relative_tables(self, seen, pic):
        """Switches of position-independent code. Each entry of the table is a case's distance from the function's
        PIC base (the address after its `call next`), which a register holds, or a stack slot once the function
        has run out of registers. The shapes the compilers of the time wrote:
            mov r, [b + i*4 + d] ; add r, b ; jmp r          the base in a register
            mov r, [b + i*4 + d] ; add r, [ebp - n] ; jmp r   the base spilled
            add b, [b + i*4 + d] ; jmp b                      load and add in one
            mov r, [t + i*4]     ; add r, b ; jmp r          the table's address computed earlier (lea t, [b + d])
        with instructions that set other registers in between. Returns the cases of the tables not seen before."""
        if len(pic) != 1: return []
        base, found = next(iter(pic)), []
        def entries(ta, limit=None):
            tbl = []
            while len(tbl) < (limit or 4096):
                try: t = (base + int.from_bytes(self.img.read(ta, 4), "little")) & 0xFFFFFFFF
                except KeyError: break
                if not (self.addr <= t < self.end): break
                tbl.append(t); ta += 4
            return tbl
        for i in [x for x in seen.values() if x]:
            if i.address in self.tables or i.mnemonic != "jmp" or i.operands[0].type != X.X86_OP_REG: continue
            r = i.operands[0].reg
            prev = sorted((x for x in seen.values() if x and i.address - 96 <= x.address < i.address), key=lambda x: x.address)
            add = load = None
            for p in reversed(prev[-14:]):
                o = p.operands
                if len(o) != 2 or o[0].type != X.X86_OP_REG or o[0].reg != r: continue
                indexed = o[1].type == X.X86_OP_MEM and o[1].mem.index and o[1].mem.scale == 4
                if add is None:
                    if p.mnemonic == "add" and indexed and o[1].mem.base == r: load = p; break
                    if p.mnemonic == "add" and (o[1].type == X.X86_OP_REG or (o[1].type == X.X86_OP_MEM and not o[1].mem.index)):
                        add = p
                    else: break
                elif p.mnemonic == "mov" and indexed and o[1].mem.base:
                    load = p; break
                else: break
            if not load: continue
            m = load.operands[1].mem
            if m.disp:
                # As many entries as lead into this function. The compare that guards the index is not looked for:
                # it may be on another register or a screen away, and a compare taken for it by mistake cuts real
                # cases off, while a case too many (the start of the table after this one) costs nothing.
                tbl = entries((base + m.disp) & 0xFFFFFFFF)
            else:
                # The table's address was computed apart: every `lea reg, [reg + d]` of the function whose base + d
                # holds distances to this function is taken for one. More cases than this jump has is harmless.
                tbl = []
                for q in [x for x in seen.values() if x]:
                    o = q.operands
                    if q.mnemonic == "lea" and len(o) == 2 and o[1].type == X.X86_OP_MEM and o[1].mem.base \
                            and not o[1].mem.index and o[1].mem.disp:
                        e = entries((base + o[1].mem.disp) & 0xFFFFFFFF)
                        if len(e) >= 2: tbl += e
                if not tbl: continue
            self.tables[i.address] = (tbl, None)
            found.extend(tbl)
        return found

    def descend(self):
        seen, work, out = {}, [self.addr] + sorted(self.pads), []
        self.tables = {}
        pic = set()
        while True:
            self.walk(seen, work, pic)
            work = [t for t in self.relative_tables(seen, pic) if t not in seen]
            if not work: break
        ins = sorted((x for x in seen.values() if x), key=lambda x: x.address)
        # trim tables using the cmp guard now that we have linear context
        for k, i in enumerate(ins):
            if i.address in self.tables:
                tbl, reg = self.tables[i.address]
                lim = self.table_limit(k, ins, reg) if reg is not None else None
                if lim: self.tables[i.address] = (tbl[:lim], reg)
        self.bad_decode = [a for a, x in seen.items() if x is None]
        return ins

    def walk(self, seen, work, pic):
        while work:
            a = work.pop()
            while a not in seen:
                i = self.decode(a)
                if i is None:
                    seen[a] = None; break
                seen[a] = i
                m = i.mnemonic
                if i.group(cs.CS_GRP_JUMP) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                    t = i.operands[0].imm
                    if self.addr <= t < self.end: work.append(t)
                if m == "jmp" and i.operands and i.operands[0].type == X.X86_OP_MEM:
                    op = i.operands[0]
                    if not op.mem.base and op.mem.index and op.mem.scale == 4:
                        prev = sorted(x for x in seen.values() if x and x.address < i.address)[-6:] if False else None
                        tbl, ta = [], op.mem.disp
                        prev = sorted((x for x in seen.values() if x and i.address - 40 <= x.address < i.address), key=lambda x: x.address)
                        limit = self.table_limit(len(prev), prev, op.mem.index) or 4096
                        n = 0
                        while n < limit:
                            t = int.from_bytes(self.img.read(ta, 4), "little")
                            if not (self.addr <= t < self.end): break
                            tbl.append(t); ta += 4; n += 1
                        self.tables[i.address] = (tbl, op.mem.index)
                        work.extend(tbl)
                if m == "call" and i.operands and i.operands[0].type == X.X86_OP_IMM:
                    t = i.operands[0].imm
                    if t == i.address + i.size: pic.add(t)
                    elif t not in self.img.stubs:
                        try: b = self.img.read(t, 4)
                        except KeyError: b = b""
                        if len(b) == 4 and b[0] == 0x8B and b[1] in self.PC_THUNK and b[2] == 0x24 and b[3] == 0xC3:
                            pic.add(i.address + i.size)
                if m in ("ret", "jmp", "ud2", "hlt", "retf", "ljmp") or m.startswith("ret") or self.no_return(i):
                    break
                a = i.address + i.size

    def gen(self):
        for i in self.ins:
            if i.group(cs.CS_GRP_JUMP) and i.operands and i.operands[0].type == X.X86_OP_IMM:
                t = i.operands[0].imm
                if t in self.addrs: self.labels.add(t)
            if i.mnemonic == "jmp" and i.operands[0].type != X.X86_OP_IMM:
                for t in self.jump_table(i): self.labels.add(t)
        self.labels |= self.pads & self.addrs
        f = self.out
        f.append("void F_%x(CPU *c) { /* %s */" % (self.addr, self.img.func_names[self.addr]))
        f.append("    uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0; (void)cf; (void)zf; (void)sf; (void)of; (void)pf;")
        f.append("    TRACE_FN(0x%x);" % self.addr)
        for k, i in enumerate(self.ins):
            nx = self.ins[k + 1].address if k + 1 < len(self.ins) else None
            if not self.ends_block(i) and nx != i.address + i.size and (i.address + i.size) in self.addrs:
                self.labels.add(i.address + i.size)
        last = None
        for k, i in enumerate(self.ins):
            if last is not None and not self.ends_block(last) and i.address != last.address + last.size:
                ft = last.address + last.size
                self.e("goto L_%x;" % ft if ft in self.addrs else self.tail(ft, last.address))
            self.cur = i
            if i.address in self.labels:
                f.append("L_%x:;" % i.address)
            try:
                self.insn(i)
                if self.no_return(i): self.e("return;   /* that call does not come back */")
            except Exception as ex:
                self.e("bad_insn(c, 0x%x); /* %s %s : %s */" % (i.address, i.mnemonic, i.op_str, ex))
                self.tr.unsupported[i.mnemonic] = self.tr.unsupported.get(i.mnemonic, 0) + 1
            last = i
        if last is not None and not self.ends_block(last):
            ft = last.address + last.size
            self.e(self.tail(ft, last.address))
        for a in self.bad_decode:
            self.tr.unsupported["<undecodable>"] = self.tr.unsupported.get("<undecodable>", 0) + 1
        f.append("}")
        return "\n".join(f)

    def jump_table(self, i):
        return [t for t in self.tables.get(i.address, ([], 0))[0] if t in self.addrs]

    # Imports that do not come back. What follows the call is not code of this path (after a landing pad's
    # _Unwind_Resume there is the next pad, or a table): decoding on from it read data as instructions.
    NO_RETURN = {"__Unwind_Resume", "__Unwind_Resume_or_Rethrow", "___cxa_throw", "___cxa_rethrow", "_abort", "_exit",
                 "__exit", "___stack_chk_fail", "___assert_rtn", "__ZSt9terminatev", "___cxa_call_unexpected",
                 "___cxa_bad_cast", "___cxa_bad_typeid", "___cxa_pure_virtual", "_longjmp", "__longjmp", "_siglongjmp"}

    def no_return(self, i):
        if i.mnemonic != "call" or not i.operands or i.operands[0].type != X.X86_OP_IMM: return False
        t = self.img.stub_alias.get(i.operands[0].imm, i.operands[0].imm)
        name = self.img.stubs.get(t)
        if name: return name in self.NO_RETURN or name.startswith("__ZSt") and "__throw_" in name
        return self.local_no_return(t)

    def local_no_return(self, t):
        """A function of the image that is a straight line into a call that does not come back (the compiler's
        __clang_call_terminate: begin_catch, then terminate). After a call to it there is only padding."""
        known = self.tr.noreturn
        if t not in known:
            known[t] = False
            a, straight = t, t in self.img.func_names and t != self.addr
            for _ in range(12 if straight else 0):
                j = self.decode_at(a)
                if j is None or j.group(cs.CS_GRP_JUMP) or j.mnemonic.startswith("ret"): break
                if j.mnemonic == "call" and j.operands and j.operands[0].type == X.X86_OP_IMM:
                    n = self.img.stubs.get(self.img.stub_alias.get(j.operands[0].imm, j.operands[0].imm))
                    if n in self.NO_RETURN: known[t] = True; break
                a = j.address + j.size
        return known[t]

    def decode_at(self, a):
        try: code = self.img.read(a, 16)
        except Exception: return None
        return next(self.tr.md.disasm(code, a, 1), None)

    def ends_block(self, i):
        m = i.mnemonic
        return m in ("ret", "jmp", "ud2", "hlt", "retf", "ljmp") or m.startswith("ret") or self.no_return(i)

    def target(self, t, frm):
        """C statement for transferring control (tail call) to address t."""
        return self.tail(t, frm)

    def tail(self, t, frm):
        t = self.img.stub_alias.get(t, t)
        if t in self.tr.hle:
            return "%s(c); C.esp += 4; return;" % self.tr.hle_name(t)
        if self.tr.lo <= t < self.tr.hi and t in self.img.func_names:
            self.tr.refs.add(t)
            return "F_%x(c); return;" % t
        if t in self.img.stubs:
            return "%s(c); C.esp += 4; return;" % self.tr.imp(self.img.stubs[t])
        return "missing_func(c, 0x%x, 0x%x); return;" % (t, frm)

    # ---------- instructions ----------
    def insn(self, i):
        m, ops = i.mnemonic, i.operands
        nxt = i.address + i.size
        e = self.e
        if m in ("nop", "cld") or m.startswith("nop"):
            return
        if m == "mov":
            e(self.wr(ops[0], self.rd(ops[1], ops[0].size))); return
        if m == "movzx":
            e(self.wr(ops[0], "(uint32_t)" + self.rd(ops[1]))); return
        if m == "movsx":
            e(self.wr(ops[0], "(uint32_t)(int32_t)(%s)%s" % (ST[ops[1].size], self.rd(ops[1])))); return
        if m == "lea":
            e(self.wr(ops[0], self.mem_addr(ops[1]))); return
        if m == "push":
            e("PUSH((uint32_t)%s);" % self.rd(ops[0], 4) if ops[0].type == X.X86_OP_IMM
              else "PUSH(%s);" % self.rd(ops[0])); return
        if m == "pop":
            e("{ uint32_t v = POP(); %s }" % self.wr(ops[0], "v")); return
        if m == "leave":
            e("C.esp = C.ebp; C.ebp = POP();"); return
        if m == "ret":
            n = ops[0].imm if ops else 0
            e("C.esp += %d; return;" % (4 + n)); return
        if m == "cdq":
            e("C.edx = (int32_t)C.eax < 0 ? 0xFFFFFFFFu : 0;"); return
        if m == "ud2":
            e("bad_insn(c, 0x%x); return;" % i.address); return
        if m in ("add", "sub", "cmp", "and", "or", "xor", "test", "adc", "sbb"):
            return self.alu(m, ops)
        if m in ("inc", "dec", "neg", "not"):
            return self.unary(m, ops[0])
        if m in ("shl", "sal", "shr", "sar"):
            return self.shift(m, ops)
        if m in ("imul", "mul", "div", "idiv"):
            return self.muldiv(m, ops)
        if m == "call":
            return self.call(i, ops[0], nxt)
        if m == "jmp":
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm
                e("goto L_%x;" % t if t in self.addrs else self.target(t, i.address))
            else:
                tbl = self.jump_table(i)
                v = self.rd(op, 4)
                if tbl:
                    cases = " ".join("case 0x%x: goto L_%x;" % (t, t) for t in sorted(set(tbl)))
                    e("switch (%s) { %s default: bad_jump(c, %s, 0x%x); return; }" % (v, cases, v, i.address))
                else:  # indirect tail call
                    e("{ uint32_t t = %s; call_ind(c, t); return; }" % v)
            return
        if m.startswith("j"):
            cc = self.cc(m[1:])
            t = ops[0].imm
            e("if (%s) %s" % (cc, "goto L_%x;" % t if t in self.addrs else "{ %s }" % self.target(t, i.address)))
            return
        if m.startswith("set"):
            e(self.wr(ops[0], "(%s) ? 1 : 0" % self.cc(m[3:]))); return
        if m.startswith("cmov"):
            e("if (%s) %s" % (self.cc(m[4:]), self.wr(ops[0], self.rd(ops[1])))); return
        if self.strop(i, m, ops):
            return
        if m == "repe cmpsb":
            e("while (C.ecx) { uint32_t a = rd8(C.esi), b = rd8(C.edi), r = a - b; C.esi++; C.edi++; C.ecx--;"
              " cf = a < b; of = ((a ^ b) & (a ^ r) & 0x80u) != 0; %s if (!zf) break; }" % self.szf("r", 1))
            return
        if self.extra(i, m, ops) or self.sse(m, ops) or self.x87(m, ops):
            return
        raise Unsupported(m)

    def strop(self, i, m, ops):
        mm = re.match(r"(rep |repe |repne )?(movs|stos|lods|scas|cmps)([bwd])$", m)
        if not mm: return False
        if any(o.type == X.X86_OP_REG and (self.rn(o.reg) or "").startswith("xmm") for o in ops):
            return False  # SSE movsd/cmpsd, not the string instructions
        if m == "repe cmpsb": return False
        rep, op, w = mm.group(1), mm.group(2), {"b": 1, "w": 2, "d": 4}[mm.group(3)]
        b = 8 * w
        acc = {1: "(uint8_t)C.eax", 2: "(uint16_t)C.eax", 4: "C.eax"}[w]
        if op == "movs": body = "wr%d(C.edi, rd%d(C.esi)); C.esi += %d; C.edi += %d;" % (b, b, w, w)
        elif op == "stos": body = "wr%d(C.edi, %s); C.edi += %d;" % (b, acc, w)
        elif op == "lods": body = "{ uint32_t v = rd%d(C.esi); C.esi += %d; C.eax = %s; }" % (b, w, {1: "(C.eax & 0xFFFFFF00u) | v", 2: "(C.eax & 0xFFFF0000u) | v", 4: "v"}[w])
        elif op == "scas": body = "{ uint32_t a = %s, bb = rd%d(C.edi), r = (a - bb) & %s; C.edi += %d; cf = a < bb; of = (((a ^ bb) & (a ^ r)) >> %d) & 1; %s }" % (acc, b, MASK[w], w, b - 1, self.szf("r", w))
        else: body = "{ uint32_t a = rd%d(C.esi), bb = rd%d(C.edi), r = (a - bb) & %s; C.esi += %d; C.edi += %d; cf = a < bb; of = (((a ^ bb) & (a ^ r)) >> %d) & 1; %s }" % (b, b, MASK[w], w, w, b - 1, self.szf("r", w))
        if not rep:
            self.e(body)
        elif rep == "rep ":
            if op == "movs" and w == 4: self.e("if (C.ecx) { MC(C.edi, 4 * C.ecx); memmove(M + C.edi, M + C.esi, 4ull * C.ecx); C.esi += 4 * C.ecx; C.edi += 4 * C.ecx; C.ecx = 0; }")
            elif op == "stos" and w == 1: self.e("if (C.ecx) { MC(C.edi, C.ecx); memset(M + C.edi, (uint8_t)C.eax, C.ecx); C.edi += C.ecx; C.ecx = 0; }")
            else: self.e("while (C.ecx) { %s C.ecx--; }" % body)
        else:
            stop = "if (!zf) break;" if rep == "repe " else "if (zf) break;"
            self.e("while (C.ecx) { %s C.ecx--; %s }" % (body, stop))
        return True

    def cc(self, c):
        alias = {"z": "e", "nz": "ne", "c": "b", "nae": "b", "nb": "ae", "nc": "ae", "na": "be",
                 "nbe": "a", "nge": "l", "nl": "ge", "ng": "le", "nle": "g", "pe": "p", "po": "np"}
        c = alias.get(c, c)
        if c not in CC: raise Unsupported("cc " + c)
        return CC[c]

    def alu(self, m, ops):
        d, s = ops
        n = d.size
        sign = 8 * n - 1
        M_ = MASK[n]
        if m == "xor" and d.type == X.X86_OP_REG and s.type == X.X86_OP_REG and d.reg == s.reg:
            self.e(self.wr(d, "0") + " zf = 1; sf = 0; cf = 0; of = 0; pf = 1;"); return
        a, b = self.rd(d), self.rd(s, n)
        st = ["uint32_t a = %s, b = %s, r;" % (a, b)]
        if m in ("add", "adc"):
            st.append("r = (a + b%s) & %s;" % (" + cf" if m == "adc" else "", M_))
            if m == "add": st.append("cf = r < a;")
            else: st.append("cf = (uint64_t)a + b + cf > %s;" % M_)
            st.append("of = (((a ^ r) & (b ^ r)) >> %d) & 1;" % sign)
        elif m in ("sub", "cmp", "sbb"):
            if m == "sbb":
                st.append("r = (a - b - cf) & %s; cf = (uint64_t)a < (uint64_t)b + cf;" % M_)
            else:
                st.append("r = (a - b) & %s; cf = a < b;" % M_)
            st.append("of = (((a ^ b) & (a ^ r)) >> %d) & 1;" % sign)
        else:
            op = {"and": "&", "test": "&", "or": "|", "xor": "^"}[m]
            st.append("r = a %s b; cf = 0; of = 0;" % op)
        st.append(self.szf("r", n))
        if m not in ("cmp", "test"):
            st.append(self.wr(d, "r"))
        self.e("{ " + " ".join(st) + " }")

    def unary(self, m, d):
        n = d.size; sign = 8 * n - 1; M_ = MASK[n]
        a = self.rd(d)
        if m == "not":
            self.e(self.wr(d, "~%s" % a)); return
        if m == "inc":
            body = "r = (a + 1) & %s; of = r == (1u << %d);" % (M_, sign)
        elif m == "dec":
            body = "r = (a - 1) & %s; of = a == (1u << %d);" % (M_, sign)
        else:
            body = "r = (0u - a) & %s; cf = a != 0; of = a == (1u << %d);" % (M_, sign)
        self.e("{ uint32_t a = %s, r; %s %s %s }" % (a, body, self.szf("r", n), self.wr(d, "r")))

    def shift(self, m, ops):
        d = ops[0]; n = d.size; bits = 8 * n; M_ = MASK[n]
        if len(ops) == 1: cnt = "1u"
        elif ops[1].type == X.X86_OP_IMM: cnt = "%du" % (ops[1].imm & 31)
        else: cnt = "(C.ecx & 31u)"
        a = self.rd(d)
        if m in ("shl", "sal"):
            body = "r = (uint32_t)(((uint64_t)a << cc) & %s); cf = ((uint64_t)a >> (%d - cc)) & 1; of = ((r >> %d) & 1) ^ cf;" % (M_, bits, bits - 1)
        elif m == "shr":
            body = "r = a >> cc; cf = (a >> (cc - 1)) & 1; of = (a >> %d) & 1;" % (bits - 1)
        else:
            body = "r = (uint32_t)(((int32_t)(%s)a) >> cc) & %s; cf = (((int32_t)(%s)a) >> (cc - 1)) & 1; of = 0;" % (ST[n], M_, ST[n])
        self.e("{ uint32_t cc = %s; if (cc) { uint32_t a = %s, r; %s %s %s } }" % (cnt, a, body, self.szf("r", n), self.wr(d, "r")))

    def muldiv(self, m, ops):
        e = self.e
        if m == "imul" and len(ops) >= 2:
            d = ops[0]
            a = self.rd(ops[1]) if len(ops) == 3 else self.rd(d)
            b = self.rd(ops[2], d.size) if len(ops) == 3 else self.rd(ops[1], d.size)
            if d.size == 2:
                e("{ int32_t p = (int32_t)(int16_t)%s * (int32_t)(int16_t)%s; uint16_t r = (uint16_t)p; "
                  "cf = of = p != (int32_t)(int16_t)r; %s %s }" % (a, b, self.szf("r", 2), self.wr(d, "r")))
                return
            if d.size != 4: raise Unsupported("imul%d" % (8 * d.size))
            e("{ int64_t p = (int64_t)(int32_t)%s * (int64_t)(int32_t)%s; uint32_t r = (uint32_t)p; "
              "cf = of = p != (int64_t)(int32_t)r; %s %s }" % (a, b, self.szf("r", 4), self.wr(d, "r")))
            return
        s = ops[0]
        if s.size != 4 and m in ("mul", "imul"):   # al * r/m8 -> ax ; ax * r/m16 -> dx:ax
            v, n = self.rd(s), 8 * s.size
            ty = ("int%d_t" if m == "imul" else "uint%d_t") % n
            lo = "C.eax = (C.eax & 0xFFFF0000u) | (uint16_t)p;" if n == 8 else \
                 "C.eax = (C.eax & 0xFFFF0000u) | (uint16_t)p; C.edx = (C.edx & 0xFFFF0000u) | (uint16_t)((uint32_t)p >> 16);"
            over = "p != (int32_t)(%s)p" % ty if m == "imul" else "((uint32_t)p >> %d) != 0" % n
            e("{ int32_t p = (int32_t)(%s)C.eax * (int32_t)(%s)%s; %s cf = of = %s; }" % (ty, ty, v, lo, over))
            return
        if s.size != 4: raise Unsupported(m + "8/16")
        v = self.rd(s)
        if m == "mul":
            e("{ uint64_t p = (uint64_t)C.eax * %s; C.eax = (uint32_t)p; C.edx = (uint32_t)(p >> 32); cf = of = C.edx != 0; }" % v)
        elif m == "imul":
            e("{ int64_t p = (int64_t)(int32_t)C.eax * (int32_t)%s; C.eax = (uint32_t)p; C.edx = (uint32_t)((uint64_t)p >> 32); cf = of = p != (int32_t)C.eax; }" % v)
        elif m == "div":
            e("{ uint64_t nn = ((uint64_t)C.edx << 32) | C.eax; uint32_t dd = %s; C.eax = (uint32_t)(nn / dd); C.edx = (uint32_t)(nn %% dd); }" % v)
        else:
            e("{ int64_t nn = (int64_t)(((uint64_t)C.edx << 32) | C.eax); int32_t dd = (int32_t)%s; C.eax = (uint32_t)(int32_t)(nn / dd); C.edx = (uint32_t)(int32_t)(nn %% dd); }" % v)

    def unwound(self, i, stmt):
        """A call, and what follows it if the callee comes back with a C++ exception in flight: to this function's
        landing pad for the call if its table has one (with the frame's registers as they were), or out."""
        if not self.tr.eh: return stmt
        for a, b, lp, act in self.sites:
            if a <= i.address < b:
                if lp in self.pads and lp in self.addrs:
                    return ("{ uint32_t s_sp = C.esp, s_bp = C.ebp, s_bx = C.ebx, s_si = C.esi, s_di = C.edi; %s "
                            "if (C.exc) { C.esp = s_sp; C.ebp = s_bp; C.ebx = s_bx; C.esi = s_si; C.edi = s_di; "
                            "if (eh_land(c, 0x%xu, %d)) goto L_%x; return; } }" % (stmt, self.lsda, act, lp))
                break
        return stmt + " if (C.exc) return;"

    def call(self, i, op, nxt):
        e = lambda stmt: self.e(self.unwound(i, stmt) if "(c" in stmt and "gjmp_slot" not in stmt else stmt)
        if op.type == X.X86_OP_IMM:
            t = op.imm
            t = self.img.stub_alias.get(t, t)
            if t in self.img.stubs:
                name = self.img.stubs[t]
                if name == "_setjmp":
                    e("{ GJmp *j = gjmp_slot(rd32(C.esp)); gjmp_save(c, j); if (setjmp(j->host)) gjmp_restore(c, j); else C.eax = 0; }")
                else:
                    e("PUSH(0x%xu); %s(c); C.esp += 4;" % (nxt, self.tr.imp(name)))
                return
            if t == nxt:   # PIC idiom: call next; pop reg
                e("PUSH(0x%xu);" % nxt)
            elif t in self.tr.hle:
                e("PUSH(0x%xu); %s(c); C.esp += 4;" % (nxt, self.tr.hle_name(t)))
            elif self.tr.lo <= t < self.tr.hi and t in self.img.func_names:
                self.tr.refs.add(t)
                e("PUSH(0x%xu); F_%x(c);" % (nxt, t))
            else:
                e("PUSH(0x%xu); missing_func(c, 0x%x, 0x%x);" % (nxt, t, i.address))
            return
        e("{ uint32_t t = %s; PUSH(0x%xu); call_ind(c, t); }" % (self.rd(op, 4), nxt))

    # ---------- misc integer / atomic ----------
    def extra(self, i, m, ops):
        e = self.e
        if m in ("rol", "ror"):
            d = ops[0]
            cnt = "1u" if len(ops) == 1 else ("%du" % (ops[1].imm & 31) if ops[1].type == X.X86_OP_IMM else "(C.ecx & 31u)")
            n = 8 * d.size
            if n == 32:
                f = "rol32" if m == "rol" else "ror32"
                e("{ uint32_t cc = %s; if (cc) { uint32_t r = %s(%s, cc); cf = %s; %s } }" % (cnt, f, self.rd(d), "r & 1" if m == "rol" else "r >> 31", self.wr(d, "r")))
            else:
                sh = "(cc %% %d)" % n
                expr = ("(uint32_t)((a << k) | (a >> (%d - k)))" if m == "rol" else "(uint32_t)((a >> k) | (a << (%d - k)))") % n
                e("{ uint32_t cc = %s; uint32_t k = %s; if (k) { uint32_t a = %s; uint32_t r = %s & %s; cf = %s; %s } }" % (cnt, sh, self.rd(d), expr, MASK[d.size], "r & 1" if m == "rol" else "(r >> %d) & 1" % (n - 1), self.wr(d, "r")))
            return True
        if m == "bswap":
            e(self.wr(ops[0], "__builtin_bswap32(%s)" % self.rd(ops[0]))); return True
        if m in ("shld", "shrd"):
            d, s_, k = ops
            cnt = "%du" % (k.imm & 31) if k.type == X.X86_OP_IMM else "(C.ecx & 31u)"
            if d.size != 4: raise Unsupported(m + "16")
            if m == "shld": body = "r = (a << cc) | (b >> (32 - cc)); cf = (a >> (32 - cc)) & 1;"
            else: body = "r = (a >> cc) | (b << (32 - cc)); cf = (a >> (cc - 1)) & 1;"
            e("{ uint32_t cc = %s; if (cc) { uint32_t a = %s, b = %s, r; %s %s %s } }" % (cnt, self.rd(d), self.rd(s_), body, self.szf("r", 4), self.wr(d, "r")))
            return True
        if m == "bt":
            d, b = ops
            if d.type == X.X86_OP_MEM and b.type == X.X86_OP_REG:
                e("{ int32_t bi = (int32_t)%s; cf = (rd8(%s + (uint32_t)(bi >> 3)) >> (bi & 7)) & 1; }" % (self.rd(b), self.mem_addr(d)))
            else:
                e("cf = (%s >> (%s & %d)) & 1;" % (self.rd(d), self.rd(b, d.size), 8 * d.size - 1))
            return True
        if m in ("bsr", "bsf"):
            d, s_ = ops
            f = "31 - __builtin_clz(v)" if m == "bsr" else "__builtin_ctz(v)"
            e("{ uint32_t v = %s; zf = v == 0; if (v) %s }" % (self.rd(s_), self.wr(d, f)))
            return True
        if m == "cwde":
            e("C.eax = (uint32_t)(int32_t)(int16_t)C.eax;"); return True
        if m == "cbw":
            e("C.eax = (C.eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)C.eax;"); return True
        if m == "cwd":
            e("C.edx = (C.edx & 0xFFFF0000u) | (((int16_t)C.eax < 0) ? 0xFFFFu : 0);"); return True
        if m == "xchg":
            a, b = ops
            # With a memory operand the processor locks the exchange whether or not the prefix is written: it is how
            # spinlocks are taken. A plain read-then-write here let two threads both take the lock.
            mem, reg = (a, b) if a.type == X.X86_OP_MEM else (b, a) if b.type == X.X86_OP_MEM else (None, None)
            if mem is not None:
                ty = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}[mem.size]
                e("{ %s t = __atomic_exchange_n((%s *)(M + %s), (%s)%s, __ATOMIC_SEQ_CST); %s }"
                  % (ty, ty, self.mem_addr(mem), ty, self.rd(reg), self.wr(reg, "t")))
                return True
            e("{ uint32_t t = %s; %s %s }" % (self.rd(a), self.wr(a, self.rd(b)), self.wr(b, "t")))
            return True
        if m == "pushfd":
            e("PUSH(0x202u | cf | (pf << 2) | (zf << 6) | (sf << 7) | (of << 11));"); return True
        if m == "popfd":
            e("{ uint32_t f = POP(); cf = f & 1; pf = (f >> 2) & 1; zf = (f >> 6) & 1; sf = (f >> 7) & 1; of = (f >> 11) & 1; }"); return True
        if m in ("clc", "stc", "cmc"):
            e({"clc": "cf = 0;", "stc": "cf = 1;", "cmc": "cf ^= 1;"}[m]); return True
        if m in ("emms", "wait", "fwait", "lfence", "mfence", "sfence", "pause"):
            return True
        if m == "rdtsc":
            e("{ uint64_t t = __builtin_readcyclecounter(); C.eax = (uint32_t)t; C.edx = (uint32_t)(t >> 32); }"); return True
        if m == "cpuid":
            e("cpu_cpuid(c);"); self.tr.need_cpuid = True; return True
        if m.startswith("lock "):
            op = m[5:]
            d = ops[0]
            if op == "cmpxchg8b" and d.type == X.X86_OP_MEM:
                # edx:eax against the 64 bits in memory: equal -> ecx:ebx goes in (zf); else memory comes back in edx:eax
                e("{ uint64_t exp = ((uint64_t)C.edx << 32) | C.eax, v = ((uint64_t)C.ecx << 32) | C.ebx; "
                  "zf = __atomic_compare_exchange_n((uint64_t *)(M + %s), &exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); "
                  "if (!zf) { C.eax = (uint32_t)exp; C.edx = (uint32_t)(exp >> 32); } }" % self.mem_addr(d))
                return True
            if d.type != X.X86_OP_MEM or d.size != 4: raise Unsupported(m)
            p = "(uint32_t *)(M + %s)" % self.mem_addr(d)
            if op == "xadd":
                e("{ uint32_t b = %s; uint32_t a = __atomic_fetch_add(%s, b, __ATOMIC_SEQ_CST); uint32_t r = a + b; cf = r < a; of = (((a ^ r) & (b ^ r)) >> 31) & 1; %s %s }" % (self.rd(ops[1]), p, self.szf("r", 4), self.wr(ops[1], "a")))
            elif op in ("inc", "dec", "add", "sub"):
                b = "1u" if op in ("inc", "dec") else self.rd(ops[1], 4)
                fn = "__atomic_add_fetch" if op in ("inc", "add") else "__atomic_sub_fetch"
                e("{ uint32_t r = %s(%s, %s, __ATOMIC_SEQ_CST); %s }" % (fn, p, b, self.szf("r", 4)))
            elif op == "cmpxchg":
                e("{ uint32_t exp = C.eax; zf = __atomic_compare_exchange_n(%s, &exp, %s, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); if (!zf) C.eax = exp; }" % (p, self.rd(ops[1])))
            elif op in ("or", "and", "xor"):
                fn = {"or": "__atomic_or_fetch", "and": "__atomic_and_fetch", "xor": "__atomic_xor_fetch"}[op]
                e("{ uint32_t r = %s(%s, %s, __ATOMIC_SEQ_CST); cf = of = 0; %s }" % (fn, p, self.rd(ops[1], 4), self.szf("r", 4)))
            else:
                raise Unsupported(m)
            return True
        return False

    # ---------- SSE ----------
    def sse(self, m, ops):
        e = self.e
        def src_f(op):
            return "%s.f[0]" % self.xmm(op) if op.type == X.X86_OP_REG else "rdf(%s)" % self.mem_addr(op)
        def src_d(op):
            return "%s.d[0]" % self.xmm(op) if op.type == X.X86_OP_REG else "rdd(%s)" % self.mem_addr(op)
        def src_x(op):
            return self.xmm(op) if op.type == X.X86_OP_REG else "rdx(%s)" % self.mem_addr(op)
        d = ops[0] if ops else None
        if m in ("movss", "movsd"):
            fl, ty, w = ("f", "float", "wrf") if m == "movss" else ("d", "double", "wrd")
            s = ops[1]
            if d.type == X.X86_OP_MEM:
                e("%s(%s, %s.%s[0]);" % (w, self.mem_addr(d), self.xmm(s), fl))
            elif s.type == X.X86_OP_MEM:
                e("{ XMM x = {0}; x.%s[0] = %s; %s = x; }" % (fl, src_f(s) if fl == "f" else src_d(s), self.xmm(d)))
            else:
                e("%s.%s[0] = %s.%s[0];" % (self.xmm(d), fl, self.xmm(s), fl))
            return True
        if m in ("movaps", "movups", "movdqa", "movdqu", "movapd"):
            s = ops[1]
            if d.type == X.X86_OP_MEM: e("wrx(%s, %s);" % (self.mem_addr(d), self.xmm(s)))
            else: e("%s = %s;" % (self.xmm(d), src_x(s)))
            return True
        if m == "movd":
            s = ops[1]
            if d.type == X.X86_OP_REG and self.rn(d.reg).startswith("xmm"):
                e("{ XMM x = {0}; x.u[0] = %s; %s = x; }" % (self.rd(s, 4), self.xmm(d)))
            else:
                e(self.wr(d, "%s.u[0]" % self.xmm(s)))
            return True
        def isx(op): return op.type == X.X86_OP_REG and (self.rn(op.reg) or "").startswith("xmm")
        mc = re.match(r"cmp(eq|lt|le|unord|neq|nlt|nle|ord)(ss|sd)$", m)
        if mc:
            pred, ty = mc.groups()
            a_, b_ = ("%s.f[0]" % self.xmm(d), src_f(ops[1])) if ty == "ss" else ("%s.d[0]" % self.xmm(d), src_d(ops[1]))
            cond = {"eq": "a == b", "lt": "a < b", "le": "a <= b", "unord": "a != a || b != b",
                    "neq": "!(a == b)", "nlt": "!(a < b)", "nle": "!(a <= b)", "ord": "a == a && b == b"}[pred]
            if ty == "ss": e("{ float a = %s, b = %s; %s.u[0] = (%s) ? 0xFFFFFFFFu : 0; }" % (a_, b_, self.xmm(d), cond))
            else: e("{ double a = %s, b = %s; %s.q[0] = (%s) ? ~0ull : 0; }" % (a_, b_, self.xmm(d), cond))
            return True
        if m in ("sqrtsd",):
            e("%s.d[0] = sqrt(%s);" % (self.xmm(d), src_d(ops[1]))); return True
        if m in ("minsd", "maxsd"):
            o = "<" if m == "minsd" else ">"
            e("{ double a = %s.d[0], b = %s; %s.d[0] = (a %s b) ? a : b; }" % (self.xmm(d), src_d(ops[1]), self.xmm(d), o)); return True
        if m == "cvtdq2pd":
            e("{ XMM s = %s; %s.d[0] = (double)s.i[0]; %s.d[1] = (double)s.i[1]; }" % (src_x(ops[1]) if isx(ops[1]) else "rdx(%s)" % self.mem_addr(ops[1]), self.xmm(d), self.xmm(d))); return True
        if m == "cvttpd2dq":
            e("{ XMM s = %s; %s.i[0] = cvtt_i32(s.d[0]); %s.i[1] = cvtt_i32(s.d[1]); %s.q[1] = 0; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d), self.xmm(d))); return True
        if m == "cvtsd2si":
            e(self.wr(d, "(uint32_t)cvtr_i32(%s)" % src_d(ops[1]))); return True   # out of range or NaN: 0x80000000
        if m == "cvtss2si":
            e(self.wr(d, "(uint32_t)cvtr_i32((double)%s)" % src_f(ops[1]))); return True
        mp = re.match(r"(add|sub|mul|div)pd$", m)
        if mp:
            o = {"add": "+", "sub": "-", "mul": "*", "div": "/"}[mp.group(1)]
            e("{ XMM s = %s; %s.d[0] %s= s.d[0]; %s.d[1] %s= s.d[1]; }" % (src_x(ops[1]), self.xmm(d), o, self.xmm(d), o)); return True
        if m == "movq" and (isx(d) or isx(ops[1])):
            s_ = ops[1]
            if isx(d): e("{ XMM x = {0}; x.q[0] = %s; %s = x; }" % ("%s.q[0]" % self.xmm(s_) if isx(s_) else "rd64(%s)" % self.mem_addr(s_), self.xmm(d)))
            else: e("wr64(%s, %s.q[0]);" % (self.mem_addr(d), self.xmm(s_)))
            return True
        if m in ("movhpd", "movhps", "movlpd", "movlps"):
            k = 1 if "h" in m[3] else 0
            if d.type == X.X86_OP_MEM: e("wr64(%s, %s.q[%d]);" % (self.mem_addr(d), self.xmm(ops[1]), k))
            else: e("%s.q[%d] = rd64(%s);" % (self.xmm(d), k, self.mem_addr(ops[1])))
            return True
        if m == "punpckldq" and isx(d):
            e("{ XMM s = %s, x; x.u[0] = %s.u[0]; x.u[1] = s.u[0]; x.u[2] = %s.u[1]; x.u[3] = s.u[1]; %s = x; }" % (src_x(ops[1]), self.xmm(d), self.xmm(d), self.xmm(d))); return True
        if m in ("psllq", "psrlq") and isx(d) and ops[1].type == X.X86_OP_IMM:
            k = ops[1].imm; o = "<<" if m == "psllq" else ">>"
            e("for (int k = 0; k < 2; k++) %s.q[k] = %s;" % (self.xmm(d), "0" if k > 63 else "%s.q[k] %s %d" % (self.xmm(d), o, k))); return True
        if m in ("psrld", "psrad") and isx(d) and ops[1].type == X.X86_OP_IMM:
            k = ops[1].imm
            ex = ("%s.u[k] >> %d" if m == "psrld" else "(uint32_t)(%s.i[k] >> %d)") % (self.xmm(d), min(k, 31))
            e("for (int k = 0; k < 4; k++) %s.u[k] = %s;" % (self.xmm(d), "0" if (k > 31 and m == "psrld") else ex)); return True
        if m in ("xorps", "andps", "orps", "por", "pxor", "pand", "andnps", "andnpd", "xorpd", "andpd", "orpd") and not isx(d):
            raise Unsupported("mmx " + m)
        ar = {"add": "+", "sub": "-", "mul": "*", "div": "/"}
        mm = re.match(r"(add|sub|mul|div)(ss|sd|ps)$", m)
        if mm:
            o = ar[mm.group(1)]
            if mm.group(2) == "ss": e("%s.f[0] = %s.f[0] %s %s;" % (self.xmm(d), self.xmm(d), o, src_f(ops[1])))
            elif mm.group(2) == "sd": e("%s.d[0] = %s.d[0] %s %s;" % (self.xmm(d), self.xmm(d), o, src_d(ops[1])))
            else: e("{ XMM s = %s; for (int k = 0; k < 4; k++) %s.f[k] %s= s.f[k]; }" % (src_x(ops[1]), self.xmm(d), o))
            return True
        if m in ("minss", "maxss"):
            o = "<" if m == "minss" else ">"
            e("{ float a = %s.f[0], b = %s; %s.f[0] = (a %s b) ? a : b; }" % (self.xmm(d), src_f(ops[1]), self.xmm(d), o))
            return True
        if m == "sqrtss":
            e("%s.f[0] = sqrtf(%s);" % (self.xmm(d), src_f(ops[1]))); return True
        if m in ("ucomiss", "comiss", "ucomisd", "comisd"):
            a, b = ("%s.f[0]" % self.xmm(d), src_f(ops[1])) if m.endswith("ss") else ("%s.d[0]" % self.xmm(d), src_d(ops[1]))
            e("{ double a = %s, b = %s; if (a != a || b != b) { zf = pf = cf = 1; } else { zf = a == b; pf = 0; cf = a < b; } of = sf = 0; }" % (a, b))
            return True
        if m == "cmpless":
            e("%s.u[0] = (%s.f[0] <= %s) ? 0xFFFFFFFFu : 0;" % (self.xmm(d), self.xmm(d), src_f(ops[1]))); return True
        if m == "cvtss2sd":
            e("%s.d[0] = (double)%s;" % (self.xmm(d), src_f(ops[1]))); return True
        if m == "cvtsd2ss":
            e("%s.f[0] = (float)%s;" % (self.xmm(d), src_d(ops[1]))); return True
        if m in ("cvtsi2ss", "cvtsi2sd"):
            fl, ty = ("f", "float") if m.endswith("ss") else ("d", "double")
            e("%s.%s[0] = (%s)(int32_t)%s;" % (self.xmm(d), fl, ty, self.rd(ops[1], 4))); return True
        if m in ("cvttss2si", "cvttsd2si"):
            s = src_f(ops[1]) if m == "cvttss2si" else src_d(ops[1])
            e(self.wr(d, "cvtt_i32((double)%s)" % s)); return True
        if m == "cvttps2dq":
            e("{ XMM s = %s; for (int k = 0; k < 4; k++) %s.u[k] = cvtt_i32(s.f[k]); }" % (src_x(ops[1]), self.xmm(d))); return True
        if m in ("xorps", "andps", "orps", "por", "pxor", "pand", "andnps", "andnpd", "xorpd", "andpd", "orpd"):
            if m in ("xorps", "pxor", "xorpd") and ops[1].type == X.X86_OP_REG and ops[1].reg == d.reg:
                e("memset(&%s, 0, 16);" % self.xmm(d)); return True
            o = "&" if "and" in m else "|" if "or" in m and "xor" not in m else "^"
            pre = "~" if m in ("andnps", "andnpd") else ""
            e("{ XMM s = %s; for (int k = 0; k < 2; k++) %s.q[k] = (%s%s.q[k]) %s s.q[k]; }" % (src_x(ops[1]), self.xmm(d), pre, self.xmm(d), o))
            return True
        if m == "paddd":
            e("{ XMM s = %s; for (int k = 0; k < 4; k++) %s.u[k] += s.u[k]; }" % (src_x(ops[1]), self.xmm(d))); return True
        if m == "pslld" and ops[1].type == X.X86_OP_IMM:
            c = ops[1].imm
            e("for (int k = 0; k < 4; k++) %s.u[k] = %s;" % (self.xmm(d), "0" if c > 31 else "%s.u[k] << %d" % (self.xmm(d), c))); return True
        return self.packed(m, ops)

    # ---------- packed SSE: every lane of the register ----------
    # name: (field of XMM, lanes, value of lane k from a (destination) and s (source))
    LANES = {
        "paddb": ("b", 16, "a.b[k] + s.b[k]"), "psubb": ("b", 16, "a.b[k] - s.b[k]"),
        "paddw": ("w", 8, "a.w[k] + s.w[k]"), "psubw": ("w", 8, "a.w[k] - s.w[k]"),
        "psubd": ("u", 4, "a.u[k] - s.u[k]"), "paddq": ("q", 2, "a.q[k] + s.q[k]"), "psubq": ("q", 2, "a.q[k] - s.q[k]"),
        "paddusb": ("b", 16, "a.b[k] + s.b[k] > 0xFF ? 0xFF : a.b[k] + s.b[k]"),
        "paddusw": ("w", 8, "a.w[k] + s.w[k] > 0xFFFF ? 0xFFFF : a.w[k] + s.w[k]"),
        "psubusb": ("b", 16, "a.b[k] > s.b[k] ? a.b[k] - s.b[k] : 0"),
        "psubusw": ("w", 8, "a.w[k] > s.w[k] ? a.w[k] - s.w[k] : 0"),
        "pmullw": ("w", 8, "(uint16_t)((int16_t)a.w[k] * (int16_t)s.w[k])"),
        "pmulhw": ("w", 8, "(uint16_t)(((int32_t)(int16_t)a.w[k] * (int16_t)s.w[k]) >> 16)"),
        "pmulhuw": ("w", 8, "(uint16_t)(((uint32_t)a.w[k] * s.w[k]) >> 16)"),
        "pminub": ("b", 16, "a.b[k] < s.b[k] ? a.b[k] : s.b[k]"), "pmaxub": ("b", 16, "a.b[k] > s.b[k] ? a.b[k] : s.b[k]"),
        "pminsw": ("w", 8, "(int16_t)a.w[k] < (int16_t)s.w[k] ? a.w[k] : s.w[k]"),
        "pmaxsw": ("w", 8, "(int16_t)a.w[k] > (int16_t)s.w[k] ? a.w[k] : s.w[k]"),
        "pavgb": ("b", 16, "(a.b[k] + s.b[k] + 1) >> 1"), "pavgw": ("w", 8, "(a.w[k] + s.w[k] + 1) >> 1"),
        "pcmpeqb": ("b", 16, "a.b[k] == s.b[k] ? 0xFF : 0"), "pcmpeqw": ("w", 8, "a.w[k] == s.w[k] ? 0xFFFF : 0"),
        "pcmpeqd": ("u", 4, "a.u[k] == s.u[k] ? 0xFFFFFFFFu : 0"),
        "pcmpgtb": ("b", 16, "(int8_t)a.b[k] > (int8_t)s.b[k] ? 0xFF : 0"),
        "pcmpgtw": ("w", 8, "(int16_t)a.w[k] > (int16_t)s.w[k] ? 0xFFFF : 0"),
        "pcmpgtd": ("u", 4, "a.i[k] > s.i[k] ? 0xFFFFFFFFu : 0"),
        # the source when either is a NaN, and when both are zero
        "maxps": ("f", 4, "a.f[k] > s.f[k] ? a.f[k] : s.f[k]"), "minps": ("f", 4, "a.f[k] < s.f[k] ? a.f[k] : s.f[k]"),
        "maxpd": ("d", 2, "a.d[k] > s.d[k] ? a.d[k] : s.d[k]"), "minpd": ("d", 2, "a.d[k] < s.d[k] ? a.d[k] : s.d[k]"),
        "sqrtps": ("f", 4, "sqrtf(s.f[k])"), "sqrtpd": ("d", 2, "sqrt(s.d[k])"),
        "rsqrtps": ("f", 4, "rsqrt_f(s.f[k])"), "rcpps": ("f", 4, "rcp_f(s.f[k])"),
        "cvtdq2ps": ("f", 4, "(float)s.i[k]"), "cvtps2dq": ("i", 4, "cvtr_i32(s.f[k])"),
    }
    # name: (field, lanes, 1 for the upper half): the lanes of a and s, taken in turn
    INTERLEAVE = {
        "punpcklbw": ("b", 16, 0), "punpckhbw": ("b", 16, 1), "punpcklwd": ("w", 8, 0), "punpckhwd": ("w", 8, 1),
        "punpckldq": ("u", 4, 0), "punpckhdq": ("u", 4, 1), "punpcklqdq": ("q", 2, 0), "punpckhqdq": ("q", 2, 1),
        "unpcklps": ("u", 4, 0), "unpckhps": ("u", 4, 1), "unpcklpd": ("q", 2, 0), "unpckhpd": ("q", 2, 1),
    }
    PREDICATE = {"eq": "a == b", "lt": "a < b", "le": "a <= b", "unord": "a != a || b != b",
                 "neq": "!(a == b)", "nlt": "!(a < b)", "nle": "!(a <= b)", "ord": "a == a && b == b"}

    def packed(self, m, ops):
        e = self.e
        if m.startswith("prefetch") or m in ("lfence", "mfence", "sfence", "pause"): return True
        if not ops: return False
        def isx(op): return op.type == X.X86_OP_REG and (self.rn(op.reg) or "").startswith("xmm")
        def src(op): return self.xmm(op) if isx(op) else "rdx(%s)" % self.mem_addr(op)
        d = ops[0]
        imm = ops[-1].imm & 0xFF if ops[-1].type == X.X86_OP_IMM else None
        if m in ("ldmxcsr", "stmxcsr"):
            a = self.mem_addr(d)
            e("cpu_set_mxcsr(c, rd32(%s));" % a if m == "ldmxcsr" else "wr32(%s, C.mxcsr);" % a); return True
        # ---- into a general register ----
        if m in ("movmskps", "movmskpd", "pmovmskb") and isx(ops[1]):
            v = {"movmskps": "(s.u[0] >> 31) | (s.u[1] >> 31) << 1 | (s.u[2] >> 31) << 2 | (s.u[3] >> 31) << 3",
                 "movmskpd": "(uint32_t)(s.q[0] >> 63) | (uint32_t)(s.q[1] >> 63) << 1", "pmovmskb": "r"}[m]
            pre = "uint32_t r = 0; for (int k = 0; k < 16; k++) r |= (uint32_t)(s.b[k] >> 7) << k; " if m == "pmovmskb" else ""
            e("{ XMM s = %s; %s%s }" % (self.xmm(ops[1]), pre, self.wr(d, v))); return True
        if m == "pextrw" and isx(ops[1]) and not isx(d):
            e(self.wr(d, "(uint32_t)%s.w[%d]" % (self.xmm(ops[1]), imm & 7))); return True
        if not isx(d):
            if m in ("movupd", "movntps", "movntpd", "movntdq") and d.type == X.X86_OP_MEM and isx(ops[1]):
                e("wrx(%s, %s);" % (self.mem_addr(d), self.xmm(ops[1]))); return True
            return False   # MMX registers and the rest
        D = self.xmm(d)
        if len(ops) < 2: return False
        s1 = ops[1]
        if m in ("movupd", "lddqu"):
            e("%s = %s;" % (D, src(s1))); return True
        if m == "pinsrw":
            e("%s.w[%d] = (uint16_t)(%s);" % (D, imm & 7, self.rd(s1, 4) if s1.type == X.X86_OP_REG else self.rd(s1, 2))); return True
        # ---- one lane ----
        if m in ("rsqrtss", "rcpss"):
            v = "%s.f[0]" % self.xmm(s1) if isx(s1) else "rdf(%s)" % self.mem_addr(s1)
            e("%s.f[0] = %s(%s);" % (D, "rsqrt_f" if m == "rsqrtss" else "rcp_f", v)); return True
        # ---- two halves that are not the whole of the source ----
        if m == "movhlps": e("%s.q[0] = %s.q[1];" % (D, self.xmm(s1))); return True
        if m == "movlhps": e("%s.q[1] = %s.q[0];" % (D, self.xmm(s1))); return True
        if m == "cvtps2pd":   # two floats: eight bytes of memory, not sixteen
            if isx(s1): e("{ XMM s = %s; %s.d[0] = s.f[0]; %s.d[1] = s.f[1]; }" % (self.xmm(s1), D, D))
            else: e("{ uint32_t p = %s; float f0 = rdf(p), f1 = rdf(p + 4); %s.d[0] = f0; %s.d[1] = f1; }" % (self.mem_addr(s1), D, D))
            return True
        if m == "pshuflw" or m == "pshufhw":
            o = 4 if m == "pshufhw" else 0
            e("{ XMM s = %s, x = s; %s %s = x; }" % (src(s1), " ".join("x.w[%d] = s.w[%d];" % (o + k, o + ((imm >> 2 * k) & 3)) for k in range(4)), D))
            return True
        if m == "pshufd":
            e("{ XMM s = %s, x; %s %s = x; }" % (src(s1), " ".join("x.u[%d] = s.u[%d];" % (k, (imm >> 2 * k) & 3) for k in range(4)), D))
            return True
        def both(body):   # a: the destination as it was, s: the source, x: the result
            e("{ XMM a = %s, s = %s, x = a; %s %s = x; }" % (D, src(s1), body, D))
        if m in self.LANES:
            f, n, v = self.LANES[m]
            both("for (int k = 0; k < %d; k++) x.%s[k] = %s;" % (n, f, v)); return True
        if m in self.INTERLEAVE:
            f, n, hi = self.INTERLEAVE[m]
            both("for (int k = 0; k < %d; k++) { x.%s[2 * k] = a.%s[k + %d]; x.%s[2 * k + 1] = s.%s[k + %d]; }"
                 % (n // 2, f, f, hi * n // 2, f, f, hi * n // 2)); return True
        if m == "pandn":
            both("x.q[0] = ~a.q[0] & s.q[0]; x.q[1] = ~a.q[1] & s.q[1];"); return True
        if m == "shufps":
            both("x.u[0] = a.u[%d]; x.u[1] = a.u[%d]; x.u[2] = s.u[%d]; x.u[3] = s.u[%d];"
                 % (imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3)); return True
        if m == "shufpd":
            both("x.q[0] = a.q[%d]; x.q[1] = s.q[%d];" % (imm & 1, (imm >> 1) & 1)); return True
        if m == "pmuludq":
            both("x.q[0] = (uint64_t)a.u[0] * s.u[0]; x.q[1] = (uint64_t)a.u[2] * s.u[2];"); return True
        if m == "pmaddwd":
            both("for (int k = 0; k < 4; k++) x.i[k] = (int16_t)a.w[2 * k] * (int16_t)s.w[2 * k] + (int16_t)a.w[2 * k + 1] * (int16_t)s.w[2 * k + 1];")
            return True
        if m in ("packsswb", "packuswb", "packssdw"):
            f, g, n, lo, hi = {"packsswb": ("b", "(int16_t)%s.w", 8, -128, 127), "packuswb": ("b", "(int16_t)%s.w", 8, 0, 255),
                               "packssdw": ("w", "%s.i", 4, -32768, 32767)}[m]
            both("for (int k = 0; k < %d; k++) { int32_t p = %s[k], q = %s[k]; x.%s[k] = p < %d ? %d : p > %d ? %d : p;"
                 " x.%s[k + %d] = q < %d ? %d : q > %d ? %d : q; }" % (n, g % "a", g % "s", f, lo, lo, hi, hi, f, n, lo, lo, hi, hi))
            return True
        h = re.match(r"(p?)h(add|sub)(ps|pd|d|w)$", m)
        if h:
            f, n = {"ps": ("f", 4), "pd": ("d", 2), "d": ("u", 4), "w": ("w", 8)}[h.group(3)]
            o = "+" if h.group(2) == "add" else "-"
            both("for (int k = 0; k < %d; k++) { x.%s[k] = a.%s[2 * k] %s a.%s[2 * k + 1]; x.%s[k + %d] = s.%s[2 * k] %s s.%s[2 * k + 1]; }"
                 % (n // 2, f, f, o, f, f, n // 2, f, o, f)); return True
        cp = re.match(r"cmp(eq|lt|le|unord|neq|nlt|nle|ord)?(ps|pd)$", m)
        if cp and (cp.group(1) or (imm is not None and imm < 8)):
            pred = cp.group(1) or ["eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"][imm]
            f, n, g, ones, ty = ("f", 4, "u", "0xFFFFFFFFu", "float") if cp.group(2) == "ps" else ("d", 2, "q", "~0ull", "double")
            both("for (int k = 0; k < %d; k++) { %s a = %s.%s[k], b = s.%s[k]; x.%s[k] = (%s) ? %s : 0; }"
                 % (n, ty, D, f, f, g, self.PREDICATE[pred], ones)); return True
        if m == "cvtpd2ps":
            both("x.f[0] = (float)s.d[0]; x.f[1] = (float)s.d[1]; x.q[1] = 0;"); return True
        if m == "cvtpd2dq":
            both("x.i[0] = cvtr_i32(s.d[0]); x.i[1] = cvtr_i32(s.d[1]); x.q[1] = 0;"); return True
        if m == "pshufb":
            both("for (int k = 0; k < 16; k++) x.b[k] = (s.b[k] & 0x80) ? 0 : a.b[s.b[k] & 15];"); return True
        if m == "palignr":
            both("for (int k = 0; k < 16; k++) { int j = k + %d; x.b[k] = j < 16 ? s.b[j] : j < 32 ? a.b[j - 16] : 0; }" % imm); return True
        if m in ("pslldq", "psrldq") and imm is not None:
            j = "k - %d" % imm if m == "pslldq" else "k + %d" % imm
            e("{ XMM a = %s, x; for (int k = 0; k < 16; k++) { int j = %s; x.b[k] = (j >= 0 && j < 16) ? a.b[j] : 0; } %s = x; }" % (D, j, D))
            return True
        sh = re.match(r"ps(ll|rl|ra)(w|d|q)$", m)
        if sh and not (sh.group(1) == "ra" and sh.group(2) == "q"):
            bits, f, n, st = {"w": (16, "w", 8, "int16_t"), "d": (32, "u", 4, "int32_t"), "q": (64, "q", 2, "int64_t")}[sh.group(2)]
            cnt = "%du" % imm if s1.type == X.X86_OP_IMM else "%s.q[0]" % ("s")
            pre = "" if s1.type == X.X86_OP_IMM else "XMM s = %s; " % src(s1)
            if sh.group(1) == "ra": v = "(%s)a.%s[k] >> (n > %d ? %d : n)" % (st, f, bits - 1, bits - 1)
            else: v = "n > %d ? 0 : a.%s[k] %s n" % (bits - 1, f, "<<" if sh.group(1) == "ll" else ">>")
            e("{ %sXMM a = %s, x; uint64_t n = %s; for (int k = 0; k < %d; k++) x.%s[k] = %s; %s = x; }" % (pre, D, cnt, n, f, v, D))
            return True
        return False

    # ---------- x87 ----------
    def x87(self, m, ops):
        e = self.e
        if m == "fldz": e("FPUSH(0.0);"); return True
        if m == "fld1": e("FPUSH(1.0);"); return True
        if m in ("fld", "fstp", "fst"):
            op = ops[0]
            if op.type == X.X86_OP_MEM:
                a = self.mem_addr(op)
                rdm = {4: "rdf", 8: "rdd"}.get(op.size)
                wrm = {4: "wrf", 8: "wrd"}.get(op.size)
                if op.size == 10 and m != "fst":   # the stack is kept at double precision
                    e("FPUSH(rdt(%s));" % a if m == "fld" else "wrt(%s, ST(0)); FPOP();" % a); return True
                if not rdm: raise Unsupported("x87 m%d" % (8 * op.size))
                if m == "fld": e("FPUSH(%s(%s));" % (rdm, a))
                else: e("%s(%s, (%s)ST(0));%s" % (wrm, a, "float" if op.size == 4 else "double", " FPOP();" if m == "fstp" else ""))
            else:
                k = int(self.rn(op.reg)[3:-1]) if "(" in self.rn(op.reg) else 0
                if m == "fld": e("FPUSH(ST(%d));" % k)
                else: e("ST(%d) = ST(0);%s" % (k, " FPOP();" if m == "fstp" else ""))
            return True
        if m == "fnstcw": e("wr16(%s, C.fcw);" % self.mem_addr(ops[0])); return True
        if m == "fldcw": e("C.fcw = rd16(%s);" % self.mem_addr(ops[0])); return True
        if m == "fxch":
            k = int(self.rn(ops[0].reg)[3:-1]) if ops and "(" in (self.rn(ops[0].reg) or "") else 1
            e("{ double t = ST(0); ST(0) = ST(%d); ST(%d) = t; }" % (k, k)); return True
        if m == "fchs": e("ST(0) = -ST(0);"); return True
        if m == "fabs": e("ST(0) = fabs(ST(0));"); return True
        fa = re.match(r"f(add|sub|subr|mul|div|divr)(p?)$", m)
        if fa:
            op, pop = fa.groups()
            expr = {"add": "x + y", "sub": "x - y", "subr": "y - x", "mul": "x * y", "div": "x / y", "divr": "y / x"}[op]
            if len(ops) == 1 and ops[0].type == X.X86_OP_MEM:
                rdm = {4: "rdf", 8: "rdd"}[ops[0].size]
                e("{ double x = ST(0), y = %s(%s); ST(0) = %s; }" % (rdm, self.mem_addr(ops[0]), expr))
            else:
                regs = [int(self.rn(o.reg)[3:-1]) if "(" in self.rn(o.reg) else 0 for o in ops] or [1, 0]
                if len(regs) == 1: regs = [0, regs[0]]
                dst, srci = regs
                e("{ double x = ST(%d), y = ST(%d); ST(%d) = %s;%s }" % (dst, srci, dst, expr, " FPOP();" if pop else ""))
            return True
        if m == "fild" and ops[0].type == X.X86_OP_MEM:
            e("FPUSH((double)(%s)%s);" % (ST[ops[0].size], self.rd(ops[0]))); return True
        if m == "fistp" and ops[0].type == X.X86_OP_MEM:
            e("%s FPOP();" % self.wr(ops[0], "(%s)fpu_round(c, ST(0))" % ST[ops[0].size])); return True
        if m == "fist" and ops[0].type == X.X86_OP_MEM:
            e(self.wr(ops[0], "(%s)fpu_round(c, ST(0))" % ST[ops[0].size])); return True
        if m == "fisttp" and ops[0].type == X.X86_OP_MEM:   # towards zero, whatever the control word says
            v = "cvtt_i64(ST(0))" if ops[0].size == 8 else "(%s)cvtt_i32(ST(0))" % ST[ops[0].size]
            e("%s FPOP();" % self.wr(ops[0], v)); return True
        return False


class Translator:
    noreturn = {}    # functions of the image known (or not) to never return
    eh = True        # translate landing pads and test for an exception after every call (--no-eh turns it off)
    pads_out = 0     # landing pads that lie outside their function, which are not reached

    def __init__(self, img, lo, hi, hle_path=None):
        self.img, self.lo, self.hi = img, lo, hi
        self.md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
        self.md.detail = True
        self.unsupported = {}
        self.imports = set()
        self.refs = set()
        self.hle = {}
        for line in (open(hle_path) if hle_path else []):
            line = line.split("#")[0].split()
            if len(line) >= 2:
                a = int(line[0], 16)
                if "outside" in line[2:] and lo <= a < hi: continue
                self.hle[a] = line[1]

    def hle_name(self, t):
        return "H_" + cname(self.hle[t])

    def discover(self):
        """Functions outside __text: the targets of direct calls and jumps that land in another section of __TEXT,
        and then whatever those call there. Returns how many were added."""
        img = self.img
        if not img.other_text: return 0
        lo, hi = img.other_text[0][0], img.other_text[-1][1]
        lite = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
        work = []
        for a in list(img.func_addrs):
            end = img.func_end(a)
            code = img.read(a, end - a)
            p = a
            while p < end:   # a plain sweep: whatever does not decode is stepped over
                n = 0
                for address, size, mnemonic, op_str in lite.disasm_lite(code[p - a:], p):
                    n = address + size
                    if mnemonic in ("call", "jmp") and op_str.startswith("0x"):
                        t = int(op_str, 16)
                        if lo <= t < hi and img.add_function(t): work.append(t)   # from __text: a call or a tail call
                p = max(n, p + 1)
        added = len(work)
        while work:
            g = FuncGen(self, work.pop())
            for i in g.ins:   # inside the section a jump is a branch, and `call next` is how the code finds itself
                if i.mnemonic == "call" and i.operands and i.operands[0].type == X.X86_OP_IMM:
                    t = i.operands[0].imm
                    if t != i.address + i.size and lo <= t < hi and img.add_function(t): work.append(t); added += 1
        return added

    def imp(self, name):
        self.imports.add(name)
        return "I_" + cname(name)

    def run(self, outdir, per_file=200):
        outside = self.discover()
        funcs = [a for a in self.img.func_addrs if self.lo <= a < self.hi]
        os.makedirs(outdir, exist_ok=True)
        bodies = [FuncGen(self, a).gen() for a in funcs]
        hdr = '#include "rt.h"\n#include "funcs.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n'
        for k in range(0, len(bodies), per_file):
            with rewrite(os.path.join(outdir, "code_%03d.c" % (k // per_file))) as f:
                f.write(hdr + "\n\n".join(bodies[k:k + per_file]) + "\n")
        for fn in os.listdir(outdir):   # files of a longer translation before this one
            m = re.match(r"code_(\d+)\.c$", fn)
            if m and int(m.group(1)) * per_file >= len(bodies): os.remove(os.path.join(outdir, fn))
        with rewrite(os.path.join(outdir, "funcs.h")) as f:
            f.write("#pragma once\n")
            f.write("".join("void F_%x(CPU *c);\n" % a for a in funcs))
            f.write("".join("void I_%s(CPU *c);\n" % cname(n) for n in sorted(self.imports)))
            f.write("".join("void H_%s(CPU *c);\n" % cname(n) for n in sorted(self.hle.values())))
        with rewrite(os.path.join(outdir, "tables.c")) as f:
            f.write('#include "rt.h"\n#include "tables.h"\n#include "funcs.h"\n')
            f.write("const GFunc gfuncs[] = {\n")
            f.write("".join('  {0x%x, F_%x, "%s"},\n' % (a, a, self.img.func_names[a]) for a in funcs))
            f.write("};\nconst int ngfuncs = %d;\n" % len(funcs))
            # every stub gets a shim symbol (weak default = unimplemented)
            allimp = sorted({n for n in self.img.stubs.values() if n} | {n for n in self.img.ptrs.values() if n}
                            | {n for _, n in self.img.extrel} | {n for _, n, _ in self.img.binds})
            for n in allimp:
                f.write('__attribute__((weak)) void I_%s(CPU *c) { unimpl_import(c, "%s"); }\n' % (cname(n), n))
            for a, n in sorted(self.hle.items()):
                f.write('__attribute__((weak)) void H_%s(CPU *c) { unimpl_import(c, "%s (HLE)"); }\n' % (cname(n), n))
            def is_data(n):
                return (n.startswith(("__ZTV", "__ZTI", "__ZTS", "_k", ".objc", "___CFConstantStringClassReference",
                                      "__DefaultRuneLocale", "___stack_chk_guard", "___stdinp", "___stdoutp", "___stderrp",
                                      "_NSApp", "_NSDefaultRunLoopMode")) or "staticMetaObject" in n)
            f.write("const GImport gimports[] = {\n")
            f.write("".join('  {"%s", %s},\n' % (n, "0" if is_data(n) else "I_" + cname(n)) for n in allimp))
            f.write("};\nconst int ngimports = %d;\n" % len(allimp))
            f.write("const GStub gstubs[] = {\n")
            f.write("".join('  {0x%x, I_%s, "%s"},\n' % (a, cname(n), n) for a, n in sorted(self.img.stubs.items()) if n))
            f.write("};\nconst int ngstubs = %d;\n" % sum(1 for n in self.img.stubs.values() if n))
            f.write("const GPtr gptrs[] = {\n")
            f.write("".join('  {0x%x, "%s"},\n' % (a, n) for a, n in sorted(self.img.ptrs.items()) if n))
            f.write("};\nconst int ngptrs = %d;\n" % sum(1 for n in self.img.ptrs.values() if n))
            sec = {x.name: x for x in self.img.bin.sections}["__mod_init_func"]
            inits = [int.from_bytes(self.img.read(sec.virtual_address + 4 * k, 4), "little") for k in range(sec.size // 4)]
            f.write("const uint32_t gmodinit[] = {%s};\nconst int ngmodinit = %d;\n" % (", ".join("0x%x" % v for v in inits), len(inits)))
            cfs = {x.name: x for x in self.img.bin.sections}.get("__cfstring")
            f.write("const uint32_t gcfstr_lo = 0x%x, gcfstr_hi = 0x%x;\n" % ((cfs.virtual_address, cfs.virtual_address + cfs.size) if cfs else (0, 0)))
            f.write("const GPtr gextrel[] = {\n")
            f.write("".join('  {0x%x, "%s"},\n' % (a, n) for a, n in self.img.extrel))
            f.write("};\nconst int ngextrel = %d;\n" % len(self.img.extrel))
            f.write("const GBind gbinds[] = {\n")
            f.write("".join('  {0x%x, "%s", %d},\n' % b for b in self.img.binds))
            f.write("};\nconst int ngbinds = %d;\n" % len(self.img.binds))
            f.write("const GSeg gsegs[] = {\n")
            f.write("".join('  {"%s", 0x%x, 0x%x, 0x%x, 0x%x},\n' % s for s in self.img.segments))
            f.write("};\nconst int ngsegs = %d;\n" % len(self.img.segments))
        print("functions: %d (%d from symbols, %d more from LC_FUNCTION_STARTS and from calls, %d of them outside __text)"
              "  imports used: %d" % (len(funcs), sum(a not in self.img.func_unnamed for a in funcs),
                                      sum(a in self.img.func_unnamed for a in funcs), outside, len(self.imports)))
        if self.eh:
            print("C++ exceptions: %d functions have a table%s" % (len(self.img.eh_lsda()),
                  "; %d landing pads lie outside their function and are not reached" % self.pads_out if self.pads_out else ""))
        if self.unsupported:
            print("UNSUPPORTED:", self.unsupported)


if __name__ == "__main__":
    # recomp32.py BINARY LO HI OUTDIR [--hle FILE] [--no-eh]
    #   BINARY  the original 32-bit Intel Mach-O executable
    #   LO HI   the address range (hex) to translate, normally the whole __text section
    #   --hle   guest functions replaced by native ones: "address name [outside]" per line; the runtime or the game
    #           project defines H_<name>. "outside" = only when that address lies outside LO..HI (partial builds)
    args = sys.argv[1:]
    hle = None
    if "--hle" in args:
        k = args.index("--hle"); hle = args[k + 1]; del args[k:k + 2]
    if "--no-eh" in args:   # no landing pads and no test after calls: an exception thrown is then fatal
        args.remove("--no-eh"); Translator.eh = False
    path, lo, hi, out = args[0], int(args[1], 16), int(args[2], 16), args[3]
    Translator(Image(path), lo, hi, hle).run(out)
