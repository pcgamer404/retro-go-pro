/* This file is part of Snes9x. See LICENSE file. */

#ifndef USE_BLARGG_APU

#include "snes9x.h"
#include "spc700.h"
#include "apu.h"
#include "soundux.h"
#include "cpuexec.h"

static bool dsp_offload; /* S-DSP on core 1, see below */

extern const int32_t NoiseFreq[32];

bool S9xInitAPU()
{
   IAPU.RAM = (uint8_t*) malloc(0x10000);

   if (!IAPU.RAM)
   {
      S9xDeinitAPU();
      return false;
   }

   return true;
}

void S9xDeinitAPU()
{
   if (IAPU.RAM)
   {
      free(IAPU.RAM);
      IAPU.RAM = NULL;
   }
}

void S9xResetAPU()
{
   bool offload = S9xAudioOffloaded();
   if (offload)
      S9xAudioSync();
   dsp_offload = false;
   int32_t i, j;
   Settings.APUEnabled = true;
   memset(IAPU.RAM, 0, 0x100);
   memset(IAPU.RAM + 0x20, 0xFF, 0x20);
   memset(IAPU.RAM + 0x60, 0xFF, 0x20);
   memset(IAPU.RAM + 0xA0, 0xFF, 0x20);
   memset(IAPU.RAM + 0xE0, 0xFF, 0x20);

   for (i = 1; i < 256; i++)
      memcpy(IAPU.RAM + (i << 8), IAPU.RAM, 0x100);

   memset(APU.OutPorts, 0, sizeof(APU.OutPorts));
   IAPU.DirectPage = IAPU.RAM;
   /* memmove converted: Different mallocs [Neb]
    * DS2 DMA notes: The APU ROM is not 32-byte aligned [Neb] */
   memcpy(&IAPU.RAM [0xffc0], APUROM, sizeof(APUROM));
   /* memmove converted: Different mallocs [Neb]
    * DS2 DMA notes: The APU ROM is not 32-byte aligned [Neb] */
   memcpy(APU.ExtraRAM, APUROM, sizeof(APUROM));
   IAPU.PC = IAPU.RAM + IAPU.RAM [0xfffe] + (IAPU.RAM [0xffff] << 8);
   APU.Cycles = 0;
   IAPU.Registers.YA.W = 0;
   IAPU.Registers.X = 0;
   IAPU.Registers.S = 0xef;
   IAPU.Registers.P = 0x02;
   S9xAPUUnpackStatus();
   IAPU.Registers.PC = 0;
   IAPU.APUExecuting = Settings.APUEnabled;
   IAPU.WaitAddress1 = NULL;
   IAPU.WaitAddress2 = NULL;
   IAPU.WaitCounter = 1;
   APU.ShowROM = true;
   IAPU.RAM [0xf1] = 0x80;

   for (i = 0; i < 3; i++)
   {
      APU.TimerEnabled [i] = false;
      APU.TimerTarget [i] = 0;
      APU.Timer [i] = 0;
   }
   for (j = 0; j < 0x80; j++)
      APU.DSP [j] = 0;

   IAPU.TwoCycles = IAPU.OneCycle * 2;

   for (i = 0; i < 256; i++)
      S9xAPUCycles [i] = S9xAPUCycleLengths [i] * IAPU.OneCycle;

   APU.DSP [APU_ENDX] = 0;
   APU.DSP [APU_KOFF] = 0;
   APU.DSP [APU_KON] = 0;
   APU.DSP[APU_FLG] = APU_SOFT_RESET | APU_MUTE;
   APU.KeyedChannels = 0;

   S9xResetSound(true);
   S9xSetEchoEnable(0);
   S9xAudioSync();
   dsp_offload = offload;
}

uint8_t S9xAPUReadPort(int32_t Address)
{
   IAPU.APUExecuting = Settings.APUEnabled;
   IAPU.WaitCounter++;

   if (Settings.APUEnabled)
      return APU.OutPorts [Address & 3];

   CPU.BranchSkip = true;

   if ((Address & 3) < 2)
   {
      int32_t r = rand();
      if (r & 2)
      {
         if (r & 4)
            return (Address & 3) == 1 ? 0xaa : 0xbb;
         else
            return (r >> 3) & 0xff;
      }
   }
   else
   {
      int32_t r = rand();
      if (r & 2)
         return (r >> 3) & 0xff;
   }
   return Memory.FillRAM[Address];
}

