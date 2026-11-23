# NanoJava dynarec v2 — architecture

`main/nanojava/src/vm/dynarec.c` is a complete rewrite. The public interface is unchanged
(`dynarec_init / compile_method / run_method / flush_cache / check_pending_flush / reset / shutdown / dump_stats`),
`dynarec.h`, `interp.c`, `vm.h`, `xjit_emit.h`, `xjit_exec.*` are the v1.5 files, untouched.

## 1. Contract with the interpreter (unchanged)

* Compiled code is a C function `int f(Thread*, Frame*)` returning `JIT_OK`, `JIT_DEOPT` or `JIT_YIELD_CALL`.
* Compiled code is **only ever left by returning**; no native frame is ever suspended. So the cache can be
  flushed at any time and a frame can always continue in the interpreter.
* Whenever it returns, locals, operand stack, `f->sp`, `f->pc` and `t->budget` are exactly what the interpreter expects.
* `JIT_OK`: the return value is in `f->locals[0..ret_slots)`.
* `JIT_YIELD_CALL`: the interpreter executes the instruction at `f->pc`. Invokes also register a resume point, so the
  interpreter re-enters compiled code after the call returns (`jit_entry`).
* `ACC_JIT_OSR`: the method has loop headers that are entry points (on-stack replacement at interpreter back-edges).
* `ACC_JIT_UNSUPPORTED` / `m->jit_fails`: negative cache (hard reject, or soft retry up to 10×).

## 2. Pipeline

```
analyze()   decode, resolve/quicken constants and fields, find branch targets, entry points (start, invoke
            returns, loop headers), reachability, and pick the hot locals to keep in registers
gen_method  one forward pass over reachable code -> staging buffer (virtual stack, see 3)
layout      [literal pool][body]; l32r displacements patched; copied with 32-bit stores; cache sync
```

Dead code is not emitted (only code reachable from an entry point). Native size is about 5x the bytecode.

## 3. Code generation

**Virtual operand stack.** Inside a basic block, stack entries are `CONST`, `LOCAL` (lazy local read), `LREG`
(cached local register), `REG` (temporary) or `MEM`. Nothing is written to memory until a block boundary or an exit, so
`iconst/iload/iadd/istore` sequences become 1–2 instructions with no stack-pointer updates. `iconst`+op folds,
`x * 2^k` becomes a shift, `x & 0xFF` becomes `extui`, small constants use `addi/beqi/blti/...`.

**Stack depth is never needed statically.** `a5` holds the *current* stack pointer (biased by `max_stack*4` so all
offsets are non-negative). It is re-synchronised at block boundaries (`flush_all`). That is what makes invokes
yield-able without parsing descriptors.

**Cached locals.** Up to 3 locals (chosen by use count weighted by loop depth; never ones touched by long/double
ops) live in `a12..a14` for the whole call: loaded in the prologue, written back at every exit.

**Registers**: `a2` Thread*/status, `a3` Frame*, `a4` locals, `a5` biased sp, `a11` time-slice budget,
`a10`/`a15` scratch, `a6..a9` temporaries, `a12..a14` cached locals (else temporaries).

**Exits.** Rare paths (null, bounds, divide by zero, budget expiry, aastore slow path) branch to a *cold stub* that
stores the virtual stack, `f->sp`, `f->pc`, the cached locals and the budget and returns. Stubs are emitted in
"islands" after unconditional jumps, or when a branch would get out of range. The null check also rejects the
interpreter's BAD_REF range (1..0xFFF), so a corrupt small integer cannot fault the CPU.

**Time slice.** Every backward branch does `addi a11,-1; bltz a11,stub`; the budget lives in a register and is
stored back at every exit. A compiled loop therefore cannot starve other threads.

**Entry.** Prologue loads state, then a **binary search** on `f->pc - code` jumps to the method start, an invoke
resume point or a loop header; an unknown pc returns `JIT_DEOPT` untouched.

**Switches.** `tableswitch` = range check + jump table in the literal pool; `lookupswitch` = binary search.

## 4. What is compiled natively vs. yielded

Native: all int/long(add, sub, and, or, xor, neg, cmp, i2l, l2i)/float(add, sub, mul, neg, cmp, i2f, f2i) arithmetic,
constants, locals, all array loads/stores (aastore with a class-chain check), `getfield/putfield` (quickened),
`getstatic/putstatic` (initialised classes), `arraylength`, every branch, both switches, returns, all stack ops.

Yielded to the interpreter: invokes (with resume), `new/newarray/anewarray/multianewarray`, `checkcast/instanceof`,
`athrow`, `monitorenter/exit`, long `mul/div/rem/shifts`, `fdiv/frem`, all double ops, `f2l/l2f` and friends.

## 5. Behaviour changes vs v1.5

* `aastore` now performs the ArrayStoreException check (v1.x skipped it); a class-chain walk is inline, anything it
  cannot prove goes to the interpreter.
* Null checks cover the BAD_REF range.
* Methods with exception handlers, `synchronized` methods, and methods using `tableswitch`/`lookupswitch` compile.
* `JIT_LOCAL_REGS` (0..3) and `JIT_VERBOSE` are the compile-time knobs.
* `bastore` does not mask boolean arrays with `& 1` (javac only stores 0/1; v1.x did not mask either).
