/* mame-go: a second, private Z80 for the Neo Geo sound board on core 1.
 *
 * MAME 0.37's CPUs share the memory system's globals (the active CPU's
 * memory map, OP_ROM...), so the sound Z80 cannot run beside the 68000
 * through them. This builds z80.c a second time with its exported names
 * prefixed (z80snd_*) and its memory, opcode and port accesses sent to the
 * Neo Geo sound board's handlers (drivers/neogeo.c, neosnd_z80_*): ROM with
 * four switchable banks, 2 KB of RAM, the YM2610, the command latch and the
 * reply register. Nothing here touches MAME's scheduler or memory tables. */
#include "driver.h"
#include "cpuintrf.h"
#include "state.h"

#ifdef MAMEGO
/* the sound board's memory and ports, set by the driver that uses this Z80
   (Neo Geo: drivers/neogeo.c, CPS1: drivers/cps1.c) */
unsigned (*sndz80_rm)(unsigned addr);
void (*sndz80_wm)(unsigned addr, unsigned value);
unsigned (*sndz80_in)(unsigned port);
void (*sndz80_out)(unsigned port, unsigned value);

#undef cpu_readop
#undef cpu_readop_arg
#undef change_pc
#undef change_pc16
#define cpu_readmem16(a)       sndz80_rm(a)
#define cpu_writemem16(a, v)   sndz80_wm(a, v)
#define cpu_readop(a)          sndz80_rm(a)
#define cpu_readop_arg(a)      sndz80_rm(a)
#define cpu_readport(p)        sndz80_in(p)
#define cpu_writeport(p, v)    sndz80_out(p, v)
#define change_pc(pc)          do {} while (0)
#define change_pc16(pc)        do {} while (0)

/* every name z80.c exports, prefixed */
#define z80_burn               z80snd_burn
#define z80_dasm               z80snd_dasm
#define z80_execute            z80snd_execute
#define z80_exit               z80snd_exit
#define z80_get_context        z80snd_get_context
#define z80_get_cycle_table    z80snd_get_cycle_table
#define z80_get_pc             z80snd_get_pc
#define z80_get_reg            z80snd_get_reg
#define z80_get_sp             z80snd_get_sp
#define z80_ICount             z80snd_ICount
#define z80_idle_enable        z80snd_idle_enable
#define z80_info               z80snd_info
#define z80_reset              z80snd_reset
#define z80_set_context        z80snd_set_context
#define z80_set_cycle_table    z80snd_set_cycle_table
#define z80_set_irq_callback   z80snd_set_irq_callback
#define z80_set_irq_line       z80snd_set_irq_line
#define z80_set_nmi_line       z80snd_set_nmi_line
#define z80_set_pc             z80snd_set_pc
#define z80_set_reg            z80snd_set_reg
#define z80_set_sp             z80snd_set_sp
#define z80_state_load         z80snd_state_load
#define z80_state_save         z80snd_state_save
#define z80_pchist             z80snd_pchist

#define Z80SND_INSTANCE
#include "z80.c"
#endif