void S9xAPUWritePort(int32_t Address, uint8_t Byte)
{
   Memory.FillRAM [Address] = Byte;
   IAPU.RAM [(Address & 3) + 0xf4] = Byte;
   IAPU.APUExecuting = Settings.APUEnabled;
   IAPU.WaitCounter++;
}

static void dsp_apply(uint8_t reg, uint8_t byte)
{
   static uint8_t KeyOn;
   static uint8_t KeyOnPrev;
   int32_t i;

   switch (reg)
   {
   case APU_FLG:
      if (byte & APU_SOFT_RESET)
      {
         APU.DSP [reg] = APU_MUTE | APU_ECHO_DISABLED | (byte & 0x1f);
         APU.DSP [APU_ENDX] = 0;
         APU.DSP [APU_KOFF] = 0;
         APU.DSP [APU_KON] = 0;
         S9xSetEchoWriteEnable(false);

         /* Kill sound */
         S9xResetSound(false);
      }
      else
      {
         S9xSetEchoWriteEnable(!(byte & APU_ECHO_DISABLED));
         so.mute_sound = !!(byte & APU_MUTE);
         SoundData.noise_hertz = NoiseFreq [byte & 0x1f];
         for (i = 0; i < 8; i++)
            if (SoundData.channels [i].type == SOUND_NOISE)
               S9xSetSoundFrequency(i, SoundData.noise_hertz);
      }
      break;
   case APU_NON:
      if (byte != APU.DSP [APU_NON])
      {
         int32_t c;
         uint8_t mask = 1;
         for (c = 0; c < 8; c++, mask <<= 1)
         {
            int32_t type;

            if (byte & mask)
               type = SOUND_NOISE;
            else
               type = SOUND_SAMPLE;

            S9xSetSoundType(c, type);
         }
      }
      break;
   case APU_MVOL_LEFT:
      if (byte != APU.DSP [APU_MVOL_LEFT])
         S9xSetMasterVolume((int8_t) byte, (int8_t) APU.DSP [APU_MVOL_RIGHT]);
      break;
   case APU_MVOL_RIGHT:
      if (byte != APU.DSP [APU_MVOL_RIGHT])
         S9xSetMasterVolume((int8_t) APU.DSP [APU_MVOL_LEFT], (int8_t) byte);
      break;
   case APU_EVOL_LEFT:
      if (byte != APU.DSP [APU_EVOL_LEFT])
         S9xSetEchoVolume((int8_t) byte, (int8_t) APU.DSP [APU_EVOL_RIGHT]);
      break;
   case APU_EVOL_RIGHT:
      if (byte != APU.DSP [APU_EVOL_RIGHT])
         S9xSetEchoVolume((int8_t) APU.DSP [APU_EVOL_LEFT], (int8_t) byte);
      break;
   case APU_ENDX:
      byte = 0;
      break;
   case APU_KOFF:
   {
      int32_t c;
      uint8_t mask = 1;
      for (c = 0; c < 8; c++, mask <<= 1)
      {
         if ((byte & mask) != 0)
         {
            if (APU.KeyedChannels & mask)
            {
               KeyOnPrev &= ~mask;
               APU.KeyedChannels &= ~mask;
               APU.DSP [APU_KON] &= ~mask;
               S9xSetSoundKeyOff(c);
            }
         }
         else if ((KeyOnPrev & mask) != 0)
         {
            KeyOnPrev &= ~mask;
            APU.KeyedChannels |= mask;
            APU.DSP [APU_KOFF] &= ~mask;
            APU.DSP [APU_ENDX] &= ~mask;
            S9xPlaySample(c);
         }
      }

      APU.DSP [APU_KOFF] = byte;
      return;
   }
   case APU_KON:
      if (byte)
      {
         int32_t c;
         uint8_t mask = 1;
         for (c = 0; c < 8; c++, mask <<= 1)
         {
            if ((byte & mask) != 0)
            {
               /* Pac-In-Time requires that channels can be key-on
                * regardeless of their current state. */
               if ((APU.DSP [APU_KOFF] & mask) == 0)
               {
                  KeyOnPrev &= ~mask;
                  APU.KeyedChannels |= mask;
                  APU.DSP [APU_ENDX] &= ~mask;
                  S9xPlaySample(c);
               }
               else
                  KeyOn |= mask;
            }
         }
      }
      return;
   case APU_VOL_LEFT + 0x00:
   case APU_VOL_LEFT + 0x10:
   case APU_VOL_LEFT + 0x20:
   case APU_VOL_LEFT + 0x30:
   case APU_VOL_LEFT + 0x40:
   case APU_VOL_LEFT + 0x50:
   case APU_VOL_LEFT + 0x60:
   case APU_VOL_LEFT + 0x70:
      S9xSetSoundVolume(reg >> 4, (int8_t) byte, (int8_t) APU.DSP [reg + 1]);
      break;
   case APU_VOL_RIGHT + 0x00:
   case APU_VOL_RIGHT + 0x10:
   case APU_VOL_RIGHT + 0x20:
   case APU_VOL_RIGHT + 0x30:
   case APU_VOL_RIGHT + 0x40:
   case APU_VOL_RIGHT + 0x50:
   case APU_VOL_RIGHT + 0x60:
   case APU_VOL_RIGHT + 0x70:
      S9xSetSoundVolume(reg >> 4, (int8_t) APU.DSP [reg - 1], (int8_t) byte);
      break;
   case APU_P_LOW + 0x00:
   case APU_P_LOW + 0x10:
   case APU_P_LOW + 0x20:
   case APU_P_LOW + 0x30:
   case APU_P_LOW + 0x40:
   case APU_P_LOW + 0x50:
   case APU_P_LOW + 0x60:
   case APU_P_LOW + 0x70:
      S9xSetSoundHertz(reg >> 4, (((int16_t) byte + ((int16_t) APU.DSP [reg + 1] << 8)) & FREQUENCY_MASK) * 8);
      break;
   case APU_P_HIGH + 0x00:
   case APU_P_HIGH + 0x10:
   case APU_P_HIGH + 0x20:
   case APU_P_HIGH + 0x30:
   case APU_P_HIGH + 0x40:
   case APU_P_HIGH + 0x50:
   case APU_P_HIGH + 0x60:
   case APU_P_HIGH + 0x70:
      S9xSetSoundHertz(reg >> 4, ((((int16_t) byte << 8) + (int16_t) APU.DSP [reg - 1]) & FREQUENCY_MASK) * 8);
      break;
   case APU_ADSR1 + 0x00:
   case APU_ADSR1 + 0x10:
   case APU_ADSR1 + 0x20:
   case APU_ADSR1 + 0x30:
   case APU_ADSR1 + 0x40:
   case APU_ADSR1 + 0x50:
   case APU_ADSR1 + 0x60:
   case APU_ADSR1 + 0x70:
      if(byte != APU.DSP [reg])
         S9xFixEnvelope(reg >> 4, APU.DSP [reg + 2], byte, APU.DSP [reg + 1]);
      break;
   case APU_ADSR2 + 0x00:
   case APU_ADSR2 + 0x10:
   case APU_ADSR2 + 0x20:
   case APU_ADSR2 + 0x30:
   case APU_ADSR2 + 0x40:
   case APU_ADSR2 + 0x50:
   case APU_ADSR2 + 0x60:
   case APU_ADSR2 + 0x70:
      if(byte != APU.DSP [reg])
         S9xFixEnvelope(reg >> 4, APU.DSP [reg + 1], APU.DSP [reg - 1], byte);
      break;
   case APU_GAIN + 0x00:
   case APU_GAIN + 0x10:
   case APU_GAIN + 0x20:
   case APU_GAIN + 0x30:
   case APU_GAIN + 0x40:
   case APU_GAIN + 0x50:
   case APU_GAIN + 0x60:
   case APU_GAIN + 0x70:
      if(byte != APU.DSP [reg])
         S9xFixEnvelope(reg >> 4, byte, APU.DSP [reg - 2], APU.DSP [reg - 1]);
      break;
   case APU_PMON:
      if(byte != APU.DSP [APU_PMON])
         S9xSetFrequencyModulationEnable(byte);
      break;
   case APU_EON:
      if(byte != APU.DSP [APU_EON])
         S9xSetEchoEnable(byte);
      break;
   case APU_EFB:
      S9xSetEchoFeedback((int8_t) byte);
      break;
   case APU_EDL:
      S9xSetEchoDelay(byte & 0xf);
      break;
   case APU_C0:
   case APU_C1:
   case APU_C2:
   case APU_C3:
   case APU_C4:
   case APU_C5:
   case APU_C6:
   case APU_C7:
      S9xSetFilterCoefficient(reg >> 4, (int8_t) byte);
      break;
   default:
      break;
   }

   KeyOnPrev |= KeyOn;
   KeyOn = 0;

   if (reg < 0x80)
      APU.DSP [reg] = byte;
}

