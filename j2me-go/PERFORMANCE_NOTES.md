# j2me-go — performance and correctness notes (changes over v1.7)

This document records the changes made to the NanoJava Retro-Go port in this
repository, why they were made, and what was measured along the way. Read it
before changing the display pipeline, threading, the sleep policy or the
telemetry itself.

Everything below touches **only** the `j2me-go` app (`sdkconfig.defaults`,
`CMakeLists.txt`, `main/`).  The one system file reproduced in this folder is
`components/retro-go/rg_gui.c`: an app-local copy patched to add the **Emulator
Options** row to the game menu.  It checks the app id and leaves the stock
behaviour untouched for every other app.

Build:

```
python rg_tool.py build j2me-go --target my-handheld
```

## 1. The intended cache sizes now actually take effect  (`CMakeLists.txt`, `sdkconfig.defaults`, `sdkconfig`)

This is the single biggest change for this workload: the Java heap, every
decoded image, all class metadata and all method bytecode live in PSRAM, so
nearly every bytecode that touches a field, an array element or an instruction
fetch goes through the data cache.  The app asked for

* instruction cache 16 KB → **32 KB**
* data cache 32 KB → **64 KB** (8-way, 32-byte lines)

from the beginning (`sdkconfig.defaults`), but it never took effect — and it
still did not after the first attempt to fix it, for two reasons worth writing
down:

1. `CMakeLists.txt` appended `sdkconfig.defaults` only `if(DEFINED
   SDKCONFIG_DEFAULTS)`, but the build driver passes that value in the
   *environment* (`rg_tool.py` → `SDKCONFIG_DEFAULTS=<board sdkconfig>`), so the
   condition never matched and the app's defaults file was never read at all:
   `tools/cmake/project.cmake` reads the environment itself and, when it is set,
   never falls back to the project's `sdkconfig.defaults`.  Fixed by promoting
   the environment value to the CMake variable `project.cmake` prefers.
2. Even with that fixed, a *generated* `sdkconfig` always outranks every
   defaults file.  Writing values into `sdkconfig` by hand works for the next
   plain build, but "release"/"clean" deletes and regenerates it, silently
   losing them.  `sdkconfig.defaults` is the authority; the generated
   `sdkconfig` in the tree is disposable.

On ESP32-S3 the cache sizes are applied by the application itself at startup
(`esp_system/port/cpu_start.c` calls `rom_config_instruction_cache_mode()` /
`rom_config_data_cache_mode()`), **not** by the bootloader, so a bigger cache
needs no bootloader reflash and cannot affect other apps.  The cost is internal
SRAM: the extra 32 KB of data cache and 16 KB of instruction cache come out of
the heap (~48 KB; internal free drops from ~266 KB to ~218 KB), which is what
the original `sdkconfig.defaults` comment already accounted for.

Cache line size stays at 32 bytes (the original file asked for 64).  The VM's
PSRAM access pattern is scattered — many small objects — so more lines are
worth more than a longer burst; it is one line to flip if a game turns out to be
streaming-dominated instead.

## 2. VM thread stacks must stay in internal SRAM

`nj_pal_calloc_fast()` is used **only** for the per-thread operand stack and
frame array — the memory the interpreter touches on almost every bytecode.
Its "always leave this much internal RAM free" reserve was 100 KB, which would
have started pushing thread stacks to PSRAM once the caches grew.  Lowered to
48 KB; IDF's own `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` (32 KB) still protects
internal-only system allocations.

## 3. Cheaper profiling timestamps (`interp.c`, `heap.c`, `pal_retrogo.c`)

`esp_timer_get_time()` is an out-of-line function that calls an indirect
converter and does 64-bit math.  It was called **twice per native method
call** (thousands per frame — every `fillRect`, `drawImage`, `drawRGB`, …) and
twice per interpreter time slice.  The accumulators are now plain CPU cycles
from `esp_cpu_get_cycle_count()` (a single `CCOUNT` read, always inlined) and
are converted to microseconds only when the telemetry line is printed.
Power management is disabled and the clock is fixed at 240 MHz, so the
conversion is exact.  The numbers printed in the log are unchanged.

## 4. `convert_frame()` rewritten (`pal_retrogo.c`)

Runs once per presented frame.  The old version, per pixel, did a `switch` on
the rotation, two bounds checks, and (when not 1:1) a division — on Xtensa a
division is a library call.  It also cleared the whole output buffer with
`memset` on every frame even though the image then overwrote all of it.

Now the rotation is chosen *outside* the inner loop, each case walks the source
with a constant pointer step, the redundant clear only happens when the image
does not cover every output pixel, and the scaled path uses an exact Bresenham
accumulator instead of dividing.

All four rotations were checked against the old implementation for 400
combinations of canvas size (10 sizes, incl. ones larger than the output),
output size (5 sizes), rotation and placement mode — the output is
**bit-identical**.

## 5. Waits sleep instead of spinning  (`sdkconfig.defaults`, `pal_retrogo.c`)

`CONFIG_FREERTOS_HZ` was 100, i.e. a tick was 10 ms, with two consequences:

