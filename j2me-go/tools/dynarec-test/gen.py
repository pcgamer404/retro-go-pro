"""Assembler and random structured program generator."""
import random
from refvm import *

class Asm:
    def __init__(s): s.items = []; s.depth = 0; s.maxd = 0; s.nl = 0
    def lab(s): s.nl += 1; return s.nl
    def b(s, bytes_, delta=0):
        s.items.append(('b', bytes(bytes_))); s.depth += delta; s.maxd = max(s.maxd, s.depth)
    def place(s, l): s.items.append(('L', l))
    def br(s, op, l, delta):
        s.items.append(('br', op, l)); s.depth += delta; s.maxd = max(s.maxd, s.depth)
    def tswitch(s, low, labs, dflt): s.items.append(('ts', low, labs, dflt)); s.depth -= 1
    def lswitch(s, pairs, dflt): s.items.append(('ls', pairs, dflt)); s.depth -= 1
    def assemble(s):
        pos = {}; pc = 0
        for it in s.items:
            k = it[0]
            if k == 'b': pc += len(it[1])
            elif k == 'L': pos[it[1]] = pc
            elif k == 'br': pc += 3
            elif k == 'ts': pad = (4 - ((pc + 1) % 4)) % 4; pc += 1 + pad + 12 + 4 * len(it[2])
            elif k == 'ls': pad = (4 - ((pc + 1) % 4)) % 4; pc += 1 + pad + 8 + 8 * len(it[1])
        out = bytearray()
        i4 = lambda v: (v & 0xFFFFFFFF).to_bytes(4, 'big')
        for it in s.items:
            k = it[0]; cur = len(out)
            if k == 'b': out += it[1]
            elif k == 'br':
                off = pos[it[2]] - cur; out += bytes([it[1]]) + (off & 0xFFFF).to_bytes(2, 'big')
            elif k == 'ts':
                _, low, labs, dflt = it; pad = (4 - ((cur + 1) % 4)) % 4
                out += bytes([0xaa]) + bytes(pad) + i4(pos[dflt] - cur) + i4(low) + i4(low + len(labs) - 1)
                for l in labs: out += i4(pos[l] - cur)
            elif k == 'ls':
                _, pairs, dflt = it; pad = (4 - ((cur + 1) % 4)) % 4
                out += bytes([0xab]) + bytes(pad) + i4(pos[dflt] - cur) + i4(len(pairs))
                for key, l in sorted(pairs): out += i4(key) + i4(pos[l] - cur)
        return bytes(out)

BIN = [0x60, 0x64, 0x68, 0x7e, 0x80, 0x82]
SHF = [0x78, 0x7a, 0x7c]
ARRS = [(12, 0x2e, 0x4f, 15), (13, 0x33, 0x54, 15), (14, 0x34, 0x55, 7), (15, 0x35, 0x56, 7)]
BR1 = [0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e]
BR2 = [0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4]

