import ctypes, sys
sys.path.insert(0, '.')
from xtemu import Emu, M32, s32
from gen import Asm
import refvm
from refvm import *

def asm_prog(build):
    a = Asm(); build(a); return a.assemble()

def kernels():
    K = {}
    def b1(a):   # sum loop
        a.b([0x03,0x3c,0x03,0x3d]); Lc,Lb=a.lab(),a.lab(); a.br(0xa7,Lc,0); a.place(Lb)
        a.b([0x1b,0x1c,0x60,0x3c,0x84,2,1]); a.place(Lc); a.b([0x1c,0x1a]); a.br(0xa1,Lb,0); a.b([0x1b,0xac])
    K['sum loop        '] = (b1, 3)
    def b2(a):   # int[] read-modify-write
        a.b([0x03,0x3c,0x03,0x3d]); Lc,Lb=a.lab(),a.lab(); a.br(0xa7,Lc,0); a.place(Lb)
        a.b([0x2d,0x1c,0x10,15,0x7e,0x2d,0x1c,0x10,15,0x7e,0x2e,0x1c,0x60,0x4f])
        a.b([0x1b,0x2d,0x1c,0x10,15,0x7e,0x2e,0x60,0x3c,0x84,2,1]); a.place(Lc); a.b([0x1c,0x1a]); a.br(0xa1,Lb,0); a.b([0x1b,0xac])
    K['int[] rmw       '] = (b2, 3)
    def b3(a):   # nested loops, mul/shift/xor
        a.b([0x03,0x3c,0x03,0x3d]); Lci,Lbi,Lcj,Lbj=a.lab(),a.lab(),a.lab(),a.lab(); a.br(0xa7,Lci,0); a.place(Lbi)
        a.b([0x03,0x3e]); a.br(0xa7,Lcj,0); a.place(Lbj)
        a.b([0x1b,0x1c,0x1d,0x68,0x1b,0x06,0x7a,0x82,0x60,0x3c,0x84,3,1]); a.place(Lcj); a.b([0x1d,0x10,30]); a.br(0xa1,Lbj,0)
        a.b([0x84,2,1]); a.place(Lci); a.b([0x1c,0x10,30]); a.br(0xa1,Lbi,0); a.b([0x1b,0xac])
    K['nested mul/shift'] = (b3, 3)
    def b4(a):   # field update
        a.b([0x03,0x3c,0x03,0x3d]); Lc,Lb=a.lab(),a.lab(); a.br(0xa7,Lc,0); a.place(Lb)
        a.b([0x2d,0x2d,0xca,0,0,0x1c,0x60,0xcc,0,0, 0x1b,0x2d,0xca,0,0,0x11,0,255,0x7e,0x60,0x3c,0x84,2,1]); a.place(Lc); a.b([0x1c,0x1a]); a.br(0xa1,Lb,0); a.b([0x1b,0xac])
    K['field update    '] = (b4, 3)
    def b6(a):   # byte[] transform
        a.b([0x03,0x3c,0x03,0x3d]); Lc,Lb=a.lab(),a.lab(); a.br(0xa7,Lc,0); a.place(Lb)
        a.b([0x2d,0x1c,0x10,15,0x7e,0x2d,0x1c,0x10,15,0x7e,0x33,0x06,0x68,0x1c,0x60,0x54,0x84,2,1]); a.place(Lc); a.b([0x1c,0x1a]); a.br(0xa1,Lb,0); a.b([0x1b,0xac])
    K['byte[] transform'] = (b6, 3)
    def b7(a):   # if/else inside loop (forward branches)
        a.b([0x03,0x3c,0x03,0x3d]); Lc,Lb,Le,Ld=a.lab(),a.lab(),a.lab(),a.lab(); a.br(0xa7,Lc,0); a.place(Lb)
        a.b([0x1c,0x04,0x7e]); a.br(0x99,Le,0); a.b([0x1b,0x1c,0x60,0x3c]); a.br(0xa7,Ld,0); a.place(Le); a.b([0x1b,0x1c,0x64,0x3c]); a.place(Ld)
        a.b([0x84,2,1]); a.place(Lc); a.b([0x1c,0x1a]); a.br(0xa1,Lb,0); a.b([0x1b,0xac])
    K['if/else in loop '] = (b7, 3)
    return K

N = 1000
# ---------------- v2 ----------------
L2 = ctypes.CDLL(sys.argv[1] if len(sys.argv) > 1 else './libdyn_v2.so')
for n in "h_cache_base h_cache_size h_entry h_codeptr h_jit_size".split(): getattr(L2, n).restype = ctypes.c_uint32
L2.h_init()
O2 = {k: getattr(L2, 'h_off_' + k)() for k in ("frame_pc","frame_sp","frame_locals","thread_budget")}
def run_v2(code, max_locals=8):
    L2.h_reset(); i = L2.h_add(code, len(code), max_locals, 8, 1)
    if not L2.h_compile(i): return None
    base = L2.h_cache_base(); snap = bytes((ctypes.c_uint8 * L2.h_cache_size()).from_address(base))
    e = Emu(snap, code_base=base)
    refvm.OFF_SUPER = 0; init_heap(e, __import__('random').Random(1))
    for k in range(8): e.w32(LOC + 4 * k, 0)
    e.w32(LOC, N); e.w32(LOC + 12, ARR_I); e.w32(LOC + 12, ARR_I)
    e.w32(T_ADDR + O2['thread_budget'], 10 ** 7); e.w32(F_ADDR + O2['frame_pc'], L2.h_codeptr(i)); e.w32(F_ADDR + O2['frame_sp'], STK); e.w32(F_ADDR + O2['frame_locals'], LOC)
    rc = e.run(L2.h_entry(i) - base, T_ADDR, F_ADDR, max_steps=5000000)
    return rc, e.steps, s32(e.r32(LOC)), L2.h_jit_size(i), len(code)
print("%-17s %12s %10s %10s   (emulated Xtensa instructions per loop iteration, N=%d)" % ("kernel", "instr/iter", "native B", "bytecode B", N))
for name, (bld, _) in kernels().items():
    code = asm_prog(bld)
    r2 = run_v2(code)
    if r2 is None: print(name, "not compiled"); continue
    iters = 900 if 'nested' in name else N
    print("%-17s %12.1f %10d %10d" % (name, r2[1] / iters, r2[3], r2[4]))
