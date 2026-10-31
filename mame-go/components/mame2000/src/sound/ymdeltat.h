#ifndef __YMDELTAT_H_
#define __YMDELTAT_H_

#define YM_DELTAT_SHIFT    (16)

/* adpcm type A and type B struct */
typedef struct deltat_adpcm_state {
	uint8_t *memory;
	int memory_size;
	float freqbase;
	int32_t *output_pointer; /* pointer of output pointers */
	int output_range;

	uint8_t reg[16];
	uint8_t portstate,portcontrol;
	int portshift;

	uint8_t flag;          /* port state        */
	uint8_t flagMask;      /* arrived flag mask */
	uint8_t now_data;
	uint32_t now_addr;
	uint32_t now_step;
	uint32_t step;
	uint32_t start;
	uint32_t end;
	uint32_t delta;
	int32_t volume;
	int32_t *pan;        /* &output_pointer[pan] */
	int32_t /*adpcmm,*/ adpcmx, adpcmd;
	int32_t adpcml;			/* hiro-shi!! */

	/* leveling and re-sampling state for DELTA-T */
	int32_t volume_w_step;   /* volume with step rate */
	int32_t next_leveling;   /* leveling value        */
	int32_t sample_step;     /* step of re-sampling   */

	uint8_t arrivedFlag;    /* flag of arrived end address */
}YM_DELTAT;

/* static state */
extern uint8_t *ym_deltat_memory;       /* memory pointer */

/* before YM_DELTAT_ADPCM_CALC(YM_DELTAT *DELTAT); */
#define YM_DELTAT_DECODE_PRESET(DELTAT) {ym_deltat_memory = DELTAT->memory;}

void YM_DELTAT_ADPCM_Write(YM_DELTAT *DELTAT,int r,int v);
void YM_DELTAT_ADPCM_Reset(YM_DELTAT *DELTAT,int pan);

/* INLINE void YM_DELTAT_ADPCM_CALC(YM_DELTAT *DELTAT); */
#define YM_INLINE_BLOCK
#include "ymdeltat.c" /* include inline function section */
#undef  YM_INLINE_BLOCK

#endif
