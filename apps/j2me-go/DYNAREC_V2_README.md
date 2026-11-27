# j2me-go v2.0 — new dynarec (complete project)

This is the complete v1.5 project tree with the dynarec rewritten. Compared with v1.5 only these differ:

* `main/nanojava/src/vm/dynarec.c` — the rewrite (every other source file, header and `interp.c` is byte-identical to v1.5)
* `DYNAREC_ARCHITECTURE.md` — rewritten for v2
* `DYNAREC_V2_README.md` — this file
* `tools/dynarec-test/` — host-side test harness (outside the CMake source globs, so it is not built into the firmware)

## Build
Build exactly as before (`idf.py build`). No CMake or sdkconfig changes.
Optional knobs (compile definitions): `JIT_VERBOSE=1` (compile/reject log), `JIT_LOCAL_REGS=0..3` (default 3).
Memory: about 0.7 MB of extra PSRAM for compile buffers, allocated once in `dynarec_init`.

## Measured (emulated Xtensa instructions per loop iteration, same bytecode, v1.5 -> v2)

| kernel            | v1.5 | v2   | fewer |
|-------------------|-----:|-----:|------:|
| sum loop          | 15.0 |  7.0 | 2.1x  |
| int[] read-modify-write | 105.0 | 32.0 | 3.3x |
| nested mul/shift/xor | 43.6 | 10.4 | 4.2x |
| field update      | 56.0 | 18.0 | 3.1x  |
| byte[] transform  | 81.0 | 25.0 | 3.2x  |
| if/else in loop   | 31.0 | 10.0 | 3.1x  |

Generated code is also smaller. These are instruction counts, not wall-clock time.

## How it was verified (on a PC, in an Xtensa emulator)
`tools/dynarec-test/` contains the project's real `dynarec.c` built for the host, a small Xtensa emulator that executes the
emitted machine code, a reference bytecode interpreter, and a random program generator (loops in three forms, ternaries
with values live across branches, tableswitch/lookupswitch, arrays of every type, fields, statics, long, float,
dup_x*/swap, invokes, in-expression assignments, NaN/inf, division by zero, null refs, out-of-bounds, ArrayStore).
The JIT runs the way the interpreter drives it (yield, deopt, resume after invoke, loop re-entry, tiny time slices).
Result, locals (on exceptions), heap and exact time-slice accounting are compared against plain interpretation.

* more than 20,000 random programs x 3 inputs x 3 modes: **0 mismatches, 0 rejections**
* 4 register configurations (`JIT_LOCAL_REGS` 0,1,2,3) and programs up to ~12 KB of bytecode
* cache-full flush under load: 400 large methods compiled across 8 flushes, all verified
* **mutation testing**: 30 deliberately injected bugs (wrong carry, dropped bounds check, un-saved locals, off-by-one
  stack pointer, wrong switch bias, ...): every one is detected (28 by output mismatch, 2 by an infinite loop)

Run it: `cd tools/dynarec-test && bash build.sh libdyn.so && python3 drive.py 0 2000 ./libdyn.so`
(`drive.py START COUNT LIB [SCALE]`; `mutate.py`, `bench.py`, `featscan.py` are described in `tools/dynarec-test/README.txt`; needs gcc and python3).

## What this does NOT prove — check on the device
The emulator implements the instruction semantics from the Xtensa manual; the emitter's *encodings* were already
checked against the assembler by the project. Not verified here, because it needs silicon:
1. `trunc.s` on NaN / out-of-range (`(int)NaN` must be 0, `(int)1e10f` must be 2147483647). A NaN guard is emitted,
   saturation is the hardware's.
2. Real FPU compare/move (`olt.s`, `oeq.s`, `movt`, `bt`) and the FPU/coprocessor state when JIT code runs.
3. Using `a11..a15` in the windowed ABI (standard for leaf functions; confirm no window-overflow surprises).
4. PSRAM instruction-cache sync and `l32r` from PSRAM (same mechanism as v1.x).
5. Real timing, GC/thread interaction, and the interpreter hand-off (`interp.c` is unchanged and was not run).

First run: build with `-DJIT_VERBOSE=1`, play one game, and look for `[dynarec] reject ...` lines. If anything is off:
`-DJIT_LOCAL_REGS=0` removes register caching, `NJ_ENABLE_DYNAREC 0` removes the JIT.

## Known limitations / roadmap
* Invokes, `new`, `checkcast/instanceof`, long multiply/shift, doubles still go through the interpreter. After a
  non-invoke yield the rest of that call is interpreted until the next call return or loop back-edge.
* Next, in order of payoff: inline `checkcast/instanceof` (class chain), direct calls between compiled methods,
  64-bit shifts/multiply, forward short branches (2-pass layout), result-register targeting for `istore`.