* `pdMS_TO_TICKS()` rounds every delay below 10 ms down to **zero ticks**, and
  `taskYIELD()` does not idle the CPU, so `pal_wait()` — the function behind
  `Thread.sleep()` — never slept at all: it busy-yielded for the whole wait.
  The "idle" column of the telemetry was therefore CPU time, not idle time, and
  lower-priority work was only serviced at tick boundaries.
* The present worker waits for the display with
  `while (rg_display_is_busy()) vTaskDelay(1);`, i.e. up to 10 ms of added
  latency per frame.

Raised to **1000 Hz** in `sdkconfig.defaults`.  This is a FreeRTOS-only setting:
no memory-map, bootloader or other-app impact.

`pal_wait()` was also made tick-aware instead of assuming 1 ms ticks: it delays
whole ticks and spins only the sub-tick tail, and when less than one tick
remains it hands the CPU over for one tick rather than burning it.  A
short-sleep-heavy game (section 8) is the main beneficiary.

## 6. Interpreter at `-O3` (`main/CMakeLists.txt`)

The component was built at `-O2` on top of the firmware-wide `-Os`
(`CONFIG_COMPILER_OPTIMIZATION_SIZE`).  `-O3` grows `interp.c` ~14 %, but the
interpreter is linked into IRAM (zero-wait), where ~260 KB of the region is
free, so the extra code costs nothing at run time.  Flash use is still ~6 % of
the partition.

## 7. Dead code removed

`pal_retrogo.c` still carried `s_colmap`/`s_rowmap` (4 KB of BSS plus their
invalidation bookkeeping) from a removed scaled-blit path.  Removed.

---

## 8. Short sleeps were dead time (`thread.c`, `nanojava_main.c`, `vm.h`)

A flashed-device log of a game sitting on its loading screen showed the shape
of the problem (this is the steady state, well after startup):

```
FPS 1.0 | present 7.0 | VM 87.5 | native 28.7 | bytecode-est 58.8 | GC 0.00 |
idle 971.8 | frame 1064.1 | sleeps 318.0/frame, requested 2017.0 ms/frame (avg 6 ms/call)
```

`idle` is 91 % of the frame, and `971 ms / 6 ms` is about how many 6 ms sleeps
can hide in it. In other words the game's progress loop is paced by
`Thread.sleep(6)`, not by its own work. The port already shortens
frame-limiter sleeps to 25 % of the request, but only for sleeps of 16 ms or
more, so these 6 ms poll sleeps were honoured exactly and ~970 ms per second
was thrown away.

Sleeps of 2 ms and up are now shortened too (`nj_sleep_min_ms`, default 2),
with the same 1 ms floor so the thread still yields a tick to the display
worker, Retro-Go's input task and the audio task. This is a **per-game**
option: the Emulator Options menu has a new **Frame pacing** row.

| Setting | Sleeps >= 2 ms | Sleeps >= 16 ms | Notes |
|---|---|---|---|
| Accurate | kept | kept | most faithful, slowest |
| Balanced | kept | 25 % | previous build's behaviour |
| Fast (default) | 25 % (min 1 ms) | 25 % | poll/loading loops progress 3-6x faster |
| Turbo | 1 ms | 1 ms | only for games that throttle themselves hard |

A game that paces itself against a clock
(`while (now < next) Thread.sleep(5);`) is not affected - it reaches the same
deadline with more sleeps - but a game that counts its own sleeps runs faster
than the phone it was written for. Set that game to **Balanced** (or
**Accurate**) if it becomes too fast.

Note that only `Thread.sleep()` is shortened. `Object.wait(timeout)` and
`Display.getEvent(timeout)` keep their exact timeouts, so event-driven pacing
is untouched.

## 9. Stuck and crawling games now identify themselves (`interp.c`, `pal_retrogo.c`)

`vm_dump_threads()` prints every Java thread with its scheduler state and the
top frames (class.method, source line and bytecode offset). It is called from
`pal_poll_event()` - outside `interp_run()`, so every frame's pc is current -
when either

* no frame has been presented for 5 s (a hang), or
* three consecutive telemetry windows were below 5 FPS (a crawl).

Dumps are rate limited to one per 10 s (the "crawl" dumps are additionally
capped at 3 per run; a hang keeps reporting, so a log left running shows
whether the state ever changes). A log from a
stuck game therefore ends with something like

```
[stall] frame rate below 5 FPS - thread dump:
[stall] t0 SLEEP daemon=0 depth=4 wake=4ms
[stall]   at com/foo/Game.run(Game.java:412) line=143 pc=2311
[stall]   at java/lang/Thread.sleep(Native Method)
[stall] end of dump (2 threads)
```

which is what is needed to tell a poll loop apart from a real deadlock or from
a thread waiting for something that never arrives.

That is how the first real hang was diagnosed (Assassin's Creed Revelations on
a Nokia C3-00 profile): `Display.run` sat in `EVENT` wait forever because the
game's own loader thread (`h.run`) had died from an **uncaught
`java.lang.OutOfMemoryError`**.  The event thread is a non-daemon MIDlet thread,
so the VM kept running with nothing left to make progress — and `vm.uncaught`
was only consulted *after* `vm_run()` returned, which it never did.

Two things follow from that, changes 10 and 11.

## 10. Every `OutOfMemoryError` now says what failed (`heap.c`, `nanojava_main.c`)