class Gen:
    def __init__(s, rnd, feats):
        s.r = rnd; s.a = Asm(); s.f = feats; s.loopdepth = 0
    # ---- emit helpers
    def ldl(s, kind, n):    # kind i/l/f/a
        base = {'i': (0x15, 0x1a), 'l': (0x16, 0x1e), 'f': (0x17, 0x22), 'a': (0x19, 0x2a)}[kind]
        d = 2 if kind == 'l' else 1
        s.a.b([base[1] + n] if n < 4 else [base[0], n], d)
    def stl(s, kind, n):
        base = {'i': (0x36, 0x3b), 'l': (0x37, 0x3f), 'f': (0x38, 0x43), 'a': (0x3a, 0x4b)}[kind]
        d = -2 if kind == 'l' else -1
        s.a.b([base[1] + n] if n < 4 else [base[0], n], d)
    def iconst(s, v):
        a = s.a
        if -1 <= v <= 5: a.b([0x03 + v], 1)
        elif -128 <= v <= 127: a.b([0x10, v & 0xFF], 1)
        elif -32768 <= v <= 32767: a.b([0x11, (v >> 8) & 0xFF, v & 0xFF], 1)
        else:
            for idx, val in CP_INTS.items():
                if val == v: a.b([0x12, idx], 1); return
            a.b([0x10, v & 0x7F], 1)
    def rconst(s):
        r = s.r.random()
        if r < .3: return s.iconst(s.r.randint(-2, 5))
        if r < .5: return s.iconst(s.r.randint(-128, 127))
        if r < .65: return s.iconst(s.r.randint(-32768, 32767))
        if r < .75: return s.iconst(s.r.choice(list(CP_INTS.values())))
        if r < .85: return s.iconst(s.r.choice([0xFF, 0xFFFF, 0x7F, 256, 255, 65535, 1 << 20]) if s.r.random() < .5 else s.r.choice([-1, 1, 2, 4, 8, 16, 1024]))
        return s.iconst(s.r.randint(-100, 100))
    # ---- expressions
    def gen_int(s, d):
        r, a = s.r, s.a
        if d <= 0 or r.random() < .25:
            c = r.random()
            if c < .35: s.rconst()
            elif c < .85: s.ldl('i', r.choice([0, 1, 2, 3, 4, 5, 0, 1, 2, 6, 7] if s.loopdepth else [0, 1, 2, 3, 4, 5]))
            elif c < .9 and s.f.get('static', True): a.b([0xb2, 0, r.choice([10, 11, 12])], 1)
            elif c < .95 and s.f.get('field', True): s.ldl('a', 17); a.b([0xca, r.randint(0, 3), 0], 0)
            else: s.rconst()
            return
        c = r.random()
        if c < .30:
            s.gen_int(d - 1); s.gen_int(d - 1); a.b([r.choice(BIN)], -1)
        elif c < .38:
            s.gen_int(d - 1)
            if r.random() < .6: s.iconst(r.randint(0, 40))
            else: s.gen_int(d - 1)
            if a.items[-1][0] == 'b' and r.random() < .0: pass
            a.b([r.choice(SHF)], -1)
        elif c < .45 and s.f.get('div', True):
            s.gen_int(d - 1); k = r.random()
            if k < .5: s.iconst(r.choice([2, 3, 7, 10, 16, -1, -3, 255, 1000, 65537, 5]))
            elif k < .9: s.gen_int(d - 1); s.iconst(1); a.b([0x80], -1)               # x | 1: never zero
            else: s.gen_int(d - 1)                                                     # may be zero
            a.b([r.choice([0x6c, 0x70])], -1)
        elif c < .52:
            s.gen_int(d - 1); a.b([r.choice([0x74, 0x91, 0x92, 0x93])], 0)
        elif c < .64 and s.f.get('array', True):
            loc, ld, _, mask = r.choice(ARRS)
            if r.random() < .03: a.b([0x01], 1)
            else: s.ldl('a', loc)
            s.gen_int(d - 1)
            if r.random() < .9: s.iconst(mask); a.b([0x7e], -1)
            a.b([ld], -1)
        elif c < .68 and s.f.get('field', True):
            s.ldl('a', 17 if r.random() < .95 else 19); a.b([0xca, r.randint(0, 3), 0], 0)
        elif c < .73 and s.f.get('static', True):
            a.b([0xb2, 0, r.choice([10, 11, 12])], 1)
        elif c < .80 and s.f.get('ternary', True):
            s.ternary(d - 1)
        elif c < .85 and s.f.get('invoke', True):
            k = r.choice([1, 2, 4])
            for _ in range(INVOKES[k][0]): s.gen_int(d - 1)
            a.b([0xb8, 0, k], 1 - INVOKES[k][0])
        elif c < .89 and s.f.get('long', True):
            s.gen_long(d - 1); s.gen_long(d - 1); a.b([0x94], -3)
        elif c < .92 and s.f.get('float', True):
            s.gen_float(d - 1); s.gen_float(d - 1); a.b([r.choice([0x95, 0x96])], -1)
        elif c < .94 and s.f.get('long', True):
            s.gen_long(d - 1); a.b([0x88], -1)
        elif c < .96 and s.f.get('float', True):
            s.gen_float(d - 1); a.b([0x8b], 0)
        elif c < .96:
            X = r.choice([0, 1, 2, 3, 4, 5]); s.ldl('i', X); s.gen_int(d - 1); a.b([0x59], 1); s.stl('i', X); a.b([r.choice(BIN)], -1)
        elif c < .97:
            s.gen_int(d - 1); a.b([0x59], 1); s.gen_int(d - 1); a.b([r.choice(BIN)], -1); a.b([r.choice(BIN)], -1)
        elif c < .975:
            s.gen_int(d - 1); s.gen_int(d - 1); a.b([0x5f], 0); a.b([0x64], -1)
        elif c < .985:                                   # i++ : old value stays on the stack
            X = r.choice([0, 1, 2, 3, 4, 5]); s.ldl('i', X); a.b([0x84, X, r.randint(-6, 6) & 0xFF])
        elif c < .995:                                   # (x = expr) used as a value
            s.gen_int(d - 1); a.b([0x59], 1); s.stl('i', r.choice([0, 1, 2, 3, 4, 5]))
        else:
            s.ldl('a', 12); a.b([0xbe], 0)
    def ternary(s, d):
        a = s.a; Lelse, Lend = a.lab(), a.lab()
        base = a.depth
        s.cond(Lelse)
        s.gen_int(d); a.br(0xa7, Lend, 0)
        a.depth = base; a.place(Lelse)
        s.gen_int(d); a.place(Lend)
    def gen_long(s, d):
        r, a = s.r, s.a
        if d <= 0 or r.random() < .3:
            c = r.random()
            if c < .15: a.b([0x09 + r.randint(0, 1)], 2)
            elif c < .5: s.ldl('l', 8)
            elif c < .6: a.b([0x14, 0, CP_LONG[0]], 2)
            elif c < .8: s.gen_int(0); a.b([0x85], 1)
            elif c < .9 and s.f.get('array', True):
                s.ldl('a', 16); s.gen_int(0); s.iconst(7); a.b([0x7e], -1); a.b([0x2f], 0)
            elif c < .95 and s.f.get('static', True): a.b([0xb2, 0, 13], 2)
            else: s.ldl('a', 17); a.b([0xcb, 4, 0], 1)
            return
        c = r.random()
        if c < .55:
            s.gen_long(d - 1); s.gen_long(d - 1); a.b([r.choice([0x61, 0x65, 0x7f, 0x81, 0x83])], -2)
        elif c < .65: s.gen_long(d - 1); a.b([0x75], 0)
        elif c < .75 and s.f.get('yield', True): s.gen_long(d - 1); s.gen_long(d - 1); a.b([0x69], -2)
        elif c < .85 and s.f.get('yield', True): s.gen_long(d - 1); s.gen_int(d - 1); a.b([r.choice([0x79, 0x7b, 0x7d])], -1)
        else: s.gen_int(d - 1); a.b([0x85], 1)
    def gen_float(s, d):
        r, a = s.r, s.a
        if d <= 0 or r.random() < .3:
            c = r.random()
            if c < .08 and s.f.get('yield', True): a.b([0x0b, 0x0b, 0x6e], 1)             # 0/0 = NaN
            elif c < .12 and s.f.get('yield', True): a.b([0x0c, 0x0b, 0x6e], 1)           # 1/0 = +inf
            elif c < .3: a.b([0x0b + r.randint(0, 2)], 1)
            elif c < .6: s.ldl('f', 10)
            else: s.gen_int(0); a.b([0x86], 0)
            return
        c = r.random()
        if c < .6: s.gen_float(d - 1); s.gen_float(d - 1); a.b([r.choice([0x62, 0x66, 0x6a])], -1)
        elif c < .7: s.gen_float(d - 1); a.b([0x76], 0)
        elif c < .8 and s.f.get('yield', True): s.gen_float(d - 1); s.gen_float(d - 1); a.b([0x6e], -1)
        else: s.gen_int(d - 1); a.b([0x86], 0)
    def cond(s, lab):
        """emit compare+branch that jumps to lab when its (random) test is true"""
        r, a = s.r, s.a; c = r.random()
        if c < .35: s.gen_int(2); a.br(r.choice(BR1), lab, -1)
        elif c < .75:
            s.gen_int(2)
            if r.random() < .4: s.rconst()
            else: s.gen_int(2)
            a.br(r.choice(BR2), lab, -2)
        elif c < .88:
            if r.random() < .3: a.b([0x01], 1)
            else: s.ldl('a', r.choice([12, 13, 17, 18]))
            a.br(r.choice([0xc6, 0xc7]), lab, -1)
        else:
            for _ in range(2):
                if r.random() < .3: a.b([0x01], 1)
                else: s.ldl('a', r.choice([12, 17, 19]))
            a.br(r.choice([0xa5, 0xa6]), lab, -2)
    # ---- statements
    def stmt(s, d):
        r, a = s.r, s.a; c = r.random()
        if c < .22: s.gen_int(3); s.stl('i', r.choice([0, 1, 2, 3, 4, 5]))
        elif c < .30: a.b([0x84, r.choice([0, 1, 2, 3, 4, 5]), r.randint(-6, 6) & 0xFF])
        elif c < .42 and d > 0: s.ifelse(d)
        elif c < .54 and d > 0 and s.loopdepth < 2: s.loop(d)
        elif c < .62 and s.f.get('array', True): s.astore()
        elif c < .67 and s.f.get('field', True): s.fput()
        elif c < .71 and s.f.get('static', True): s.sput()
        elif c < .77 and s.f.get('invoke', True): s.invoke_stmt()
        elif c < .83 and d > 0 and s.f.get('switch', True): s.switch(d)
        elif c < .87 and s.f.get('yield', True): s.newstmt()
        elif c < .92 and s.f.get('long', True): s.gen_long(3); s.stl('l', 8)
        elif c < .95 and s.f.get('float', True): s.gen_float(3); s.stl('f', 10)
        else: s.dupstmt()
    def block(s, d, n):
        for _ in range(n): s.stmt(d - 1)
    def ifelse(s, d):
        a = s.a; Lelse, Lend = a.lab(), a.lab()
        s.cond(Lelse); s.block(d, s.r.randint(1, 3))
        if s.r.random() < .5: a.br(0xa7, Lend, 0)
        a.place(Lelse)
        if s.r.random() < .5: s.block(d, s.r.randint(1, 2))
        a.place(Lend)
    def loop(s, d):
        r, a = s.r, s.a; v = 6 + s.loopdepth; n = r.randint(1, 6)
        form = r.choice(['for', 'while', 'dowhile'])
        s.loopdepth += 1
        a.b([0x03], 1); s.stl('i', v)
        Lc, Lb, Le = a.lab(), a.lab(), a.lab()
        if form == 'for':
            a.br(0xa7, Lc, 0); a.place(Lb); s.block(d, r.randint(1, 3)); a.b([0x84, v, 1])
            a.place(Lc); s.ldl('i', v); s.iconst(n); a.br(0xa1, Lb, -2)
        elif form == 'while':
            a.place(Lc); s.ldl('i', v); s.iconst(n); a.br(0xa2, Le, -2)
            s.block(d, r.randint(1, 3)); a.b([0x84, v, 1]); a.br(0xa7, Lc, 0); a.place(Le)
        else:
            a.place(Lb); s.block(d, r.randint(1, 3)); a.b([0x84, v, 1]); s.ldl('i', v); s.iconst(n); a.br(0xa1, Lb, -2)
        s.loopdepth -= 1
    def astore(s):
        r, a = s.r, s.a; c = r.random()
        if c < .1 and s.f.get('long', True):
            s.ldl('a', 16); s.gen_int(1); s.iconst(7); a.b([0x7e], -1); s.gen_long(2); a.b([0x50], -4)
        elif c < .2:
            if r.random() < .2: a.b([0x01], 1)
            else: s.ldl('a', 18)
            s.gen_int(1); s.iconst(7); a.b([0x7e], -1)
            k = r.random()
            if k < .3: a.b([0x01], 1)
            elif k < .8: s.ldl('a', 17)
            else: s.ldl('a', 19)
            a.b([0x53], -3)
        else:
            loc, _, st, mask = r.choice(ARRS)
            if r.random() < .03: a.b([0x01], 1)
            else: s.ldl('a', loc)
            s.gen_int(2)
            if r.random() < .9: s.iconst(mask); a.b([0x7e], -1)
            s.gen_int(3); a.b([st], -3)
    def fput(s):
        r, a = s.r, s.a
        if r.random() < .25 and s.f.get('long', True):
            s.ldl('a', 17); s.gen_long(2); a.b([0xcd, 4, 0], -3)
        else:
            s.ldl('a', 17 if r.random() < .95 else 19); s.gen_int(3); a.b([0xcc, r.randint(0, 3), 0], -2)
    def sput(s):
        r, a = s.r, s.a
        if r.random() < .2 and s.f.get('long', True): s.gen_long(2); a.b([0xb3, 0, 13], -2)
        else: s.gen_int(3); a.b([0xb3, 0, r.choice([10, 11, 12])], -1)
    def invoke_stmt(s):
        r, a = s.r, s.a; k = r.choice([1, 2, 3, 4])
        for _ in range(INVOKES[k][0]): s.gen_int(2)
        na, nr = INVOKES[k]; a.b([0xb8, 0, k], nr - na)
        if nr: s.stl('i', r.choice([0, 1, 2, 3, 4, 5])) if r.random() < .6 else a.b([0x57], -1)
    def switch(s, d):
        r, a = s.r, s.a; Lend, Ld = a.lab(), a.lab()
        s.gen_int(2)
        if r.random() < .5:
            s.iconst(7); a.b([0x7e], -1)
            labs = [a.lab() for _ in range(r.randint(2, 6))]
            a.tswitch(r.randint(-1, 2), labs, Ld)
            for l in labs:
                a.place(l); s.block(d, 1); a.br(0xa7, Lend, 0)
        else:
            keys = r.sample([-70000, -5, 0, 1, 3, 9, 100, 2500, 70000, 123456], r.randint(2, 7))
            labs = {k: a.lab() for k in keys}
            a.lswitch([(k, labs[k]) for k in keys], Ld)
            for k in keys:
                a.place(labs[k]); s.block(d, 1); a.br(0xa7, Lend, 0)
        a.place(Ld); s.block(d, 1); a.place(Lend)
    def newstmt(s):
        r, a = s.r, s.a
        a.b([0xbb, 0, r.randint(0, 1)], 1)
        if r.random() < .5: a.b([0xc1, 0, 0], 0); s.stl('i', r.choice([0, 1, 2, 3, 4, 5]))
        else: a.b([0x57], -1)
    def dupstmt(s):
        r, a = s.r, s.a; k = r.randint(0, 6); X = r.choice([0, 1, 2, 3, 4, 5])
        if k == 0: s.gen_int(2); a.b([0x59], 1); s.gen_int(1); a.b([0x60], -1); a.b([0x5f], 0); a.b([0x64], -1); s.stl('i', X)
        elif k == 1: s.gen_int(1); s.gen_int(1); a.b([0x5a], 1); s.stl('i', X); a.b([0x64], -1); s.stl('i', r.choice([0, 1, 2, 3, 4, 5]))
        elif k == 2: s.gen_int(1); s.gen_int(1); s.gen_int(1); a.b([0x5b], 1); a.b([0x60], -1); a.b([0x60], -1); a.b([0x64], -1); s.stl('i', X)
        elif k == 3: s.gen_int(1); s.gen_int(1); a.b([0x5c], 2); a.b([0x60], -1); s.stl('i', X); a.b([0x64], -1); s.stl('i', r.choice([0, 1, 2, 3, 4, 5]))
        elif k == 4: s.gen_int(1); s.gen_int(1); s.gen_int(1); a.b([0x5d], 2); a.b([0x58], -2); a.b([0x60], -1); a.b([0x60], -1); s.stl('i', X)
        elif k == 5: s.gen_int(1); s.gen_int(1); s.gen_int(1); s.gen_int(1); a.b([0x5e], 2); a.b([0x58], -2); a.b([0x60], -1); a.b([0x60], -1); a.b([0x60], -1); s.stl('i', X)
        else: s.gen_long(2); a.b([0x5c], 2); a.b([0x61], -2); s.stl('l', 8); s.gen_int(1); a.b([0x57], -1)

def gen_program(seed, feats=None, scale=1):
    rnd = random.Random(seed); g = Gen(rnd, feats or {})
    n = rnd.randint(3, 9) * scale
    for _ in range(n): g.stmt(3)
    a = g.a
    # fold all locals into one int result
    g.ldl('i', 0)
    for i in (1, 2, 3, 4, 5):
        g.ldl('i', i); a.b([rnd.choice([0x60, 0x82])], -1)
    g.ldl('l', 8); a.b([0x88], -1); a.b([0x82], -1)
    g.ldl('f', 10); a.b([0x8b], 0); a.b([0x60], -1)
    a.b([0xac], -1)
    return a.assemble(), a.maxd + 4