void S9xFixEnvelope(int32_t channel, uint8_t gain, uint8_t adsr1, uint8_t adsr2)
{
   if (adsr1 & 0x80) /* ADSR mode */
   {
      /* XXX: can DSP be switched to ADSR mode directly from GAIN/INCREASE/
       * DECREASE mode? And if so, what stage of the sequence does it start
       * at? */
      if(S9xSetSoundMode(channel, MODE_ADSR))
         S9xSetSoundADSR(channel, adsr1 & 0xf, (adsr1 >> 4) & 7, adsr2 & 0x1f, (adsr2 >> 5) & 7, 8);
   } /* Gain mode */
   else if (!(gain & 0x80))
   {
      if (S9xSetSoundMode(channel, MODE_GAIN))
      {
         S9xSetEnvelopeRate(channel, 0, 0, gain & 0x7f, 0);
         S9xSetEnvelopeHeight(channel, gain & 0x7f);
      }
   }
   else if (gain & 0x40)
   {
      /* Increase mode */
      if(S9xSetSoundMode(channel, (gain & 0x20) ? MODE_INCREASE_BENT_LINE : MODE_INCREASE_LINEAR))
         S9xSetEnvelopeRate(channel, gain, 1, 127, (3 << 28) | gain);
   }
   else if (gain & 0x20)
   {
      if(S9xSetSoundMode(channel, MODE_DECREASE_EXPONENTIAL))
         S9xSetEnvelopeRate(channel, gain, -1, 0, (4 << 28) | gain);
   }
   else
   {
      if (S9xSetSoundMode(channel, MODE_DECREASE_LINEAR))
         S9xSetEnvelopeRate(channel, gain, -1, 0, (3 << 28) | gain);
   }
}