The collector is **non-moving** (conservative stack scanning plus an
object-start bitmap), so a full heap cannot be defragmented: once there is no
contiguous run for a request, `alloc_chunk()` has collected and retried and the
allocation fails for good.  Before, that ended in a bare
`java.lang.OutOfMemoryError` and a guess.  Now every failure is reported with
the three numbers that tell the cases apart:

```
nanojava: OutOfMemoryError - allocation needs 1179664 bytes; heap 6016 KB, 4297 KB free, largest free chunk 659968 bytes
```

* `free` small → the heap really is full; a bigger heap is the answer.
* `free` large but `largest free chunk` small → fragmentation; more heap helps
  only up to a point, and the non-moving collector cannot fix it.
* the request is bigger than the whole heap, or it names `StringBuffer` → a
  game or class-library bug that no heap size cures (that is change 12).

The report comes with a thread dump showing who asked, and is rate limited to
one every 2 s (a game that catches the error and retries would flood the log).

The heap size itself is **unchanged from v1.7**: `3 MB + 2 × JAR`, floored at
4 MB, capped at 6 MB and at `largest PSRAM block − 2 MB` - 5240 KB for the
1084 KB JAR above.  An intermediate build raised that to
`largest free PSRAM block − 2 MB` (6016 KB) on the theory that the loading
failures were a lack of room.  The OOM report above shows they were not, so the
change was reverted: `Runtime.freeMemory()` / `totalMemory()` hand those numbers
to the game, some titles adapt their allocation to them, and the point of this
work is that a game behaves the same as before apart from being faster.
`NJ_HEAP_MAX` in `nanojava_main.c` (and the `2u << 20` reserve in the clamp) are
the knobs if a log ever shows a genuinely full heap.

## 11. A dead MIDlet thread is reported, not killed (`interp.c`, `vm.h`, `pal_retrogo.c`)

`vm.uncaught_count` counts uncaught Java exceptions.  `pal_present()` remembers
that counter whenever a frame goes out, so the PAL can tell "an exception killed
a thread *and nothing has been drawn since*" from "an exception happened ages
ago and the game is fine".

When that is true and the stall detector has dumped twice in a row (no frame for
5 s, then ≥10 s more), the log says so once:

```
nanojava: a Java thread died from an uncaught exception and nothing has been drawn since; if the game stays frozen, exit with the menu
```

The app **does not** end the MIDlet itself.  An earlier build did queue
`EV_QUIT` there, and that turned out to be too eager: a title that simply needs
15 s to load, after a *harmless* uncaught exception in a helper thread (audio is
stubbed, resource parsers throw and recover), looks exactly like a dead game -
and quitting it turns a slow title into a "crash".  It is also not possible to
detect "nothing can make progress" reliably: the loader thread in the diagnosis
below is a poll loop that sleeps 6 ms at a time, so it always looks alive.

The dumps remain (change 9), and they say which method the game is in, which is
what the fix needs.  Leaving is one menu press away.

## 12. Reusing a `StringBuffer` no longer grows it forever (`main/nanojava/classlib`, `nanojava_classlib.jar`)

This is the fix for the loading-screen failures, and it is a plain bug in the
class library rather than in the VM.

`StringBuffer.toString()` shares its `char[]` with the returned `String` and sets
a `shared` flag; the next in-place write has to copy the array first.
`ensure(minimum)` did that copy **and** grew the array to `value.length * 2 + 2`
whenever `shared` was set - even when `minimum` was `count`, i.e. when no extra
room was needed at all.  Four methods that only rewrite the array in place were
passing exactly that:

| method | call | effect |
| --- | --- | --- |
| `setCharAt` | `ensure(count)` | doubles the array on the first write after `toString()` |
| `setLength` (shrinking, e.g. the usual `setLength(0)` reset) | `ensure(newLength)` | doubles it |
| `delete` | `ensure(count)` | doubles it |
| `reverse` | `ensure(count)` | doubles it |

A game that reuses one buffer - append, `toString()`, `delete()`/`setLength(0)`,
append again - therefore doubled its capacity on every reuse while adding no
data.  Starting from the default 16 chars: 16, 34, 70, ... 294910, 589822.  The
last step asks the heap for `20 + 2 × 589822 = 1179664` bytes, which is exactly
the request in the Assassin's Creed Revelations log, after 13 reuses of a buffer
whose content never exceeded 23 characters:

```
nanojava: OutOfMemoryError - allocation needs 1179664 bytes; heap 6016 KB, 4297 KB free, largest free chunk 659968 bytes
[stall]   at java/lang/StringBuffer.ensure(StringBuffer.java) line=39 pc=34
[stall]   at java/lang/StringBuffer.delete(StringBuffer.java) line=146 pc=43
```

4.3 MB were free; the request could not fit in one piece, so the paint thread
died and the game stayed on the loading screen.  No amount of heap would have
cured it - it grows without bound - and several games share the pattern, which
is why more than one title failed while loading.

The fix is a private `unshare()` that copies the array at its **current**
capacity and clears the flag; the four in-place methods call it instead of
`ensure()`.  `setLength` now grows only when the new length really is larger
than the capacity.  Behaviour is unchanged otherwise (a `String` taken before a
write still never sees that write), and the capacity no longer moves:

```
reuse loop, shared: capacity 34 -> 34 after 1000 rounds
```

The class library is `main/nanojava_classlib.jar`, embedded by
`main/CMakeLists.txt`, so the jar was rebuilt from
`main/nanojava/classlib/src`.  Those sources reproduce the shipped jar
byte-for-byte with

```
javac --release 8 -nowarn -encoding UTF-8 -sourcepath main/nanojava/classlib/src -d <out> @sources.txt
```

so the rebuilt jar differs from the original in exactly one entry
(`java/lang/StringBuffer.class`, still class file version 52, same 223 entries
in the same order).  If you ever rebuild it, keep `--release 8`: newer class
file versions are not what this VM was tested with.

## 13. `flushGraphics()` no longer copies the frame twice (`Display.java`, `nanojava_classlib.jar`)

A `GameCanvas` game paints into its own `Image` and calls `flushGraphics()`.
That used to run the frame through three buffers on every frame:

1. game buffer → display buffer (`Graphics.drawImage`, a full-screen blit),
2. display buffer → snapshot (`pal_present`, a full-screen `memcpy`),
3. snapshot → RGB565 output (the display worker).

Step 1 existed for the case where the MIDlet paints a command menu *over* the
game.  When no menu is shown - the normal in-game case - the display buffer is
just a copy of the game buffer, so `flush()` now presents the game buffer
directly.  That is one full-screen blit less per frame: at 320x240 it is 150 KB
read plus 150 KB written, roughly 2-4 ms of PSRAM traffic, which is 5-10 % of a
30 FPS frame for the side-scrollers this round targets.  When a menu is open,
or the flush does not cover the whole canvas, the old path is used unchanged.

Nothing else in the class library changed; rebuilding the jar from
`main/nanojava/classlib/src` (see section 12) reproduces the shipped jar except
for `javax/microedition/lcdui/Display.class`.

## 14. Keypad layout and the diagonal number keys (`pal_retrogo.c`, `nanojava_main.c`)

The Retro-Go buttons now send the key codes a Nokia MIDlet expects:

| button   | MIDP key                        |     | button | MIDP key |
|----------|---------------------------------|-----|--------|----------|
| D-pad    | up 2, down 8, left 4, right 6   |     | Y      | `0` (48) |
| A        | FIRE / centre 5 (-5)            |     | B      | `*` (42) |
| Select   | left soft key, "OK" (-6)        |     | X      | `#` (35) |
| Start    | right soft key, "Back" (-7)     |     |        |          |

Soft keys matter for the ports that draw a menu with "Options"/"Back" on them;
before this, `X`/`B` were the soft keys and neither Select nor Start did
anything a menu could use.

Some ports only accept `1`, `3`, `7` or `9` (not a direction) as the key that
advances a screen, and this handheld has no numeric keypad.  **Emulator Options
→ Keypad diagonals** therefore turns a *held* diagonal into the matching digit:

```
up+left -> 1     up+right -> 3     down+left -> 7     down+right -> 9
```

The pair has to be held for the chosen time (0.5 s, 0.75 s or 1 s); the digit
is then pressed and stays pressed until the pair is released.  The two
direction keys stay pressed as well, so diagonal movement is untouched - which
is also why the option is **off by default**: a game that uses diagonals to
move would otherwise get a stray number key whenever the player holds one.
Enable it per game, and it is stored in that game's cfg as `diag_ms=500`
(`0` = off).

## 15. The telemetry line names the expensive native methods (`interp.c`, `vm.h`, `pal_retrogo.c`)

`native` in the FPS line says how much time was spent inside native methods but
not which ones.  Every native call is now accounted to a 64-entry table keyed by
its function pointer (a few cycles per call, no allocation), and once per second
the three methods with the most CPU time are printed after the FPS line:

```
native 1834.0 calls/frame, top: javax/microedition/lcdui/Graphics.drawRegion0 14.2, ...fillRect 6.9, ...drawString0 1.1 ms/frame
```

That separates "many cheap calls" from "two expensive blits" and points at the
function to optimise; the table is cleared with the other counters, and the
whole thing disappears with Performance logging off.

## 16. Which Java methods own the interpreted time (`interp.c`, `vm.h`, `pal_retrogo.c`)

`bytecode-est` is the largest column in a slow game, but one number cannot say
whether it is the game's own code or the class library.  Every VM time slice
(`TIME_SLICE` = 2000 backedges/calls, roughly 15-30k bytecodes, ~250 us) is now
charged to the method that was on top of the stack when the slice started, and
the three largest are printed once per second:

```
java 412 slices/frame, top: com/gloft/asbr/GameMain.run 78.4, javax/microedition/lcdui/game/TiledLayer.paint 12.1, com/gloft/asbr/World.step 9.8 ms/frame
```

Two details make it safe to leave on:

* it samples per slice, not per bytecode: two `CCOUNT` reads and one table
  update per ~250 us, well below 0.1 % of a frame.  A per-bytecode counter
  would cost 10-15 % (a load, a test and a branch on every dispatch), which is
  exactly the reason for sampling;
* native time is subtracted (the `nj_profile_native_cyc` delta across the
  slice), so `Thread.sleep` and drawing do not pollute the Java numbers; they
  are already in the native line.

