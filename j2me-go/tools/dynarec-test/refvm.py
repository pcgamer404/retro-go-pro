"""Reference bytecode interpreter + mini assembler + random program generator for differential testing."""
import struct, random
from xtemu import M32, s32

DATA = 0x20000000
T_ADDR, F_ADDR, LOC = DATA + 0x100, DATA + 0x400, DATA + 0x1000
MAXL = 24
STK = LOC + 4 * MAXL
STAT = DATA + 0x6000
ARR_I, ARR_B, ARR_C, ARR_S, ARR_L, ARR_A = (DATA + 0x8000 + 0x100 * k for k in range(6))
OBJ1, OBJ2 = DATA + 0x8600, DATA + 0x8700
CLS_ARR, CLS_X, CLS_BASE, CLS_OBJ, CLS_OTHER = (DATA + 0x9000 + 0x100 * k for k in range(5))
ARR_LEN = {ARR_I: 16, ARR_B: 24, ARR_C: 12, ARR_S: 12, ARR_L: 8, ARR_A: 6}
LOCAL_REFS = {12: ARR_I, 13: ARR_B, 14: ARR_C, 15: ARR_S, 16: ARR_L, 17: OBJ1, 18: ARR_A, 19: OBJ2}
NEW_OBJS = [OBJ1, OBJ2]
INVOKES = {1: (2, 1), 2: (1, 1), 3: (3, 0), 4: (0, 1)}      # idx -> (arg slots, result slots)
CP_INTS = {1: 100000, 2: -77777, 3: 0x7fffffff, 4: -2147483648, 5: 123456789}
CP_LONG = (6, 0x00000012, 0x3456789a)                         # idx, hi, lo
OFF_SUPER = OFF_COMP = 0                                      # patched by the driver from the C library

class JExc(Exception):
    def __init__(self, kind, pc): self.kind, self.pc = kind, pc

