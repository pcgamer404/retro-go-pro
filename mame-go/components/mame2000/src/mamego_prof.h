/* mame-go frame profiler (build with -DNEOPROF): exclusive time per part of
 * the emulation, printed as ms per frame every 60 frames. A part entered
 * inside another (the YM2610 synthesised while the Z80 writes a register)
 * is not counted twice: the outer part pauses while the inner one runs. */
#ifndef MAMEGO_PROF_H
#define MAMEGO_PROF_H
#ifdef NEOPROF
enum { PROF_OTHER, PROF_CPU0, PROF_CPU1, PROF_YM, PROF_VIDEO, PROF_MIXER, PROF_BLIT, PROF_OUT, PROF_COUNT };
void mamego_prof_push(int part);
void mamego_prof_pop(void);
void mamego_prof_frame(void);
#define PROF_PUSH(p) mamego_prof_push(p)
#define PROF_POP()   mamego_prof_pop()
#define PROF_FRAME() mamego_prof_frame()
#else
#define PROF_PUSH(p) do {} while (0)
#define PROF_POP()   do {} while (0)
#define PROF_FRAME() do {} while (0)
#endif
#endif