A method is charged for the whole slice it *starts* in, so very short methods
(accessors, small helpers) are folded into their caller.  That is the right
trade-off for the question it answers: is the game itself heavy, or is the class
library the reason?

How to read it: if the top entries are game classes (`com/gloft/...`), the VM is
already executing as little as it can and only a faster interpreter helps.  If
they are `javax/microedition/...` or `java/...`, the class library is the place
to work - and that code is testable on a PC before it ever reaches the handheld.

## 17. Why there is no dynamic recompiler (yet)

A JIT that turns bytecode into Xtensa code would be the real fix for
`bytecode-est`, and it is not impossible on this chip, but it is not something
that can be added safely in this tree.  The measured constraints:

| What | Value |
|---|---|
| Executable memory | IRAM only.  PSRAM is not executable on the ESP32-S3. |
| IRAM (DIRAM region) | 341,760 bytes total, 136,946 used statically at link time |
| Free at link | ~204 KB - but the ~147 KB internal heap Retro-Go and the drivers use comes out of the same pool |
| VM interpreter | `interp_run` already lives in IRAM (0x4037xxxx) |
| Flash code | 306 KB, executed through the instruction cache |

So a code cache would have to be a fixed IRAM arena of maybe 32-64 KB, filled
with generated code, while a J2ME game's translated code needs megabytes: the
JIT must therefore translate on demand, evict, and *relocate* already translated
blocks, because Xtensa branches and `l32r` literals are PC-relative.  On top of
that: hand-written LX7 encoding, the windowed ABI for calls into C natives, GC
safepoints for every live reference (this VM's collector walks the C stack and
the interpreter's `Frame` arrays), exception tables that must remap bytecode
offsets to code offsets, `Thread.sleep`/`wait` yields from inside compiled code,
and deoptimisation when a constant pool entry or a class initialisation is still
unresolved.

Realistically that is a few thousand lines of new code and weeks of work on the
device, and it still would not make the heaviest titles fast.  The interpreter
spends a large part of every bytecode on the dispatch itself (an indirect jump
on a core without a branch-target buffer) plus operand traffic to PSRAM; a JIT
removes most of the former and some of the latter, which is a factor of roughly
two in the best case - 7 FPS becomes ~14, not 30.

The cheaper ladder, in order of payoff per risk, is:

1. **Superinstructions**: fuse the most frequent 2-3 opcode sequences
   (`aload_0; getfield`, `iload; iload; iadd`, array load/store forms) so one
   dispatch does the work of three.  Dispatch is ~30 % of the 17 cycles, so this
   is worth ~15-25 %.  It needs the real opcode mix, which is why section 16
   exists.
2. **Field/method quickening**: the `RESOLVE` macro already reads a resolved
   pointer from the constant pool; extending it to the same style inline cache
   the interface calls use (`icache_lookup`) removes the last indirections.
3. **Class library**: anything that shows up in the `java ... top:` line is
   ordinary Java and can be optimised and unit-tested on a PC (as the
   `StringBuffer` fix and the `flushGraphics` fast path were).

All three keep the interpreter's semantics, need no executable memory and no
relocation, and are testable without risking the GC or the ABI.

## 18. `currentTimeMillis()` had one-second resolution (`pal_retrogo.c`)

**Symptom.** In some 2D titles the character runs and animates normally but the
world behind it crawls: the camera/parallax layers move a fraction of the
distance they should, which makes scrolling games (the Assassin's Creed ports
are the reported example) unplayable even though the frame rate is no worse
than in comparable titles.

**Cause.** `pal_epoch_ms()`, which backs `System.currentTimeMillis()`, was:

```c
time_t now = time(NULL);
if (now > 1000000000) return (int64_t)now * 1000;   /* whole seconds! */
return 1704067200000LL + pal_time_ms();             /* monotonic fallback */
```

On a handheld whose clock is set (RTC or SNTP) the first branch is taken and
`time()` truncates to seconds.  Every frame in between a game sees
`dt = now - last == 0`; once a second it sees `dt == 1000`, which the usual
`if (dt > 50) dt = 50;` clamp cuts back to 50 ms.  A camera or parallax
integrating that delta therefore advances 50 ms of world movement per second
instead of 1000 ms - about 5-10 % of the intended speed - while anything counted
in frames (sprite animation, per-frame movement) is unaffected.  The fallback
branch (no clock configured) was already correct, which is why only some
devices/games showed it.

**Fix.** Read the same wall clock in microseconds.  ESP-IDF's `gettimeofday()`
is `boot_time + esp_timer_get_time()`, so it keeps the date and gains the
monotonic timer's resolution:

```c
struct timeval tv;
if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1000000000)
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
return 1704067200000LL + pal_time_ms();
```

That is the *correct* result, but see §19: reading the wall clock on every call
is far too slow for games that poll the clock thousands of times per frame, so
the shipped version resolves the offset once and adds `pal_time_ms()`.

This also fixes `java.util.Timer`, `Date` and every game that paces itself with
`currentTimeMillis()` deltas, not just scrolling.

**Audited at the same time, no defect found:** `copyArea` overlap direction and
clip handling, `gfx_blit` clipping/anchor math, `Graphics.translate`/`clipRect`
interaction, `LayerManager.setViewWindow` (correctly does *not* clamp to the
layer bounds) and `TiledLayer.paint`'s visible-range computation.  Those paths
are all spec-correct, so a slow scroll is a timing or frame-rate problem, not a
drawing bug.

**New in the same round:** the **Frame pacing** option gained a fourth setting,
**Turbo** (`{0, 2}`): every `Thread.sleep()` of 2 ms or more becomes 1 ms.  Use
it only for a game that throttles itself so hard that its own sleeps are a
visible part of the frame (check `sleeps` and `sleep_ms` in the telemetry
line); it does nothing for a game that is compute-bound (`sleep_ms ≈ 0`).

## 19. The clock fix cost 36 ms/frame until it was cached (`pal_retrogo.c`, `natives_lang.c`)

**How it showed up.** A telemetry line from a 2D/3D title at 9.7 FPS:

```
FPS 9.7 | present 3.0 ms/frame | VM 144.7 | native 44.4 | bytecode-est 100.3 | ...
native 7277.6 calls/frame, top: java/lang/System.currentTimeMillis 36.6, ...
```

7277 native calls per frame, and **36.6 of the 44.4 native milliseconds** were
`System.currentTimeMillis()`.  That is not the game being silly: the classic
J2ME frame limiter is `while (now() - start < budget) Thread.yield();`, and
`now()` is called on every animation/physics step as well.  The §18 fix routed
each of those calls through `gettimeofday()`, which in newlib takes a reent
lock and in IDF takes a critical section plus a 64-bit division - about 5 us
here.  The second-resolution `time()` it replaced was much cheaper, so fixing
the *resolution* had made the *cost* 36 ms per frame.

**Fix.** Resolve the wall clock once, then advance it with the monotonic timer:

```c
static int64_t s_epoch_base;

int64_t pal_epoch_ms(void)
{
    if (!s_epoch_base) {
        struct timeval tv;
        bool wall = gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1000000000;
        s_epoch_base = wall ? (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 - pal_time_ms()
                            : 1704067200000LL;
        RG_LOGI("Java time base: %d s since epoch (%s)", (int)(s_epoch_base / 1000),
                wall ? "wall clock + monotonic ms" : "monotonic fallback");
    }
    return s_epoch_base + pal_time_ms();
}
```

`pal_time_ms()` is `esp_timer_get_time() / 1000` (a register read), so the call
drops from ~5 us to well under 1 us while keeping millisecond resolution.  The
first call happens on the VM main thread before any game thread exists, so the
lazy initialisation needs no lock.  The startup line still tells a log reader
whether the date is real or the monotonic fallback; it now prints seconds with
`%d` because Retro-Go's logger does not implement `%lld` (the old line appeared
mangled in the log).

