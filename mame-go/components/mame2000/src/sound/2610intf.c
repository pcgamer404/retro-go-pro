/***************************************************************************

  2610intf.c

  The YM2610 emulator supports up to 2 chips.
  Each chip has the following connections:
  - Status Read / Control Write A
  - Port Read / Data Write A
  - Control Write B
  - Data Write B

***************************************************************************/

#include "driver.h"
#include "ay8910.h"
#include "fm.h"

#if BUILD_YM2610

/* use FM.C with stream system */

static int stream[MAX_2610];

/* Global Interface holder */
static const struct YM2610interface *intf;

static void *Timer[MAX_2610][2];

/*------------------------- TM2610 -------------------------------*/
/* IRQ Handler */
static void IRQHandler(int n,int irq)
{
	if(intf->handler[n]) intf->handler[n](irq);
}

/* Timer overflow callback from timer.c */
static void timer_callback_2610(int param)
{
	int n=param&0x7f;
	int c=param>>7;

//	logerror("2610 TimerOver %d\n",c);
	Timer[n][c] = 0;
	YM2610TimerOver(n,c);
}

#ifdef MAMEGO
/* the Neo Geo sound board on core 1 (drivers/neogeo.c) counts the timers
   itself and renders the samples as its Z80 goes */
void (*neosnd_timer_hook)(int c, int count, timer_tm step);
void (*neosnd_render_hook)(void);
#endif

/* TimerHandler from fm.c */
static void TimerHandler(int n,int c,int count,timer_tm stepTime)
{
#ifdef MAMEGO
	if (neosnd_timer_hook)
	{
		neosnd_timer_hook(c, count, stepTime);
		return;
	}
#endif
	if( count == 0 )
	{	/* Reset FM Timer */
		if( Timer[n][c] )
		{
//			logerror("2610 TimerReset %d\n",c);
	 		timer_remove (Timer[n][c]);
			Timer[n][c] = 0;
		}
	}
	else
	{	/* Start FM Timer */
		timer_tm timeSec = (timer_tm)count * stepTime;

		if( Timer[n][c] == 0 )
		{
			Timer[n][c] = timer_set (timeSec , (c<<7)|n, timer_callback_2610 );
		}
	}
}

static void FMTimerInit( void )
{
	int i;

	for( i = 0 ; i < MAX_2610 ; i++ )
		Timer[i][0] = Timer[i][1] = 0;
}

#ifdef MAMEGO
/* mame-go save state (Neo Geo): YM2610 chip, its SSG and the two timer
 * handles (entries of timer.c's static array, restored by index there) */
size_t YM2610_state(unsigned char *buf, size_t size, int mode);
size_t AY8910_state(int chip, unsigned char *buf, size_t size, int mode);
size_t YM2610_mamego_state(unsigned char *buf, size_t size, int mode)
{
	size_t a = YM2610_state(NULL, 0, 0), b = AY8910_state(0, NULL, 0, 0);
	size_t len = sizeof(Timer[0]) + a + b;
	if (!mode)
		return len;
	if (size < len || !YM2610_state(buf, a, mode) || !AY8910_state(0, buf + a, b, mode))
		return 0;
	if (mode == 1) memcpy(buf + a + b, Timer[0], sizeof(Timer[0]));
	else memcpy(Timer[0], buf + a + b, sizeof(Timer[0]));
	return len;
}
#endif

/* update request from fm.c */
#ifdef MAMEGO
/* core 0 -> the sound board on core 1: MAME's timers dropped, the chip's
   running timers re-armed through the hook */
void YM2610_timers_to_host(void)
{
	extern void YM2610_rearm_timers(int n);
	extern void YM2610_offload(int on);
	int c;
	YM2610_offload(0); /* core 1 now writes the chip directly */
	for (c = 0; c < 2; c++)
		if (Timer[0][c])
		{
			timer_remove(Timer[0][c]);
			Timer[0][c] = 0;
		}
	YM2610_rearm_timers(0);
}
#endif

void YM2610UpdateRequest(int chip)
{
#ifdef MAMEGO
	if (neosnd_render_hook)
	{
		neosnd_render_hook();
		return;
	}
#endif
	stream_update(stream[chip],100);
}