void S9xSetAPUControl(uint8_t byte)
{
   if ((byte & 1) && !APU.TimerEnabled [0])
   {
      APU.Timer [0] = 0;
      IAPU.RAM [0xfd] = 0;
      if ((APU.TimerTarget [0] = IAPU.RAM [0xfa]) == 0)
         APU.TimerTarget [0] = 0x100;
   }
   if ((byte & 2) && !APU.TimerEnabled [1])
   {
      APU.Timer [1] = 0;
      IAPU.RAM [0xfe] = 0;
      if ((APU.TimerTarget [1] = IAPU.RAM [0xfb]) == 0)
         APU.TimerTarget [1] = 0x100;
   }
   if ((byte & 4) && !APU.TimerEnabled [2])
   {
      APU.Timer [2] = 0;
      IAPU.RAM [0xff] = 0;
      if ((APU.TimerTarget [2] = IAPU.RAM [0xfc]) == 0)
         APU.TimerTarget [2] = 0x100;
   }
   APU.TimerEnabled [0] = !!(byte & 1);
   APU.TimerEnabled [1] = !!(byte & 2);
   APU.TimerEnabled [2] = !!(byte & 4);

   if (byte & 0x10)
      IAPU.RAM [0xF4] = IAPU.RAM [0xF5] = 0;

   if (byte & 0x20)
      IAPU.RAM [0xF6] = IAPU.RAM [0xF7] = 0;

   if (byte & 0x80)
   {
      if (!APU.ShowROM)
      {
         /* memmove converted: Different mallocs [Neb]
          * DS2 DMA notes: The APU ROM is not 32-byte aligned [Neb] */
         memcpy(&IAPU.RAM [0xffc0], APUROM, sizeof(APUROM));
         APU.ShowROM = true;
      }
   }
   else if (APU.ShowROM)
   {
      APU.ShowROM = false;
      /* memmove converted: Different mallocs [Neb]
       * DS2 DMA notes: The APU ROM is not 32-byte aligned [Neb] */
      memcpy(&IAPU.RAM [0xffc0], APU.ExtraRAM, sizeof(APUROM));
   }
   IAPU.RAM [0xf1] = byte;
}