**Also in this round**

* `Runtime.gc()` (`natives_lang.c`): CLDC says `gc()` is advisory, but it ran a
  full mark/sweep every time.  One title calls it every frame and spent
  2.8 ms/frame there.  It is now honoured at most twice a second, or immediately
  when the free heap is below 1/8 of the total; allocation failures still
  collect on demand, so this cannot cause an `OutOfMemoryError` that the old
  behaviour would have avoided.
* Display worker (`pal_retrogo.c`): `pal_present()` blocks until the worker has
  consumed the snapshot, and the worker used to convert *and then* wait for the
  panel to pick up the previous frame before signalling.  The game thread
  therefore paid for the conversion, the queue wait and the geometry setup of
  every frame.  The worker now converts and releases the game thread
  immediately, then waits for the display queue and submits; `s_out_w/s_out_h`
  and the conversion are captured per job so the game thread can already be
  preparing the next frame (the snapshot is the only shared buffer, and the
  semaphore still guarantees it is idle).  The telemetry line gained
  `+ convert N ms/frame` so the worker's real cost is visible instead of being
  inferred from `present`.
* **Stretch to screen** is not a frame-rate cost: the conversion runs on the
  display worker on the other core, and the VM only copies the canvas snapshot
  (`present` in the log) which is the same size either way.  It did, however,
  run a Bresenham accumulator plus a multiply per output pixel.  The source
  offset of each output column is now computed once per frame into
  `s_colmap[]`, leaving a table read per pixel; the stretch is also the only
  path that has to touch every output pixel (centred 1:1 copies the canvas
  once).  Use *Centered 1:1* when the canvas already matches the panel: it is
  the cheapest and the sharpest.

## 20. Canvas profiles: sorted, trimmed, and migrated (`nanojava_main.c`)

The menu used to offer nine sizes in an arbitrary order, three of which cannot
be shown on a 320x240 panel without scaling (`240x320`, `240x400`, `360x640` -
after rotation their width exceeds the panel).  The list is now:

```
Auto (JAR name) | 128x128 | 128x160 | 176x208 | 176x220 | 208x208 | 320x240
```

sorted by width then height, every entry with a height of at most 240, plus
`208x208` (square Nokias).  Auto-detection from the JAR filename still knows the
retired sizes, so `Foo [240x400].jar` keeps its native canvas - the removal is
about the menu, not about what a title may ask for.