int YM2610_sh_start(const struct MachineSound *msound)
{
	int i,j;
	int rate = Machine->sample_rate;
	char buf[YM2610_NUMBUF][40];
	const char *name[YM2610_NUMBUF];
	int mixed_vol,vol[YM2610_NUMBUF];
	void *pcmbufa[YM2610_NUMBUF],*pcmbufb[YM2610_NUMBUF];
	int  pcmsizea[YM2610_NUMBUF],pcmsizeb[YM2610_NUMBUF];

	intf = (const struct YM2610interface *)msound->sound_interface;
#ifdef MAMEGO
	{
		/* mame-go: synthesise the YM2610 once per frame instead of catching
		   it up on every register write (Neo Geo, Metal Slug 2 in play:
		   3-7 ms per frame spread over many tiny updates). Writes then take
		   effect at frame granularity, as with MAME's -fastsound. */
		extern int fast_sound;
		fast_sound = 1;
	}
#endif
	if( intf->num > MAX_2610 ) return 1;

	if (AY8910_sh_start(msound)) return 1;

	/* Timer Handler set */
	FMTimerInit();

	/* stream system initialize */
	for (i = 0;i < intf->num;i++)
	{
		/* stream setup */
		mixed_vol = intf->volumeFM[i];
		/* stream setup */
		for (j = 0 ; j < YM2610_NUMBUF ; j++)
		{
			name[j]=buf[j];
			vol[j] = mixed_vol & 0xffff;
			mixed_vol>>=16;
			sprintf(buf[j],"%s #%d Ch%d",sound_name(msound),i,j+1);
		}
		stream[i] = stream_init_multi(YM2610_NUMBUF,name,vol,rate,i,YM2610UpdateOne);
		/* setup adpcm buffers */
		pcmbufa[i]  = (void *)(memory_region(intf->pcmroma[i]));
		pcmsizea[i] = memory_region_length(intf->pcmroma[i]);
		pcmbufb[i]  = (void *)(memory_region(intf->pcmromb[i]));
		pcmsizeb[i] = memory_region_length(intf->pcmromb[i]);
	}

	/**** initialize YM2610 ****/
	if (YM2610Init(intf->num,intf->baseclock,rate,
		           pcmbufa,pcmsizea,pcmbufb,pcmsizeb,
		           TimerHandler,IRQHandler) == 0)
	{
#ifdef MAMEGO
		/* synthesis on the second core from here on (fm.c) */
		extern void YM2610_offload(int on);
#ifndef ESP_PLATFORM
		if (!getenv("YMOFFLOAD") || strcmp(getenv("YMOFFLOAD"), "0"))
#endif
		YM2610_offload(1);
#endif
		return 0;
	}

	/* error */
	return 1;
}

#if BUILD_YM2610B
int YM2610B_sh_start(const struct MachineSound *msound)
{
	int i,j;
	int rate = Machine->sample_rate;
	char buf[YM2610_NUMBUF][40];
	const char *name[YM2610_NUMBUF];
	int mixed_vol,vol[YM2610_NUMBUF];
	void *pcmbufa[YM2610_NUMBUF],*pcmbufb[YM2610_NUMBUF];
	int  pcmsizea[YM2610_NUMBUF],pcmsizeb[YM2610_NUMBUF];

	intf = (const struct YM2610interface *)msound->sound_interface;
	if( intf->num > MAX_2610 ) return 1;

	if (AY8910_sh_start(msound)) return 1;

	/* Timer Handler set */
	FMTimerInit();

	/* stream system initialize */
	for (i = 0;i < intf->num;i++)
	{
		/* stream setup */
		mixed_vol = intf->volumeFM[i];
		/* stream setup */
		for (j = 0 ; j < YM2610_NUMBUF ; j++)
		{
			name[j]=buf[j];
			vol[j] = mixed_vol & 0xffff;
			mixed_vol>>=16;
			sprintf(buf[j],"%s #%d Ch%d",sound_name(msound),i,j+1);
		}
		stream[i] = stream_init_multi(YM2610_NUMBUF,name,vol,rate,i,YM2610BUpdateOne);
		/* setup adpcm buffers */
		pcmbufa[i]  = (void *)(memory_region(intf->pcmroma[i]));
		pcmsizea[i] = memory_region_length(intf->pcmroma[i]);
		pcmbufb[i]  = (void *)(memory_region(intf->pcmromb[i]));
		pcmsizeb[i] = memory_region_length(intf->pcmromb[i]);
	}

	/**** initialize YM2610 ****/
	if (YM2610Init(intf->num,intf->baseclock,rate,
		           pcmbufa,pcmsizea,pcmbufb,pcmsizeb,
		           TimerHandler,IRQHandler) == 0)
		return 0;

	/* error */
	return 1;
}
#endif