/* ----------------------------------------------------------------------
 * S-DSP on core 1 (esp32-emu-turbo, roadmap Phase 5).
 *
 * The SPC700 has to stay on core 0, in lockstep with the 65816 (they talk
 * through four ports every few cycles). The S-DSP's sample generation does
 * not: this port already mixes a whole frame at once at the frame end, from
 * the channel state the frame's register writes left. So the frame's DSP
 * register writes are queued, and core 1 replays them in order and mixes
 * the frame while core 0 emulates the next one (same model as the Genesis
 * YM2612 and the Neo Geo YM2610). Audio comes out one frame (~17 ms) later.
 *
 * What the SPC700 reads back: the registers from an image kept on core 0
 * (updated on every write the way the handler would store it), ENDX/KON/KOFF
 * bits the mixer changes when a sample ends (recorded as deltas by
 * soundux.c and applied at the frame boundary), and OUTX/ENVX from a
 * snapshot taken there. Inline, those came from the end of the previous
 * frame's mix as well, so readback timing is unchanged.
 * ---------------------------------------------------------------------- */
uint8_t S9xDSPMixEndX, S9xDSPMixKeyClr;    /* soundux.c: channels the mixer ended */

typedef struct { uint8_t reg, val; } dsp_write_t;
#define DSPQ_MAX 8192
static dsp_write_t *dspq[2];
static int dspq_n[2], dspq_fill, dspq_overflow;
static uint8_t dsp_image[0x80], outx_snap[8], envx_snap[8];
static int16_t *mixbuf[2];
static int mix_job_q, mix_job_count, mix_job_lowpass, mix_job_range, mix_out, mix_ready;

static void dsp_publish(void)
{
   int c;
   dsp_image[APU_ENDX] |= S9xDSPMixEndX;
   dsp_image[APU_KON] &= ~S9xDSPMixKeyClr;
   dsp_image[APU_KOFF] &= ~S9xDSPMixKeyClr;
   S9xDSPMixEndX = S9xDSPMixKeyClr = 0;
   for (c = 0; c < 8; c++)
   {
      Channel *ch = &SoundData.channels[c];
      int32_t e = ch->envx;
      outx_snap[c] = ch->state == SOUND_SILENT ? 0 : ((ch->sample >> 8) | (ch->sample & 0xff));
      envx_snap[c] = e > 0x7f ? 0x7f : (e < 0 ? 0 : e);
   }
}

/* core 1: the frame's register writes in order, then its samples */
static void mix_job(void)
{
   int i, q = mix_job_q;
   for (i = 0; i < dspq_n[q]; i++)
      dsp_apply(dspq[q][i].reg, dspq[q][i].val);
   dspq_n[q] = 0;
   if (mix_job_lowpass)
      S9xMixSamplesLowPass(mixbuf[mix_out], mix_job_count << 1, mix_job_range);
   else
      S9xMixSamples(mixbuf[mix_out], mix_job_count << 1);
}

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
static TaskHandle_t mix_task;
static SemaphoreHandle_t mix_done;
static volatile bool mix_busy;
static void mix_task_main(void *arg)
{
   for (;;)
   {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      mix_job();
      __sync_synchronize();
      mix_busy = false;
      xSemaphoreGive(mix_done);
   }
}
static void mix_wait(void)
{
   if (mix_busy)
      xSemaphoreTake(mix_done, portMAX_DELAY);
}
static void mix_start(void)
{
   mix_busy = true;
   __sync_synchronize();
   xTaskNotifyGive(mix_task);
}
static bool mix_task_create(void)
{
   if (!mix_task)
   {
      mix_done = xSemaphoreCreateBinary();
      if (!mix_done || xTaskCreatePinnedToCore(mix_task_main, "snes_dsp", 4096, NULL, 5, &mix_task, 1) != pdPASS)
         return false;
   }
   return true;
}
#else
static void mix_wait(void) {}
static void mix_start(void) { mix_job(); } /* PC: runs at once, same order, deterministic */
static bool mix_task_create(void) { return true; }
#endif

/* Everything applied and mixed, image = real registers (save/load, reset,
   switching the offload): nothing may run on core 1 after this. */
void S9xAudioSync(void)
{
   int q, i;
   mix_wait();
   for (q = 0; q < 2; q++)
   {
      for (i = 0; dspq[q] && i < dspq_n[q]; i++)
         dsp_apply(dspq[q][i].reg, dspq[q][i].val);
      dspq_n[q] = 0;
   }
   S9xDSPMixEndX = S9xDSPMixKeyClr = 0;
   memcpy(dsp_image, APU.DSP, sizeof(dsp_image));
   dsp_publish();
   mix_ready = 0;
}