Because `canvas_profile` is saved per game, renumbering the list would silently
change the canvas of every configured title.  Configs now carry `cfg_ver=2`, and
a config without it is translated through `s_canvas_legacy_map[]` (old 1 -> new
6, 3 -> 3, 6 -> 2, 8 -> 1, ...).  The three retired sizes stay in the table with
`offered = false`: a saved game keeps exactly the canvas it was saved with, the
menu just skips those entries when cycling and shows the size when one is
selected from an old config.

## 21. "Stuck on the loading screen" is usually a missing 3D API (`interp.c`)

A title that ends with

```
Uncaught exception in thread:
java.lang.NoClassDefFoundError: javax/microedition/m3g/Transform
    at o.<clinit>(Unknown Source)
```

is not hung: its main thread died.  `javax.microedition.m3g` is the mobile 3D
API; this build implements MIDP 2.0 and the 2D game API, not M3G, so the title
cannot start (Asphalt 3/4/5, many Gameloft racers and other 3D ports).  The
window keeps the last loading frame because only the event thread is left, and
the existing stall hint then prints `no frame presented for 5 s - thread dump:
... t0 EVENT`.  After the trace, the log now says explicitly that the MIDlet
needs M3G and cannot run, instead of leaving the player to guess.  No
auto-exit was added on purpose (§11): a helper thread dying is harmless and a
slow load looks identical.

If a *2D* title stays on its loading screen, look at the same window: an
`Uncaught exception` line names the class it failed on, and the `java ... top:`
line names what it was doing.  A loading screen that is progressing shows
`FPS` above 0 with `present` running.

## 22. Input lag: one paint per key event (`Display.java`, `events.c`, `pal_retrogo.c`)

**Symptom.** Buttons sometimes feel immediate and sometimes lag by a second or
more, in the same game, with no pattern. Releasing a direction late makes the
character keep running.

**Cause.** `Display.run()` (the MIDP event thread) was:

```java
while (running) {
    if (repaintPending) paint();          // game repaint + full frame present
    ... run callSerially work ...
    int type = Events.next(ev, timeout);  // block for ONE event
    switch (type) { ... deliver it ... }
}
```

So every queued event cost one full paint, and `paint()` is the most expensive
thing in the emulator (the game's `paint()` plus `Screen.present()`, a full
canvas copy, all inside the global `LOCK`). A burst of presses - or simply a
game whose paint takes 100 ms at 10 FPS - put several events in the queue, each
waiting for the paints of the events before it. The MIDP queue holds 64 events,
so the worst case is *seconds* of backlog; that backlog is exactly the "lag
that comes and goes". Once the queue was full, new events were dropped
(including, occasionally, a `KEY_UP` - which leaves the key stuck down).

**Fix.**

* `Display.run()` drains up to 16 queued events **before** painting, then paints
  once for the batch; the blocking wait still delivers a key the moment it
  arrives (`dispatch()` extracted from the old switch). Key auto-repeat moved to
  the top of the loop so it is not tied to the event wait. Delivery now costs
  microseconds per event, and the queue cannot build up: the delay is bounded by
  one paint regardless of how fast the player presses.
* The event queue (`events.c`) drops the **oldest** event when it is full rather
  than the newest, and counts the drops. Keeping the newest input is what a
  player feels; better to lose the tap that is already several frames old than
  to delay the one being pressed now.

**Measuring it.** The telemetry now reports input latency directly:

```
input: 3 events/frame, waited 12.4 ms avg, 61 ms max, 0 dropped
```