def f2b(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def b2f(u): return struct.unpack('<f', struct.pack('<I', u & M32))[0]
def f32(x):
    try: return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError: return float('inf') if x > 0 else float('-inf')
def w64(x): x &= (1 << 64) - 1; return x - (1 << 64) if x >> 63 else x

def init_heap(e, rnd):
    """build the shared heap image: arrays, objects, classes, statics"""
    for a, ln in ARR_LEN.items():
        e.w32(a + 12, ln)
        e.w32(a, CLS_ARR)
        n = ln * (8 if a == ARR_L else (1 if a == ARR_B else (2 if a in (ARR_C, ARR_S) else 4)))
        for i in range(0, n, 4): e.w32(a + 16 + i, rnd.randint(-50, 50) if rnd.random() < .5 else rnd.getrandbits(32))
    for i in range(6): e.w32(ARR_A + 16 + 4 * i, 0)
    e.w32(OBJ1, CLS_X); e.w32(OBJ2, CLS_OTHER)
    for o in (OBJ1, OBJ2):
        for s in range(8): e.w32(o + 12 + 4 * s, rnd.randint(-100, 100) if rnd.random() < .6 else rnd.getrandbits(32))
    e.w32(CLS_ARR + OFF_COMP, CLS_BASE); e.w32(CLS_ARR + OFF_SUPER, CLS_OBJ)
    e.w32(CLS_X + OFF_SUPER, CLS_BASE); e.w32(CLS_BASE + OFF_SUPER, CLS_OBJ); e.w32(CLS_OBJ + OFF_SUPER, 0)
    e.w32(CLS_OTHER + OFF_SUPER, CLS_OBJ)
    for s in range(8): e.w32(STAT + 4 * s, rnd.randint(-9, 9))

def invoke_result(k, args):
    a = [s32(x) for x in args]
    if k == 1: return [s32(a[0] * 31 + a[1] * 17 + 7)]
    if k == 2: return [s32((a[0] ^ 0x5bd1e995) * 3 + 1)]
    return None

class Ref:
    """interprets one instruction at a time on the shared emulator memory"""
    def __init__(self, e, code):
        self.e, self.code = e, code
        self.nback = 0
    # --- helpers
    def u1(self, p): return self.code[p]
    def s1(self, p): v = self.code[p]; return v - 256 if v > 127 else v
    def u2(self, p): return (self.code[p] << 8) | self.code[p + 1]
    def s2(self, p): v = self.u2(p); return v - 65536 if v > 32767 else v
    def s4(self, p): v = (self.code[p] << 24) | (self.code[p+1] << 16) | (self.code[p+2] << 8) | self.code[p+3]; return s32(v)
    def step(self, pc, sp):
        """returns (new_pc, new_sp, event) ; event in (None,'invoke','back')"""
        e, c = self.e, self.code
        op = c[pc]
        R = lambda a: e.r32(a)
        W = lambda a, v: e.w32(a, v & M32)
        def pop():
            nonlocal sp
            sp -= 4; return e.r32(sp)
        def push(v):
            nonlocal sp
            W(sp, v); sp += 4
        def popl():
            hi = pop(); lo = pop(); return w64((hi << 32) | lo)
        def pushl(v):
            v &= (1 << 64) - 1; push(v & M32); push(v >> 32)
        def chk(ref):
            if ref == 0 or 0 < ref < 0x1000: raise JExc('NPE', pc)
        def chkarr(a, i, shift_len=None):
            chk(a)
            ln = R(a + 12)
            if not (0 <= s32(i) < ln): raise JExc('AIOOBE', pc)
        nxt = pc + 1
        event = None
        loc = lambda i: LOC + 4 * i
        if op == 0x00: pass
        elif op == 0x01: push(0)
        elif 0x02 <= op <= 0x08: push(op - 3)
        elif op in (0x09, 0x0a): pushl(op - 9)
        elif op == 0x0b: push(0)
        elif op == 0x0c: push(0x3F800000)
        elif op == 0x0d: push(0x40000000)
        elif op == 0x0e: pushl(0)
        elif op == 0x0f: pushl(0x3FF0000000000000)
        elif op == 0x10: push(self.s1(pc + 1)); nxt = pc + 2
        elif op == 0x11: push(self.s2(pc + 1)); nxt = pc + 3
        elif op == 0x12: push(CP_INTS[c[pc + 1]]); nxt = pc + 2
        elif op == 0x14: idx = self.u2(pc + 1); assert idx == CP_LONG[0]; push(CP_LONG[2]); push(CP_LONG[1]); nxt = pc + 3
        elif op in (0x15, 0x17, 0x19): push(R(loc(c[pc + 1]))); nxt = pc + 2
        elif op in (0x16, 0x18): i = c[pc + 1]; push(R(loc(i))); push(R(loc(i + 1))); nxt = pc + 2
        elif 0x1a <= op <= 0x2d:
            g, n = divmod(op - 0x1a, 4)
            push(R(loc(n)))
            if g in (1, 3): push(R(loc(n + 1)))
        elif 0x2e <= op <= 0x35:
            i = pop(); a = pop(); chkarr(a, i); i = s32(i)
            if op == 0x2f or op == 0x31: push(R(a + 16 + 8 * i)); push(R(a + 20 + 8 * i))
            elif op == 0x33: v = e.rd(a + 16 + i, 1); push(v - 256 if v > 127 else v)
            elif op == 0x34: push(e.rd(a + 16 + 2 * i, 2))
            elif op == 0x35: push(e.rd(a + 16 + 2 * i, 2, True))
            else: push(R(a + 16 + 4 * i))
        elif op in (0x36, 0x38, 0x3a): W(loc(c[pc + 1]), pop()); nxt = pc + 2
        elif op in (0x37, 0x39): i = c[pc + 1]; W(loc(i + 1), pop()); W(loc(i), pop()); nxt = pc + 2
        elif 0x3b <= op <= 0x4e:
            g, n = divmod(op - 0x3b, 4)
            if g in (1, 3): W(loc(n + 1), pop())
            W(loc(n), pop())
        elif 0x4f <= op <= 0x56:
            if op in (0x50, 0x52): vh = pop(); vl = pop()
            else: v = pop()
            i = pop(); a = pop(); chkarr(a, i); i = s32(i)
            if op in (0x50, 0x52): W(a + 16 + 8 * i, vl); W(a + 20 + 8 * i, vh)
            elif op == 0x53:
                if v != 0:
                    cls = R(v); comp = R(R(a) + OFF_COMP); ok = False
                    while cls:
                        if cls == comp: ok = True; break
                        cls = R(cls + OFF_SUPER)
                    if not ok: raise JExc('ArrayStore', pc)
                W(a + 16 + 4 * i, v)
            elif op == 0x54: e.wr(a + 16 + i, 1, v)
            elif op in (0x55, 0x56): e.wr(a + 16 + 2 * i, 2, v)
            else: W(a + 16 + 4 * i, v)
        elif op == 0x57: pop()
        elif op == 0x58: pop(); pop()
        elif op == 0x59: v = pop(); push(v); push(v)
        elif op == 0x5a: v1 = pop(); v2 = pop(); push(v1); push(v2); push(v1)
        elif op == 0x5b: v1 = pop(); v2 = pop(); v3 = pop(); push(v1); push(v3); push(v2); push(v1)
        elif op == 0x5c: v1 = pop(); v2 = pop(); push(v2); push(v1); push(v2); push(v1)
        elif op == 0x5d: v1 = pop(); v2 = pop(); v3 = pop(); push(v2); push(v1); push(v3); push(v2); push(v1)
        elif op == 0x5e: v1 = pop(); v2 = pop(); v3 = pop(); v4 = pop(); push(v2); push(v1); push(v4); push(v3); push(v2); push(v1)
        elif op == 0x5f: v1 = pop(); v2 = pop(); push(v1); push(v2)
        elif op in (0x60, 0x64, 0x68, 0x6c, 0x70, 0x78, 0x7a, 0x7c, 0x7e, 0x80, 0x82):
            b = s32(pop()); a = s32(pop())
            if op == 0x60: r = a + b
            elif op == 0x64: r = a - b
            elif op == 0x68: r = a * b
            elif op in (0x6c, 0x70):
                if b == 0: raise JExc('Arith', pc)
                if a == -2**31 and b == -1: r = a if op == 0x6c else 0
                else:
                    q = abs(a) // abs(b); q = -q if (a < 0) != (b < 0) else q
                    r = q if op == 0x6c else a - q * b
            elif op == 0x78: r = a << (b & 31)
            elif op == 0x7a: r = a >> (b & 31)
            elif op == 0x7c: r = (a & M32) >> (b & 31)
            elif op == 0x7e: r = a & b
            elif op == 0x80: r = a | b
            else: r = a ^ b
            push(s32(r))
        elif op in (0x61, 0x65, 0x7f, 0x81, 0x83, 0x69):
            b = popl(); a = popl()
            r = {0x61: a + b, 0x65: a - b, 0x7f: a & b, 0x81: a | b, 0x83: a ^ b, 0x69: a * b}[op]
            pushl(w64(r))
        elif op in (0x79, 0x7b, 0x7d):
            sh = s32(pop()) & 63; a = popl()
            r = a << sh if op == 0x79 else (a >> sh if op == 0x7b else (a & (2**64 - 1)) >> sh)
            pushl(w64(r))
        elif op in (0x62, 0x66, 0x6a, 0x6e):
            b = b2f(pop()); a = b2f(pop())
            if op == 0x62: r = a + b
            elif op == 0x66: r = a - b
            elif op == 0x6a: r = a * b
            else:
                try: r = a / b
                except ZeroDivisionError: r = float('nan') if (a == 0 or a != a) else (float('inf') if (a > 0) == (str(b)[0] != '-') else float('-inf'))
            push(f2b(f32(r)))
        elif op == 0x74: push(s32(-s32(pop())))
        elif op == 0x75: pushl(w64(-popl()))
        elif op == 0x76: push(f2b(-b2f(pop())))
        elif op == 0x84: i = c[pc + 1]; W(loc(i), s32(R(loc(i))) + self.s1(pc + 2)); nxt = pc + 3
        elif op == 0x85: v = s32(pop()); pushl(v)
        elif op == 0x86: push(f2b(f32(float(s32(pop())))))
        elif op == 0x88: pop()
        elif op == 0x8b:
            f = b2f(pop())
            push(0 if f != f else max(-2**31, min(2**31 - 1, int(f))))
        elif op in (0x91, 0x92, 0x93):
            v = pop()
            push(((v & 0xFF) ^ 0x80) - 0x80 if op == 0x91 else (v & 0xFFFF if op == 0x92 else ((v & 0xFFFF) ^ 0x8000) - 0x8000))
        elif op == 0x94: b = popl(); a = popl(); push((a > b) - (a < b))
        elif op in (0x95, 0x96):
            b = b2f(pop()); a = b2f(pop())
            if a != a or b != b: push(-1 if op == 0x95 else 1)
            else: push((a > b) - (a < b))
        elif 0x99 <= op <= 0xa7 or op in (0xc6, 0xc7):
            off = self.s2(pc + 1); nxt = pc + 3
            if off <= 0: self.nback += 1
            if op == 0xa7: take = True
            elif op in (0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0xc6, 0xc7):
                v = s32(pop())
                take = {0x99: v == 0, 0x9a: v != 0, 0x9b: v < 0, 0x9c: v >= 0, 0x9d: v > 0, 0x9e: v <= 0, 0xc6: v == 0, 0xc7: v != 0}[op]
            else:
                b = s32(pop()); a = s32(pop())
                take = {0x9f: a == b, 0xa0: a != b, 0xa1: a < b, 0xa2: a >= b, 0xa3: a > b, 0xa4: a <= b, 0xa5: a == b, 0xa6: a != b}[op]
            if take:
                nxt = pc + off
                if off <= 0: event = 'back'
        elif op == 0xaa:
            k = s32(pop()); p = (pc + 4) & ~3
            d = self.s4(p); lo = self.s4(p + 4); hi = self.s4(p + 8)
            off = self.s4(p + 12 + 4 * (k - lo)) if lo <= k <= hi else d
            nxt = pc + off
            if off <= 0: event = 'back'
        elif op == 0xab:
            k = s32(pop()); p = (pc + 4) & ~3
            d = self.s4(p); n = self.s4(p + 4); off = d
            for i in range(n):
                if self.s4(p + 8 + 8 * i) == k: off = self.s4(p + 12 + 8 * i)
            nxt = pc + off
            if off <= 0: event = 'back'
        elif op in (0xac, 0xae, 0xb0): return ('ret', [pop()])
        elif op in (0xad, 0xaf): hi = pop(); lo = pop(); return ('ret', [lo, hi])
        elif op == 0xb1: return ('ret', [])
        elif op == 0xb2:
            idx = self.u2(pc + 1); off, wide = STATIC_FIELDS[idx]
            push(R(STAT + 4 * off))
            if wide: push(R(STAT + 4 * off + 4))
            nxt = pc + 3
        elif op == 0xb3:
            idx = self.u2(pc + 1); off, wide = STATIC_FIELDS[idx]
            if wide: W(STAT + 4 * off + 4, pop())
            W(STAT + 4 * off, pop()); nxt = pc + 3
        elif op == 0xb8:
            k = self.u2(pc + 1); na, nr = INVOKES[k]
            args = [pop() for _ in range(na)][::-1]
            if k == 3: W(STAT + 24, args[0] ^ args[2])
            elif k == 4: v = (R(STAT + 28) + 1); W(STAT + 28, v); push(s32(v))
            else:
                for r in invoke_result(k, args): push(r)
            nxt = pc + 3; event = 'invoke'
        elif op == 0xbb: push(NEW_OBJS[self.u2(pc + 1) % 2]); nxt = pc + 3
        elif op == 0xbe: a = pop(); chk(a); push(R(a + 12))
        elif op == 0xc0: nxt = pc + 3
        elif op == 0xc1: v = pop(); push(1 if (v and R(v) == CLS_X) else 0); nxt = pc + 3
        elif op == 0xca: o = pop(); chk(o); push(R(o + 12 + 4 * c[pc + 1])); nxt = pc + 3
        elif op == 0xcb: o = pop(); chk(o); s = c[pc + 1]; push(R(o + 12 + 4 * s)); push(R(o + 16 + 4 * s)); nxt = pc + 3
        elif op == 0xcc: v = pop(); o = pop(); chk(o); W(o + 12 + 4 * c[pc + 1], v); nxt = pc + 3
        elif op == 0xcd: vh = pop(); vl = pop(); o = pop(); chk(o); s = c[pc + 1]; W(o + 12 + 4 * s, vl); W(o + 16 + 4 * s, vh); nxt = pc + 3
        else: raise Exception("ref: unhandled opcode %#x at %d" % (op, pc))
        return (nxt, sp, event)

STATIC_FIELDS = {10: (0, False), 11: (1, False), 12: (2, False), 13: (4, True)}   # cp idx -> (slot, wide)
