import ctypes, sys, random, time
sys.path.insert(0, '.')
import refvm
from refvm import *
from xtemu import Emu, M32, s32
from gen import gen_program

L = ctypes.CDLL(sys.argv[3] if len(sys.argv) > 3 else './libdyn_v2.so')
for n in "h_cache_base h_cache_size h_entry h_codeptr h_jit_size".split(): getattr(L, n).restype = ctypes.c_uint32
L.h_last_log.restype = ctypes.c_char_p
L.h_init()
refvm.OFF_SUPER = L.h_off_class_super(); refvm.OFF_COMP = L.h_off_class_component()
OFF = {k: getattr(L, 'h_off_' + k)() for k in ("frame_pc", "frame_sp", "frame_locals", "thread_budget")}
for idx, v in CP_INTS.items(): L.h_set_cp_int(idx, v)
L.h_set_cp_long(CP_LONG[0], CP_LONG[1], CP_LONG[2])
for cp, (off, wide) in STATIC_FIELDS.items(): L.h_set_cp_static(cp, cp - 10, off, 1 if wide else 0)
L.h_set_fstate(3)   # CLS_INITIALIZED

def initial_locals(rnd):
    vals = [0, 1, -1, 2, 7, -8, 100, 0x7fffffff, -0x80000000, 12345, -99999]
    loc = [rnd.choice(vals) if rnd.random() < .5 else rnd.getrandbits(32) for _ in range(6)] + [0, 0]
    loc += [rnd.getrandbits(32), rnd.getrandbits(32) if rnd.random() < .5 else 0, f2b(rnd.choice([0.0, 1.5, -2.25, 100.0, 3e5])), 0]
    for i in range(12, 20):
        loc.append(0 if rnd.random() < .03 else LOCAL_REFS[i])
    return loc + [0] * (MAXL - len(loc))

def run_prog(code, snap, base, entry, cp, osr, seed, jit, budget_mode):
    e = Emu(snap if jit else b'', code_base=base)
    rnd = random.Random(seed); init_heap(e, rnd)
    for i, v in enumerate(initial_locals(rnd)): e.w32(LOC + 4 * i, v & M32)
    ref = Ref(e, code)
    brnd = random.Random(seed ^ 0x5555)
    budget = 10 ** 6 if budget_mode == 'big' else brnd.randint(1, 6)
    pc, sp = 0, STK
    try_jit = jit
    nb0 = 0
    try:
        while True:
            if try_jit:
                e.w32(T_ADDR + OFF['thread_budget'], budget & M32)
                e.w32(F_ADDR + OFF['frame_pc'], cp + pc); e.w32(F_ADDR + OFF['frame_sp'], sp)
                e.w32(F_ADDR + OFF['frame_locals'], LOC)
                rc = e.run(entry - base, T_ADDR, F_ADDR, max_steps=400000)
                budget = s32(e.r32(T_ADDR + OFF['thread_budget']))
                if rc == 0:
                    if budget_mode == 'big': return ('ret', [e.r32(LOC)], 10 ** 6 - (budget - (ref.nback - nb0) * 0) ), e
                    return ('ret', [e.r32(LOC)]), e
                if rc not in (1, 2): return ('badrc', rc), e
                pc = e.r32(F_ADDR + OFF['frame_pc']) - cp; sp = e.r32(F_ADDR + OFF['frame_sp'])
                if budget < 0: budget = brnd.randint(1, 6) if budget_mode != 'big' else 10 ** 6
            for _ in range(100000):
                res = ref.step(pc, sp)
                if res[0] == 'ret':
                    if budget_mode == 'big': budget -= (ref.nback - nb0); nb0 = ref.nback; return ('ret', [v & M32 for v in res[1]], 10 ** 6 - budget), e
                    return ('ret', [v & M32 for v in res[1]]), e
                pc, sp, ev = res
                if budget_mode == 'big': budget -= (ref.nback - nb0); nb0 = ref.nback
                if jit and (ev == 'invoke' or (ev == 'back' and osr)): break
            else: return ('hang',), e
            try_jit = jit
    except JExc as x:
        return ('exc', x.kind, x.pc), e
    except Exception as x:
        return ('emu-error', str(x)), e

def snapshot_state(out, e):
    mem = bytes(e.data[0x6000:0x9500])
    loc = bytes(e.data[0x1000:0x1000 + 96]) if out[0] == 'exc' else b''
    return mem, loc

def check_program(seed, ninputs=3, feats=None, verbose=False, scale=1):
    code, ms = gen_program(seed, feats, scale)
    L.h_reset()
    i = L.h_add(code, len(code), MAXL, ms, 1)
    ok = L.h_compile(i)
    if not ok:
        return 'rejected', (L.h_last_log() or b'').decode(), code
    base, size = L.h_cache_base(), L.h_cache_size()
    snap = bytes((ctypes.c_uint8 * size).from_address(base))
    entry, cp, osr = L.h_entry(i), L.h_codeptr(i), L.h_osr(i)
    bad = []
    for k in range(ninputs):
        s = seed * 1000 + k
        o0, e0 = run_prog(code, snap, base, entry, cp, osr, s, False, 'big')
        s0 = snapshot_state(o0, e0)
        for bm in ('big', 'small'):
            o1, e1 = run_prog(code, snap, base, entry, cp, osr, s, True, bm)
            s1 = snapshot_state(o1, e1)
            if o0[0] in ('hang', 'emu-error'): continue
            a0 = o0[:2] if (bm == 'small' and o0[0] == 'ret') else o0     # 'big' mode ret tuples also carry the consumed budget
            if a0 != o1 or s0 != s1:
                bad.append((s, bm, a0, o1, s0[0] != s1[0], s0[1] != s1[1]))
    return ('FAIL' if bad else 'ok'), bad, code

if __name__ == '__main__':
    start, count = int(sys.argv[1]), int(sys.argv[2])
    SCALE = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    t0 = time.time(); stats = {'ok': 0, 'FAIL': 0, 'rejected': 0}; fails = []; rej = {}
    for seed in range(start, start + count):
        r, info, code = check_program(seed, 3, None, False, SCALE)
        stats[r] += 1
        if r == 'FAIL': fails.append((seed, info, code))
        if r == 'rejected': rej[info] = rej.get(info, 0) + 1
    print("programs %d..%d: %s  (%.1fs)" % (start, start + count - 1, stats, time.time() - t0))
    if rej: print("rejection reasons:", rej)
    for seed, info, code in fails[:4]:
        print("FAIL seed", seed, "len", len(code))
        for f in info[:2]: print("   input", f[0], f[1], "ref:", f[2], "jit:", f[3], "mem-diff", f[4], "loc-diff", f[5])
