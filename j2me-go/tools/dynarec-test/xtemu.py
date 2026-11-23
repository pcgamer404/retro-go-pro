"""Tiny Xtensa LX7 interpreter: just the integer subset the dynarec emits (windowing treated as no-ops)."""
M32 = 0xFFFFFFFF
import struct as _st
def _f32(x):
    try: return _st.unpack('<f', _st.pack('<f', x))[0]
    except OverflowError: return float('inf') if x > 0 else float('-inf')
def s32(x): x &= M32; return x - (1 << 32) if x & 0x80000000 else x
def sx(v, bits): return v - (1 << bits) if v & (1 << (bits - 1)) else v

class Emu:
    DATA = 0x20000000
    def __init__(self, code, data_size=0x20000, code_base=0x10000000):
        self.CODE = code_base
        self.code = bytes(code); self.data = bytearray(data_size); self.a = [0] * 16; self.sar = 0; self.steps = 0; self.f = [0.0]*16; self.b = [False]*16
    def _mem(self, addr, n):
        if self.CODE <= addr < self.CODE + len(self.code): return self.code, addr - self.CODE
        if self.DATA <= addr < self.DATA + len(self.data): return self.data, addr - self.DATA
        raise Exception("bad address %#x" % addr)
    def rd(self, addr, n, signed=False):
        buf, o = self._mem(addr, n); v = int.from_bytes(buf[o:o+n], 'little'); return sx(v, 8*n) if signed else v
    def wr(self, addr, n, v):
        buf, o = self._mem(addr, n)
        if buf is self.code: raise Exception("write to code")
        buf[o:o+n] = (v & ((1 << 8*n) - 1)).to_bytes(n, 'little')
    def w32(self, addr, v): self.wr(addr, 4, v)
    def r32(self, addr): return self.rd(addr, 4)
    def run(self, entry, a2, a3, max_steps=2000000):
        a = self.a; pc = self.CODE + entry; a[2] = a2 & M32; a[3] = a3 & M32
        while True:
            self.steps += 1
            if self.steps > max_steps: raise Exception("step limit")
            b0 = self.rd(pc, 1); op0 = b0 & 15
            if op0 >= 8:                                   # 16-bit density
                w = self.rd(pc, 2); t = (w >> 4) & 15; s = (w >> 8) & 15; r = (w >> 12) & 15
                npc = pc + 2
                if op0 == 0xD:
                    if r == 0 and True: a[t] = a[s]        # mov.n at, as   (r==0)
                    elif w == 0xF01D: return a[2]          # retw.n
                    else: raise Exception("D? %#x" % w)
                elif op0 == 0xC:
                    if (t & 8) == 0:
                        v = ((t & 7) << 4) | r; a[s] = (v - 128 if v >= 96 else v) & M32     # movi.n
                    else: raise Exception("beqz.n unsupported")
                elif op0 == 0xA: a[r] = (a[s] + a[t]) & M32
                elif op0 == 0xB: a[r] = (a[s] + (-1 if t == 0 else t)) & M32
                elif op0 == 0x8: a[t] = self.r32((a[s] + (r << 2)) & M32)
                elif op0 == 0x9: self.w32((a[s] + (r << 2)) & M32, a[t])
                else: raise Exception("density op0=%d" % op0)
                pc = npc; continue
            w = self.rd(pc, 3); t = (w >> 4) & 15; s = (w >> 8) & 15; r = (w >> 12) & 15
            op1 = (w >> 16) & 15; op2 = (w >> 20) & 15; imm8 = (w >> 16) & 255; npc = pc + 3
            if op0 == 0:
                if op1 == 0:
                    if op2 == 0:
                        if r == 0 and t == 0xA: npc = a[s]; pc = npc; continue      # jx
                        raise Exception("RST0/0 %#x" % w)
                    elif op2 == 1: a[r] = a[s] & a[t]
                    elif op2 == 2: a[r] = a[s] | a[t]
                    elif op2 == 3: a[r] = a[s] ^ a[t]
                    elif op2 == 4:
                        if r == 0: self.sar = a[s] & 31                  # ssr
                        elif r == 1: self.sar = (32 - (a[s] & 31))       # ssl
                        else: raise Exception("op2=4 r=%d" % r)
                    elif op2 == 6 and s == 0: a[r] = (-s32(a[t])) & M32  # neg
                    elif op2 == 8: a[r] = (a[s] + a[t]) & M32
                    elif op2 == 9: a[r] = ((a[s] << 1) + a[t]) & M32
                    elif op2 == 0xA: a[r] = ((a[s] << 2) + a[t]) & M32
                    elif op2 == 0xC: a[r] = (a[s] - a[t]) & M32
                    else: raise Exception("RST0 op2=%d" % op2)
                elif op1 == 1:
                    if op2 in (0, 1): sa = 32 - (t | ((op2 & 1) << 4)); a[r] = (a[s] << sa) & M32   # slli
                    elif op2 in (2, 3): sa = s | ((op2 & 1) << 4); a[r] = (s32(a[t]) >> sa) & M32   # srai
                    elif op2 == 4: a[r] = a[t] >> s                                                  # srli
                    elif op2 == 9: a[r] = a[t] >> self.sar                                           # srl
                    elif op2 == 0xA: a[r] = (a[s] << (32 - self.sar)) & M32 if self.sar else a[s]   # sll
                    elif op2 == 0xB: a[r] = (s32(a[t]) >> self.sar) & M32                            # sra
                    else: raise Exception("RST1 op2=%d" % op2)
                elif op1 == 2:
                    x, y = a[s], a[t]
                    if op2 == 6: a[r] = 1 if x < y else 0                    # saltu
                    elif op2 == 7: a[r] = 1 if s32(x) < s32(y) else 0        # salt
                    elif op2 == 8: a[r] = (x * y) & M32                      # mull
                    elif op2 == 0xD:                                         # quos
                        if y == 0: raise Exception("div0")
                        q = abs(s32(x)) // abs(s32(y)); q = -q if (s32(x) < 0) != (s32(y) < 0) else q; a[r] = q & M32
                    elif op2 == 0xF:                                         # rems
                        q = abs(s32(x)) % abs(s32(y)); a[r] = (-q if s32(x) < 0 else q) & M32
                    else: raise Exception("RST2 op2=%d" % op2)
                elif op1 == 3:
                    if op2 == 8:
                        if a[t] == 0: a[r] = a[s]                            # moveqz
                    elif op2 == 9:
                        if a[t] != 0: a[r] = a[s]
                    elif op2 == 0xD:                                                     # movt
                        if self.b[t]: a[r] = a[s]
                    elif op2 == 2: a[r] = sx(a[s] & ((1 << (t + 8)) - 1), t + 8) & M32   # sext
                    else: raise Exception("RST3 op2=%d" % op2)
                elif op1 in (4, 5): a[r] = (a[t] >> (s | ((op1 & 1) << 4))) & ((1 << (op2 + 1)) - 1)   # extui
                elif op1 == 0xA and op2 == 0xF and t == 5: a_f = __import__('struct'); self.f[r] = a_f.unpack('<f', a_f.pack('<I', a[s]))[0]   # wfr
                elif op1 == 0xA and op2 == 0xF and t == 4: a_f = __import__('struct'); a[r] = a_f.unpack('<I', a_f.pack('<f', self.f[s]))[0]   # rfr
                elif op1 == 0xA and op2 in (0, 1, 2):
                    x, y = self.f[s], self.f[t]; self.f[r] = (x + y, x - y, x * y)[op2]
                    self.f[r] = _f32(self.f[r])
                elif op1 == 0xA and op2 == 0xF and t == 6: self.f[r] = -self.f[s]                   # neg.s
                elif op1 == 0xA and op2 == 0xC: self.f[r] = _f32(float(s32(a[s])) / (1 << t))               # float.s
                elif op1 == 0xA and op2 == 9:                                                       # trunc.s
                    v = self.f[s] * (1 << t); a[r] = (0 if v != v else max(-2**31, min(2**31 - 1, int(v)))) & M32
                elif op1 == 0xB and op2 == 4: self.b[r] = self.f[s] < self.f[t]     # olt.s (False on NaN)
                elif op1 == 0xB and op2 == 2: self.b[r] = self.f[s] == self.f[t]    # oeq.s
                elif op1 == 3 and op2 == 0xD:                                        # movt
                    if self.b[t]: a[r] = a[s]
                else: raise Exception("op1=%d op2=%d" % (op1, op2))
            elif op0 == 1:                                                   # l32r
                imm16 = (w >> 8) & 0xFFFF
                addr = (((pc + 3) & ~3) + (0xFFFC0000 | (imm16 << 2)) - (1 << 32)) & M32
                a[t] = self.r32(addr)
            elif op0 == 2:
                if r == 0: a[t] = self.rd((a[s] + imm8) & M32, 1)
                elif r == 1: a[t] = self.rd((a[s] + imm8 * 2) & M32, 2)
                elif r == 2: a[t] = self.r32((a[s] + imm8 * 4) & M32)
                elif r == 4: self.wr((a[s] + imm8) & M32, 1, a[t])
                elif r == 5: self.wr((a[s] + imm8 * 2) & M32, 2, a[t])
                elif r == 6: self.w32((a[s] + imm8 * 4) & M32, a[t])
                elif r == 9: a[t] = self.rd((a[s] + imm8 * 2) & M32, 2, True) & M32
                elif r == 0xA: a[t] = sx(((s << 8) | imm8), 12) & M32       # movi
                elif r == 0xC: a[t] = (a[s] + sx(imm8, 8)) & M32            # addi
                elif r == 0xD: a[t] = (a[s] + (sx(imm8, 8) << 8)) & M32     # addmi
                else: raise Exception("LSAI r=%d" % r)
            elif op0 == 6:
                n = (w >> 4) & 3; m = (w >> 6) & 3
                if n == 0: npc = pc + 4 + sx((w >> 6) & 0x3FFFF, 18)       # j
                elif n == 1:
                    v = s32(a[s]); d = sx((w >> 12) & 0xFFF, 12)
                    take = [v == 0, v != 0, v < 0, v >= 0][m]
                    if take: npc = pc + 4 + d
                elif n == 3 and m == 0: pass                               # entry
                elif n == 2:                                               # beqi bnei blti bgei
                    k = [-1, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256][r]; v = s32(a[s]); d = sx((w >> 16) & 0xFF, 8)
                    if [v == k, v != k, v < k, v >= k][m]: npc = pc + 4 + d
                elif n == 3 and m in (2, 3):                               # bltui bgeui
                    k = [32768, 65536, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256][r]; d = sx((w >> 16) & 0xFF, 8)
                    if (a[s] < k) if m == 2 else (a[s] >= k): npc = pc + 4 + d
                elif n == 3 and m == 1:                                    # bf / bt
                    bv = self.b[s]; d = sx((w >> 16) & 0xFF, 8)
                    if (r == 1 and bv) or (r == 0 and not bv): npc = pc + 4 + d
                else: raise Exception("op0=6 n=%d m=%d" % (n, m))
            elif op0 == 7:
                x, y = a[s], a[t]; d = sx(imm8, 8)
                take = {1: x == y, 2: s32(x) < s32(y), 3: x < y, 9: x != y, 0xA: s32(x) >= s32(y), 0xB: x >= y}.get(r)
                if take is None: raise Exception("BR r=%d" % r)
                if take: npc = pc + 4 + d
            else: raise Exception("op0=%d w=%#x" % (op0, w))
            a[0] = a[0]  # a0 untouched
            pc = npc