bool S9xAudioOffload(bool on)
{
   S9xAudioSync();
   if (on && !dspq[0])
   {
      dspq[0] = malloc(DSPQ_MAX * sizeof(dsp_write_t));
      dspq[1] = malloc(DSPQ_MAX * sizeof(dsp_write_t));
      mixbuf[0] = malloc(2048 * sizeof(int16_t));
      mixbuf[1] = malloc(2048 * sizeof(int16_t));
      if (!dspq[0] || !dspq[1] || !mixbuf[0] || !mixbuf[1] || !mix_task_create())
         on = false;
   }
   dsp_offload = on;
   return on;
}

bool S9xAudioOffloaded(void)
{
   return dsp_offload;
}

/* Frame end, core 0: collects the last frame's samples (NULL on the first
   frame after a sync) and starts mixing this one on core 1. */
int16_t *S9xAudioFrame(int32_t sample_count, bool low_pass, int32_t low_pass_range)
{
   int16_t *out = NULL;
   if (sample_count > 1024)
      sample_count = 1024;
   mix_wait();
   dsp_publish();
   if (mix_ready)
      out = mixbuf[mix_out];
   mix_out ^= 1;
   mix_job_q = dspq_fill;
   dspq_fill ^= 1;
   dspq_n[dspq_fill] = 0;
   mix_job_count = sample_count;
   mix_job_lowpass = low_pass;
   mix_job_range = low_pass_range;
   mix_ready = 1;
   mix_start();
   return out;
}

void S9xSetAPUDSP(uint8_t byte)
{
   uint8_t reg = IAPU.RAM [0xf2];
   if (!dsp_offload)
   {
      dsp_apply(reg, byte);
      return;
   }
   if (dspq_n[dspq_fill] < DSPQ_MAX)
   {
      dspq[dspq_fill][dspq_n[dspq_fill]].reg = reg;
      dspq[dspq_fill][dspq_n[dspq_fill]].val = byte;
      dspq_n[dspq_fill]++;
   }
   else
      dspq_overflow++;
   /* the image, as dsp_apply() would leave the register */
   if (reg >= 0x80)
      return;
   switch (reg)
   {
   case APU_KON:
      dsp_image[APU_ENDX] &= ~(byte & ~dsp_image[APU_KOFF]);
      return;                       /* KON is not stored on a write */
   case APU_ENDX:
      byte = 0;
      break;
   case APU_FLG:
      if (byte & APU_SOFT_RESET)
      {
         dsp_image[APU_ENDX] = dsp_image[APU_KOFF] = dsp_image[APU_KON] = 0;
         byte = APU_MUTE | APU_ECHO_DISABLED | (byte & 0x1f);
      }
      break;
   default:
      break;
   }
   dsp_image[reg] = byte;
}

uint8_t S9xGetAPUDSP()
{
   uint8_t reg = IAPU.RAM [0xf2] & 0x7f;
   uint8_t byte = APU.DSP [reg];

   if (dsp_offload)
   {
      if ((reg & 0x0f) == APU_OUTX)
         return outx_snap[reg >> 4];
      if ((reg & 0x0f) == APU_ENVX)
         return envx_snap[reg >> 4];
      return dsp_image[reg];
   }

   switch (reg)
   {
   case APU_OUTX + 0x00:
   case APU_OUTX + 0x10:
   case APU_OUTX + 0x20:
   case APU_OUTX + 0x30:
   case APU_OUTX + 0x40:
   case APU_OUTX + 0x50:
   case APU_OUTX + 0x60:
   case APU_OUTX + 0x70:
      if(SoundData.channels [reg >> 4].state == SOUND_SILENT)
         return 0;
      return (SoundData.channels [reg >> 4].sample >> 8) | (SoundData.channels [reg >> 4].sample & 0xff);
   case APU_ENVX + 0x00:
   case APU_ENVX + 0x10:
   case APU_ENVX + 0x20:
   case APU_ENVX + 0x30:
   case APU_ENVX + 0x40:
   case APU_ENVX + 0x50:
   case APU_ENVX + 0x60:
   case APU_ENVX + 0x70:
   {
      int32_t eVal = SoundData.channels [reg >> 4].envx;
      return (eVal > 0x7F) ? 0x7F : (eVal < 0 ? 0 : eVal);
   }
   default:
      break;
   }
   return byte;
}

#endif