/************************************************/
/* Sound Hardware Stop							*/
/************************************************/
void YM2610_sh_stop(void)
{
#ifdef MAMEGO
	{ extern void YM2610_offload(int on); YM2610_offload(0); } /* core 1 idle before the chip goes */
#endif
	YM2610Shutdown();
#ifdef MAMEGO
	{ extern int fast_sound; fast_sound = 0; } /* back to the default for the next game */
#endif
}

/* reset */
void YM2610_sh_reset(void)
{
	int i;

	for (i = 0;i < intf->num;i++)
		YM2610ResetChip(i);
}

/************************************************/
/* Status Read for YM2610 - Chip 0				*/
/************************************************/
READ_HANDLER( YM2610_status_port_0_A_r )
{
//logerror("PC %04x: 2610 S0A=%02X\n",cpu_get_pc(),YM2610Read(0,0));
	return YM2610Read(0,0);
}

READ_HANDLER( YM2610_status_port_0_B_r )
{
//logerror("PC %04x: 2610 S0B=%02X\n",cpu_get_pc(),YM2610Read(0,2));
	return YM2610Read(0,2);
}

/************************************************/
/* Status Read for YM2610 - Chip 1				*/
/************************************************/
READ_HANDLER( YM2610_status_port_1_A_r ) {
	return YM2610Read(1,0);
}

READ_HANDLER( YM2610_status_port_1_B_r ) {
	return YM2610Read(1,2);
}

/************************************************/
/* Port Read for YM2610 - Chip 0				*/
/************************************************/
READ_HANDLER( YM2610_read_port_0_r ){
	return YM2610Read(0,1);
}

/************************************************/
/* Port Read for YM2610 - Chip 1				*/
/************************************************/
READ_HANDLER( YM2610_read_port_1_r ){
	return YM2610Read(1,1);
}

/************************************************/
/* Control Write for YM2610 - Chip 0			*/
/* Consists of 2 addresses						*/
/************************************************/
WRITE_HANDLER( YM2610_control_port_0_A_w )
{
//logerror("PC %04x: 2610 Reg A %02X",cpu_get_pc(),data);
	YM2610Write(0,0,data);
}

WRITE_HANDLER( YM2610_control_port_0_B_w )
{
//logerror("PC %04x: 2610 Reg B %02X",cpu_get_pc(),data);
	YM2610Write(0,2,data);
}

/************************************************/
/* Control Write for YM2610 - Chip 1			*/
/* Consists of 2 addresses						*/
/************************************************/
WRITE_HANDLER( YM2610_control_port_1_A_w ){
	YM2610Write(1,0,data);
}

WRITE_HANDLER( YM2610_control_port_1_B_w ){
	YM2610Write(1,2,data);
}

/************************************************/
/* Data Write for YM2610 - Chip 0				*/
/* Consists of 2 addresses						*/
/************************************************/
WRITE_HANDLER( YM2610_data_port_0_A_w )
{
//logerror(" =%02X\n",data);
	YM2610Write(0,1,data);
}

WRITE_HANDLER( YM2610_data_port_0_B_w )
{
//logerror(" =%02X\n",data);
	YM2610Write(0,3,data);
}

/************************************************/
/* Data Write for YM2610 - Chip 1				*/
/* Consists of 2 addresses						*/
/************************************************/
WRITE_HANDLER( YM2610_data_port_1_A_w ){
	YM2610Write(1,1,data);
}
WRITE_HANDLER( YM2610_data_port_1_B_w ){
	YM2610Write(1,3,data);
}

/**************** end of file ****************/

#endif