The numbers are measured from `pal_poll_event()` (when the platform saw the
button) to `Events.next()` (when the game's event loop read it). If `avg`/`max`
are small and the game still feels laggy, the delay is inside the game (its own
key handling, or `Thread.sleep()` pacing - see **Frame pacing**; *Accurate*
honours a long sleep in full). If `max` is large, the event thread was busy in a
paint, and `dropped` tells you the queue overflowed.

## Verifying the effect

The telemetry line (Emulator Options → Performance logging, on by default) is
the thing to look at; it breaks each frame down:

```
FPS .. | present .. + convert .. ms/frame | VM .. | native .. | bytecode-est .. | GC .. | idle ..
native .. calls/frame, top: <three native methods> ms/frame
java .. slices/frame, top: <three Java methods> ms/frame
input .. events/frame, waited .. ms avg, .. ms max, .. dropped
```

* `bytecode-est` large → the VM/interpreter is the bottleneck (the caches,
  `-O3` and the profiling change target this).
* `native` large → read the second line: it names the native methods that own
  the time (drawing, image handling, timers).  `System.currentTimeMillis`
  should now be a fraction of a millisecond even for games that call it
  thousands of times per frame (change 19).
* `java ... top:` large → the third line says whether that interpreted time is
  in the game's own classes or in the class library (change 16).
* `present` large → the game thread's own canvas copy; `convert` is the display
  worker's rotation/scaling on the other core and only becomes interesting if
  it approaches the frame time (change 4 targets this, and `idle` shows whether
  the VM is blocked waiting for the display).
* `GC` large → allocation rate; a game allocating per frame will show here.
* `idle` large -> the game is asleep, not computing; changes 5, 8 and the
  **Frame pacing** option target this.

Two caveats when reading the line:

* `sleeps` / `requested` count the *shortened* request and sum over concurrent
  threads, so they do not have to match `idle` (the honest wall-clock number).
* In a 1 FPS window a single frame straddles the once-per-second counter reset,
  so `native` can come out slightly larger than `VM` and `bytecode-est` drops
  to 0. The split is only meaningful with several frames per window.

If a game is still slow, the `[stall]` dump (change 9) says which method it is
in. If it stops on a loading screen, look for `OutOfMemoryError` (change 10):
the report gives the requested size, the free bytes and the largest free chunk.
A small "free" value means the heap really is full; a large one with a small
largest chunk means fragmentation (more heap helps only up to a point); a
request larger than the whole heap, or one that names `StringBuffer`, is a game
or class-library bug that no heap size fixes (change 12 covers the class-library
case). If `bytecode-est` dominates at a healthy frame rate, the next step would
be interpreter-level work (e.g. promoting hot method bytecode from PSRAM to
internal RAM), which is a much larger change.

What the Assassin's Creed Brotherhood log (320x240, 7 FPS in-game) showed, as an
example of reading the line:

```
FPS 7.1 | present 6.6 | VM 140.5 | native 31.7 | bytecode-est 108.7
        | GC 0.00 | idle 0.0 | frame 140.5 | sleeps 12/frame, requested 180.0
```

Every millisecond of the frame is VM time (`frame == VM`, `idle 0`), and of
that 108.7 ms is interpreted bytecode and 31.7 ms native (drawing plus the
shortened sleeps).  No display, GC or sleep setting can help there: that title
is simply in the same class as the 3D racers, which is why the effort went into
the 2D path (change 13) and into making the native split visible (change 15).

## Reverting

* Bigger caches: delete the six cache lines from `sdkconfig.defaults` and rebuild
  (the build regenerates `sdkconfig`; no need to touch it by hand).  Cache sizes
  only affect this app — they are applied at app startup — so this cannot affect
  other apps.
* Tick rate: delete `CONFIG_FREERTOS_HZ=1000` from `sdkconfig.defaults` and
  rebuild.
* `-O3` -> `-O2` in `main/CMakeLists.txt`.
* Sleep shortening: set **Frame pacing** to *Balanced* or *Accurate* per game
  (no rebuild needed), or change `NJ_SLEEP_PCT` / `NJ_SLEEP_MIN_MS` in
  `thread.c`.
* Heap size: unchanged from v1.7.  `NJ_HEAP_MAX` in `nanojava_main.c` is the
  upper bound; the `2u << 20` reserve in the `largest` clamp is the PSRAM left
  to everything that is not the heap.
* Automatic exit after a dead thread: it is off now.  `pal_poll_event()` only
  logs the hint; changing `NJ_STALL_HINT_DUMPS` changes when that appears.
* The `StringBuffer` fix lives in `nanojava_classlib.jar`: restore the original
  jar (the one shipped in v1.7) and rebuild.  The source
  change in `main/nanojava/classlib/src/java/lang/StringBuffer.java` is
  self-contained (one new private method, four call sites).
* The `flushGraphics()` fast path is `javax/microedition/lcdui/Display.java` in
  the same jar: delete the early `if (menu == null && ...)` block in `flush()`
  to get the old unconditional copy back.
* Keypad layout: the `s_keymap[]` table in `pal_retrogo.c` is the whole
  mapping, one line per button.
* Diagonal number keys: **Keypad diagonals → Off** per game (or `diag_ms=0` in
  the cfg); `s_diagonals[]` in `pal_retrogo.c` holds the four pairs.
* `System.gc()` rate limit: delete the `if (now - last_ms >= 500 || ...)` guard
  in `Runtime_gc()` (`natives_lang.c`) to run a full collection on every call
  again.
* The cached epoch: `pal_epoch_ms()` in `pal_retrogo.c`.  Calling
  `gettimeofday()` per call is correct but cost 36 ms/frame in a game that polls
  the clock (§19), so do not revert this alone.
* Canvas list: `s_canvas_profiles[]` / `s_canvas_legacy_map[]` in
  `nanojava_main.c`.  Deleting the `cfg_ver` handling restores the old
  numbering at the cost of re-picking the canvas in every saved game.
* Input drain-before-paint: `Display.run()` / `dispatch()` in
  `javax/microedition/lcdui/Display.java` (rebuild the classlib jar).  The old
  one-event-per-paint loop is only correct for a slow game that is never busy;
  keep the new one.
* Event queue drop policy: `queue_put()` in `events.c`.
* Native accounting: the `NjNativeStat` block in `interp.c` and the second
  telemetry line in `pal_retrogo.c`; removing both cannot change behaviour.
* Java method accounting: the `NjJavaStat` block plus the wrapper around
  `interp_run_impl()` in `interp.c`, and the third telemetry line; like the
  native table it is bookkeeping only and can be deleted without touching VM
  semantics.
* The `CMakeLists.txt` change is what makes `sdkconfig.defaults` work at all;
  without it nothing in that file has any effect.
* Everything else is self-contained in `pal_retrogo.c`, `interp.c`, `heap.c`,
  `thread.c`, `vm.h`, `nanojava_main.c`.
* To go back to the untouched v1.7 app, restore the original sources and jar and
  rebuild: `./sdkconfig` and `./sdkconfig.defaults` are the only config files
  involved.
