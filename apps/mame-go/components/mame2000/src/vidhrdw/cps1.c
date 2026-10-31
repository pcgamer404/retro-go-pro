/***************************************************************************

  vidhrdw.c

  Functions to emulate the video hardware of the machine.

  Todo:
  7 unknown attribute bits on scroll 2/3.
  Speed, speed, speed...

  OUTPUT PORTS (preliminary)
  0x00-0x01     OBJ RAM base (/256)
  0x02-0x03     Scroll1 RAM base (/256)
  0x04-0x05     Scroll2 RAM base (/256)
  0x06-0x07     Scroll3 RAM base (/256)
  0x08-0x09     "Other" RAM - Scroll distortion (/256)
  0x0a-0x0b     Palette base (/256)
  0x0c-0x0d     Scroll 1 X
  0x0e-0x0f     Scroll 1 Y
  0x10-0x11     Scroll 2 X
  0x12-0x13     Scroll 2 Y
  0x14-0x15     Scroll 3 X
  0x16-0x17     Scroll 3 Y
  0x18-0x19     Related to X pos (xored with 0x1e0 then 0x20 added when flip)
  0x1a-0x1b     Related to Y pos (xored with 0x100 when screen flip on)
  0x1c-0x1d     Related to X pos (xored with 0x1e0 then 0x20 added when flip)
  0x1e-0x1f     Related to Y pos (xored with 0x100 when screen flip on)
  0x20-0x21     start offset for the rowscroll matrix
  0x22-0x23     unknown but widely used - usually 0x0e. bit 0 enables rowscroll
				on layer 2. bit 15 is flip screen.


  Registers move from game to game.. following example strider
  0x66-0x67     Video control register (location varies according to game)
  0x68-0x69     Priority mask
  0x6a-0x6b     Priority mask
  0x6c-0x6d     Priority mask
  0x6e-0x6f     Priority mask
  0x70-0x71     Control register (usually 0x003f)

  Fixed registers
  0x80-0x81     Sound command
  0x88-0x89     Sound fade

  Known Bug List
  ==============
  All games
  * Large sprites don't exit smoothly on the left side of the screen (e.g. Car
	in Mega Man attract mode, cadillac in Cadillacs and Dinosaurs)
  * There might be problems if high priority tiles (over sprites) and rowscroll
    are used at the same time, because the priority buffer is not rowscrolled.
    The only place I know were this might cause problems is the cave in mtwins,
    which waves to simulate heat. I haven't noticed anything wrong, though.

  Magic Sword.
  * during attract mode, characters are shown with a black background. There is
	a background, but the layers are disabled. I think this IS the correct
	behaviour.

  King of Dragons (World).
  * Distortion effect missing on character description screen during attract
	mode. The game rapidly toggles on and off the layer enable bit. Again, I
	think this IS the correct behaviour. The Japanese version does the
	distortion as expected.

  qad
  * layer enable mask incomplete

   dino
  * in level 6, the legs of the big dino which stomps you are almost entirely
	missing.
  * in level 6, palette changes due to lightnings cause a lot of tiling effects
	on scroll2.

  Unknown issues
  ==============

  There are often some redundant high bits in the scroll layer's attributes.
  I think that these are spare bits that the game uses for to store additional
  information, not used by the hardware.
  The games seem to use them to mark platforms, kill zones and no-go areas.

***************************************************************************/

#ifndef SELF_INCLUDE

#include "driver.h"
#include "vidhrdw/generic.h"
#include "drivers/cps1.h"

#define VERBOSE 0

#define CPS1_DUMP_VIDEO 0

/********************************************************************

			Configuration table:

********************************************************************/

/* Game specific data */
struct CPS1config
{
	char *name;             /* game driver name */

	/* Some games interrogate a couple of registers on bootup. */
	/* These are CPS1 board B self test checks. They wander from game to */
	/* game. */
	int cpsb_addr;        /* CPS board B test register address */
	int cpsb_value;       /* CPS board B test register expected value */

	/* some games use as a protection check the ability to do 16-bit multiplies */
	/* with a 32-bit result, by writing the factors to two ports and reading the */
	/* result from two other ports. */
	/* It looks like this feature was introduced with 3wonders (CPSB ID = 08xx) */
	int mult_factor1;
	int mult_factor2;
	int mult_result_lo;
	int mult_result_hi;

	int layer_control;
	int priority0;
	int priority1;
	int priority2;
	int priority3;
	int control_reg;  /* Control register? seems to be always 0x3f */

	/* ideally, the layer enable masks should consist of only one bit, */
	/* but in many cases it is unknown which bit is which. */
	int scrl1_enable_mask;
	int scrl2_enable_mask;
	int scrl3_enable_mask;

	int bank_scroll1;
	int bank_scroll2;
	int bank_scroll3;

	/* Some characters aren't visible */
	const int space_scroll1;
	const int start_scroll2;
	const int end_scroll2;
	const int start_scroll3;
	const int end_scroll3;

	int kludge;  /* Ghouls n Ghosts sprite kludge */
};

struct CPS1config *cps1_game_config;

static struct CPS1config cps1_config_table[]=
{
	/* name       CPSB ID    multiply protection  ctrl    priority masks  unknwn  layer enable   banks spacechr kludge */
	{"forgottn",0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x04,0x08, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff, 7},
	{"lostwrld",0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x04,0x08, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff, 7},
	{"ghouls",  0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x04,0x08, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff, 1},
	{"ghoulsu", 0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x04,0x08, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff, 1},
	{"ghoulsj", 0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x04,0x08, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff, 1},
	{"strider", 0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 1,0,1,     -1,0x0000,0xffff,0x0000,0xffff },
	{"striderj",0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 1,0,1,     -1,0x0000,0xffff,0x0000,0xffff },
	{"stridrja",0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 1,0,1,     -1,0x0000,0xffff,0x0000,0xffff },
	{"dwj",     0x00,0x0000, 0x00,0x00,0x00,0x00, 0x6c,0x6a,0x68,0x66,0x64,0x62, 0x02,0x04,0x08, 0,1,1,     -1,0x0000,0xffff,0x0000,0xffff },
	{"willow",  0x00,0x0000, 0x00,0x00,0x00,0x00, 0x70,0x6e,0x6c,0x6a,0x68,0x66, 0x20,0x10,0x08, 0,1,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"willowj", 0x00,0x0000, 0x00,0x00,0x00,0x00, 0x70,0x6e,0x6c,0x6a,0x68,0x66, 0x20,0x10,0x08, 0,1,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"unsquad", 0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x38,0x38,0x38, 0,0,0,     -1,0x0000,0xffff,0x0001,0xffff },
	{"area88",  0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x38,0x38,0x38, 0,0,0,     -1,0x0000,0xffff,0x0001,0xffff },
	{"ffight",  0x60,0x0004, 0x00,0x00,0x00,0x00, 0x6e,0x66,0x70,0x68,0x72,0x6a, 0x02,0x0c,0x0c, 0,0,0,     -1,0x0001,0xffff,0x0001,0xffff },
	{"ffightu", 0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 0,0,0,     -1,0x0001,0xffff,0x0001,0xffff },
	{"ffightj", 0x60,0x0004, 0x00,0x00,0x00,0x00, 0x6e,0x66,0x70,0x68,0x72,0x6a, 0x02,0x0c,0x0c, 0,0,0,     -1,0x0001,0xffff,0x0001,0xffff },
	{"1941",    0x60,0x0005, 0x00,0x00,0x00,0x00, 0x68,0x6a,0x6c,0x6e,0x70,0x72, 0x02,0x08,0x20, 0,0,0,     -1,0x0000,0xffff,0x0400,0x07ff },
	{"1941j",   0x60,0x0005, 0x00,0x00,0x00,0x00, 0x68,0x6a,0x6c,0x6e,0x70,0x72, 0x02,0x08,0x20, 0,0,0,     -1,0x0000,0xffff,0x0400,0x07ff },
	{"mercs",   0x60,0x0402, 0x00,0x00,0x00,0x00, 0x6c,0x00,0x00,0x00,0x00,0x62, 0x02,0x04,0x08, 0,0,0,     -1,0x0600,0x5bff,0x0700,0x17ff, 4 },	/* (uses port 74) */
	{"mercsu",  0x60,0x0402, 0x00,0x00,0x00,0x00, 0x6c,0x00,0x00,0x00,0x00,0x62, 0x02,0x04,0x08, 0,0,0,     -1,0x0600,0x5bff,0x0700,0x17ff, 4 },	/* (uses port 74) */
	{"mercsj",  0x60,0x0402, 0x00,0x00,0x00,0x00, 0x6c,0x00,0x00,0x00,0x00,0x62, 0x02,0x04,0x08, 0,0,0,     -1,0x0600,0x5bff,0x0700,0x17ff, 4 },	/* (uses port 74) */
	{"mtwins",  0x5e,0x0404, 0x00,0x00,0x00,0x00, 0x52,0x54,0x56,0x58,0x5a,0x5c, 0x38,0x38,0x38, 0,0,0,     -1,0x0000,0xffff,0x0e00,0xffff },
	{"chikij",  0x5e,0x0404, 0x00,0x00,0x00,0x00, 0x52,0x54,0x56,0x58,0x5a,0x5c, 0x38,0x38,0x38, 0,0,0,     -1,0x0000,0xffff,0x0e00,0xffff },
	{"msword",  0x00,0x0000, 0x00,0x00,0x00,0x00, 0x62,0x64,0x66,0x68,0x6a,0x6c, 0x20,0x06,0x06, 0,0,0,     -1,0x2800,0x37ff,0x0000,0xffff },
	{"mswordu", 0x00,0x0000, 0x00,0x00,0x00,0x00, 0x62,0x64,0x66,0x68,0x6a,0x6c, 0x20,0x06,0x06, 0,0,0,     -1,0x2800,0x37ff,0x0000,0xffff },
	{"mswordj", 0x00,0x0000, 0x00,0x00,0x00,0x00, 0x62,0x64,0x66,0x68,0x6a,0x6c, 0x20,0x06,0x06, 0,0,0,     -1,0x2800,0x37ff,0x0000,0xffff },
	{"cawing",  0x40,0x0406, 0x00,0x00,0x00,0x00, 0x4c,0x4a,0x48,0x46,0x44,0x42, 0x10,0x0a,0x0a, 0,0,0, 0x0002,0x0000,0xffff,0x0000,0xffff, 6},	/* row scroll used at the beginning of mission 8, put 07 at ff8501 to jump there */
	{"cawingj", 0x40,0x0406, 0x00,0x00,0x00,0x00, 0x4c,0x4a,0x48,0x46,0x44,0x42, 0x10,0x0a,0x0a, 0,0,0, 0x0002,0x0000,0xffff,0x0000,0xffff, 6},	/* row scroll used at the beginning of mission 8, put 07 at ff8501 to jump there */
	{"nemo",    0x4e,0x0405, 0x00,0x00,0x00,0x00, 0x42,0x44,0x46,0x48,0x4a,0x4c, 0x04,0x22,0x22, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"nemoj",   0x4e,0x0405, 0x00,0x00,0x00,0x00, 0x42,0x44,0x46,0x48,0x4a,0x4c, 0x04,0x22,0x22, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sf2",     0x48,0x0407, 0x00,0x00,0x00,0x00, 0x54,0x52,0x50,0x4e,0x4c,0x4a, 0x08,0x12,0x12, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sf2a",    0x48,0x0407, 0x00,0x00,0x00,0x00, 0x54,0x52,0x50,0x4e,0x4c,0x4a, 0x08,0x12,0x12, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sf2b",    0x48,0x0407, 0x00,0x00,0x00,0x00, 0x54,0x52,0x50,0x4e,0x4c,0x4a, 0x08,0x12,0x12, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sf2e",    0xd0,0x0408, 0x00,0x00,0x00,0x00, 0xdc,0xda,0xd8,0xd6,0xd4,0xd2, 0x10,0x0a,0x0a, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sf2j",    0x6e,0x0403, 0x00,0x00,0x00,0x00, 0x62,0x64,0x66,0x68,0x6a,0x6c, 0x20,0x06,0x06, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sf2jb",   0x48,0x0407, 0x00,0x00,0x00,0x00, 0x54,0x52,0x50,0x4e,0x4c,0x4a, 0x08,0x12,0x12, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"3wonders",0x72,0x0800, 0x4e,0x4c,0x4a,0x48, 0x68,0x66,0x64,0x62,0x60,0x70, 0x20,0x04,0x08, 0,1,1,     -1,0x0000,0xffff,0x0000,0xffff, 2},
	{"wonder3", 0x72,0x0800, 0x4e,0x4c,0x4a,0x48, 0x68,0x66,0x64,0x62,0x60,0x70, 0x20,0x04,0x08, 0,1,1,     -1,0x0000,0xffff,0x0000,0xffff, 2},
	{"kod",     0x00,0x0000, 0x5e,0x5c,0x5a,0x58, 0x60,0x6e,0x6c,0x6a,0x68,0x70, 0x30,0x08,0x30, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"kodj",    0x00,0x0000, 0x5e,0x5c,0x5a,0x58, 0x60,0x6e,0x6c,0x6a,0x68,0x70, 0x30,0x08,0x30, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"kodb",    0x00,0x0000, 0x00,0x00,0x00,0x00, 0x60,0x6e,0x6c,0x6a,0x68,0x70, 0x30,0x08,0x30, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"captcomm",0x00,0x0000, 0x46,0x44,0x42,0x40, 0x60,0x6e,0x6c,0x6a,0x68,0x70, 0x20,0x12,0x12, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },	/* multiply is used only to center the startup text */
	{"captcomu",0x00,0x0000, 0x46,0x44,0x42,0x40, 0x60,0x6e,0x6c,0x6a,0x68,0x70, 0x20,0x12,0x12, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },	/* multiply is used only to center the startup text */
	{"captcomj",0x00,0x0000, 0x46,0x44,0x42,0x40, 0x60,0x6e,0x6c,0x6a,0x68,0x70, 0x20,0x12,0x12, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },	/* multiply is used only to center the startup text */
	{"knights", 0x00,0x0000, 0x46,0x44,0x42,0x40, 0x68,0x66,0x64,0x62,0x60,0x70, 0x20,0x10,0x02, 0,0,0, 0xf020,0x0000,0xffff,0x0000,0xffff },
	{"knightsj",0x00,0x0000, 0x46,0x44,0x42,0x40, 0x68,0x66,0x64,0x62,0x60,0x70, 0x20,0x10,0x02, 0,0,0, 0xf020,0x0000,0xffff,0x0000,0xffff },
	{"sf2ce",   0x00,0x0000, 0x40,0x42,0x44,0x46, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sf2cea",  0x00,0x0000, 0x40,0x42,0x44,0x46, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sf2ceb",  0x00,0x0000, 0x40,0x42,0x44,0x46, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sf2cej",  0x00,0x0000, 0x40,0x42,0x44,0x46, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sf2rb",   0x00,0x0000, 0x40,0x42,0x44,0x46, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sf2red",  0x00,0x0000, 0x40,0x42,0x44,0x46, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sf2accp2",0x00,0x0000, 0x40,0x42,0x44,0x46, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"varth",   0x00,0x0000, 0x00,0x00,0x00,0x00, 0x6e,0x66,0x70,0x68,0x72,0x6a, 0x02,0x0c,0x0c, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },	/* CPSB test has been patched out (60=0008) */
	{"varthu",  0x00,0x0000, 0x00,0x00,0x00,0x00, 0x6e,0x66,0x70,0x68,0x72,0x6a, 0x02,0x0c,0x0c, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },	/* CPSB test has been patched out (60=0008) */
	{"varthj",  0x00,0x0000, 0x4e,0x4c,0x4a,0x48, 0x60,0x6e,0x6c,0x6a,0x68,0x70, 0x20,0x06,0x06, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },	/* CPSB test has been patched out (72=0001) */
	{"cworld2j",0x00,0x0000, 0x00,0x00,0x00,0x00, 0x60,0x6e,0x6c,0x6a,0x68,0x70, 0x20,0x14,0x14, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },  /* The 0x76 priority values are incorrect values */
	{"wof",     0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x04,0x08, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"wofa",    0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x04,0x08, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"wofj",    0x00,0x0000, 0x00,0x00,0x00,0x00, 0x62,0x64,0x66,0x68,0x6a,0x6c, 0x10,0x08,0x04, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"dino",    0x00,0x0000, 0x00,0x00,0x00,0x00, 0x4a,0x4c,0x4e,0x40,0x42,0x44, 0x16,0x16,0x16, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },	/* layer enable never used */
	{"dinoj",   0x00,0x0000, 0x00,0x00,0x00,0x00, 0x4a,0x4c,0x4e,0x40,0x42,0x44, 0x16,0x16,0x16, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },	/* layer enable never used */
	{"punisher",0x4e,0x0c00, 0x00,0x00,0x00,0x00, 0x52,0x54,0x56,0x48,0x4a,0x4c, 0x04,0x02,0x20, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"punishru",0x4e,0x0c00, 0x00,0x00,0x00,0x00, 0x52,0x54,0x56,0x48,0x4a,0x4c, 0x04,0x02,0x20, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"punishrj",0x4e,0x0c00, 0x00,0x00,0x00,0x00, 0x52,0x54,0x56,0x48,0x4a,0x4c, 0x04,0x02,0x20, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"slammast",0x6e,0x0c01, 0x00,0x00,0x00,0x00, 0x56,0x40,0x42,0x68,0x6a,0x6c, 0x0c,0x0c,0x10, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"mbomberj",0x6e,0x0c01, 0x00,0x00,0x00,0x00, 0x56,0x40,0x42,0x68,0x6a,0x6c, 0x0c,0x0c,0x10, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"mbombrd", 0x5e,0x0c02, 0x00,0x00,0x00,0x00, 0x6a,0x6c,0x6e,0x70,0x72,0x5c, 0x0c,0x0c,0x10, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"mbombrdj",0x5e,0x0c02, 0x00,0x00,0x00,0x00, 0x6a,0x6c,0x6e,0x70,0x72,0x5c, 0x0c,0x0c,0x10, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sf2t",    0x00,0x0000, 0x40,0x42,0x44,0x46, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sf2tj",   0x00,0x0000, 0x40,0x42,0x44,0x46, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 2,2,2,     -1,0x0000,0xffff,0x0000,0xffff },
	{"pnickj",  0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x00,0x00,0x00,0x00,0x70, 0x0e,0x0e,0x0e, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"qad",     0x00,0x0000, 0x00,0x00,0x00,0x00, 0x6c,0x00,0x00,0x00,0x00,0x52, 0x14,0x02,0x14, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"qadj",    0x00,0x0000, 0x40,0x42,0x44,0x46, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x0e,0x0e,0x0e, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"qtono2",  0x00,0x0000, 0x40,0x42,0x44,0x46, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x06,0x06,0x08, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"pang3",   0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff, 5},	/* EEPROM port is among the CPS registers */
	{"pang3j",  0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x0c,0x0c, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff, 5}, /* EEPROM port is among the CPS registers */
	{"megaman", 0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x04,0x08, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"rockmanj",0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x04,0x08, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	{"sfzch",   0x00,0x0000, 0x00,0x00,0x00,0x00, 0x66,0x68,0x6a,0x6c,0x6e,0x70, 0x02,0x04,0x08, 0,0,0,     -1,0x0000,0xffff,0x0000,0xffff },
	/* End of table */
	{0,	    0x00,0x0000, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00, 0,0,0,      0,0x0000,0x0000,0x0000,0x0000}
};

static void cps1_init_machine(void)
{
	const char *gamename = Machine->gamedrv->name;
	unsigned char *RAM = memory_region(REGION_CPU1);


	struct CPS1config *pCFG=&cps1_config_table[0];
	while(pCFG->name)
	{
		if (strcmp(pCFG->name, gamename) == 0)
		{
			break;
		}
		pCFG++;
	}
#ifdef MAMEGO
	/* modern sets (scripts/cps1_modern_sets.py) are not in the table: their
	   parent is the 0.37 set with the same chips, hence the same CPS-B board.
	   Without this they got the empty entry (every layer masked off: the
	   Street Fighter II' stages had no background) */
	{
		const struct GameDriver *d = Machine->gamedrv;
		while (!pCFG->name && (d = d->clone_of) != NULL)
			for (pCFG = &cps1_config_table[0]; pCFG->name; pCFG++)
				if (strcmp(pCFG->name, d->name) == 0)
					break;
		if (!pCFG->name)
			printf("cps1: no board configuration for %s\n", gamename);
	}
#endif
	cps1_game_config=pCFG;

	if (strcmp(gamename, "sf2rb" )==0)
	{
		/* Patch out protection check */
		WRITE_WORD(&RAM[0xe5464],0x6012);
	}
#if 0
	else if (strcmp(gamename, "ghouls" )==0)
	{
		/* Patch out self-test... it takes forever */
		WRITE_WORD(&RAM[0x61964+0], 0x4ef9);
		WRITE_WORD(&RAM[0x61964+2], 0x0000);
		WRITE_WORD(&RAM[0x61964+4], 0x0400);
	}
#endif
}

static INLINE int cps1_port(int offset)
{
	return READ_WORD(&cps1_output[offset]);
}

static INLINE unsigned char *cps1_base(int offset,int boundary)
{
	int base=cps1_port(offset)*256;
	/*
	The scroll RAM must start on a 0x4000 boundary.
	Some games do not do this.
	For example:
	   Captain commando     - continue screen will not display
	   Muscle bomber games  - will animate garbage during gameplay
	Mask out the irrelevant bits.
	*/
	base &= ~(boundary-1);
 	return &cps1_gfxram[base&0x3ffff];
}



READ_HANDLER( cps1_output_r )
{
#if VERBOSE
if (offset >= 0x18) logerror("PC %06x: read output port %02x\n",cpu_get_pc(),offset);
#endif

	/* Some games interrogate a couple of registers on bootup. */
	/* These are CPS1 board B self test checks. They wander from game to */
	/* game. */
	if (offset && offset == cps1_game_config->cpsb_addr)
		return cps1_game_config->cpsb_value;

	/* some games use as a protection check the ability to do 16-bit multiplies */
	/* with a 32-bit result, by writing the factors to two ports and reading the */
	/* result from two other ports. */
	if (offset && offset == cps1_game_config->mult_result_lo)
		return (READ_WORD(&cps1_output[cps1_game_config->mult_factor1]) *
				READ_WORD(&cps1_output[cps1_game_config->mult_factor2])) & 0xffff;
	if (offset && offset == cps1_game_config->mult_result_hi)
		return (READ_WORD(&cps1_output[cps1_game_config->mult_factor1]) *
				READ_WORD(&cps1_output[cps1_game_config->mult_factor2])) >> 16;

	/* Pang 3 EEPROM interface */
	if (cps1_game_config->kludge == 5 && offset == 0x7a)
		return cps1_eeprom_port_r(0);

	return READ_WORD(&cps1_output[offset]);
}

WRITE_HANDLER( cps1_output_w )
{
#if VERBOSE
if (offset >= 0x18 && //offset != 0x22 &&
		offset != cps1_game_config->layer_control &&
		offset != cps1_game_config->priority0 &&
		offset != cps1_game_config->priority1 &&
		offset != cps1_game_config->priority2 &&
		offset != cps1_game_config->priority3 &&
		offset != cps1_game_config->control_reg)
	logerror("PC %06x: write %02x to output port %02x\n",cpu_get_pc(),data,offset);
#endif

	/* Pang 3 EEPROM interface */
	if (cps1_game_config->kludge == 5 && offset == 0x7a)
	{
		cps1_eeprom_port_w(0,data);
		return;
	}

	COMBINE_WORD_MEM(&cps1_output[offset],data);
}



/* Public variables */
unsigned char *cps1_gfxram;
unsigned char *cps1_output;


size_t cps1_gfxram_size;
size_t cps1_output_size;

/* Private */

/* Offset of each palette entry */
const int cps1_obj_palette    =0;
const int cps1_scroll1_palette=32;
const int cps1_scroll2_palette=32+32;
const int cps1_scroll3_palette=32+32+32;
#define cps1_palette_entries (32*4)  /* Number colour schemes in palette */

const int cps1_scroll1_size=0x4000;
const int cps1_scroll2_size=0x4000;
const int cps1_scroll3_size=0x4000;
const int cps1_obj_size    =0x0800;
const int cps1_other_size  =0x0800;
const int cps1_palette_size=cps1_palette_entries*32; /* Size of palette RAM */
static int cps1_flip_screen;    /* Flip screen on / off */

static unsigned char *cps1_scroll1;
static unsigned char *cps1_scroll2;
static unsigned char *cps1_scroll3;
static unsigned char *cps1_obj;
static unsigned char *cps1_buffered_obj;
static unsigned char *cps1_palette;
static unsigned char *cps1_other;
static unsigned char *cps1_old_palette;

/* Working variables */
static int cps1_last_sprite_offset;     /* Offset of the last sprite */
static int cps1_layer_enabled[4];       /* Layer enabled [Y/N] */

int scroll1x, scroll1y, scroll2x, scroll2y, scroll3x, scroll3y;
static unsigned char *cps1_scroll2_old;
static struct osd_bitmap *cps1_scroll2_bitmap;


/* Output ports */
#define CPS1_OBJ_BASE			0x00    /* Base address of objects */
#define CPS1_SCROLL1_BASE       0x02    /* Base address of scroll 1 */
#define CPS1_SCROLL2_BASE       0x04    /* Base address of scroll 2 */
#define CPS1_SCROLL3_BASE       0x06    /* Base address of scroll 3 */
#define CPS1_OTHER_BASE			0x08    /* Base address of other video */
#define CPS1_PALETTE_BASE       0x0a    /* Base address of palette */
#define CPS1_SCROLL1_SCROLLX    0x0c    /* Scroll 1 X */
#define CPS1_SCROLL1_SCROLLY    0x0e    /* Scroll 1 Y */
#define CPS1_SCROLL2_SCROLLX    0x10    /* Scroll 2 X */
#define CPS1_SCROLL2_SCROLLY    0x12    /* Scroll 2 Y */
#define CPS1_SCROLL3_SCROLLX    0x14    /* Scroll 3 X */
#define CPS1_SCROLL3_SCROLLY    0x16    /* Scroll 3 Y */

#define CPS1_ROWSCROLL_OFFS     0x20    /* base of row scroll offsets in other RAM */

#define CPS1_SCROLL2_WIDTH      0x40
#define CPS1_SCROLL2_HEIGHT     0x40


/*
CPS1 VIDEO RENDERER

*/
static uint32_t *cps1_gfx;		 /* Converted GFX memory */
/* pen usage: a mask of the 16 pens a tile uses. 16 bits on mame-go: the
   three arrays are 1.25 MB as ints for Street Fighter II's 6 MB of tiles */
#ifdef MAMEGO
typedef uint16_t cps1_pen_t;
#else
typedef int cps1_pen_t;
#endif
static cps1_pen_t *cps1_char_pen_usage;	/* pen usage array */
static cps1_pen_t *cps1_tile16_pen_usage;      /* pen usage array */
static cps1_pen_t *cps1_tile32_pen_usage;      /* pen usage array */
static int cps1_max_char;	       /* Maximum number of 8x8 chars */
static int cps1_max_tile16;	     /* Maximum number of 16x16 tiles */
static int cps1_max_tile32;	     /* Maximum number of 32x32 tiles */

#ifdef MAMEGO
#define CPS1_CHUNK_WORDS 16384 /* 64 KB */
static int cps1_gfx_flash;
static uint32_t *cps1_gfx_chunk;
static size_t cps1_gfx_offset;
#define CPS1_GFX_OUT(n) (*(cps1_gfx_flash ? &cps1_gfx_chunk[(n) % CPS1_CHUNK_WORDS] : &cps1_gfx[n]))
#else
#define CPS1_GFX_OUT(n) cps1_gfx[n]
#endif

#ifdef MAMEGO
/* ----------------------------------------------------------------------
 * Graphics streamed from the zip (mame-go). The conversion below reads four
 * quarters of the graphics ROM in step (bit planes), each sequentially, so
 * the ROM never has to sit in memory: four readers go through the zip at
 * once and each 64 KB block of tiles goes to the mamerom flash partition,
 * the part that does not fit (Street Fighter II: 6 MB of tiles, 4 MB
 * partition) to PSRAM. Drawing reads tile words below cps1_gfx_split from
 * cps1_gfx (flash) and the rest from cps1_gfx_hi. Used when the region is
 * plain ROM_LOADs laid end to end (all but ~25 sets); else the region is
 * loaded as usual and converted below.
 * ---------------------------------------------------------------------- */
static uint32_t *cps1_gfx_hi;
static unsigned cps1_gfx_split = ~0u;       /* in words */
static const struct RomModule *cps1gfx_rom;
static int cps1gfx_entries;

void *mamego_romrd_open(const struct RomModule *rom);
int mamego_romrd_read(void *r, void *buf, unsigned len);
void mamego_romrd_close(void *r);

/* readroms() pre-pass (common.c): a stub instead of loading the region */
unsigned char *cps1gfx_region_stub(int type, const struct RomModule *first, int entries, unsigned size)
{
	unsigned expect = 0;
	int i;
	cps1gfx_rom = 0;
	if (!Machine->drv || Machine->drv->vh_start != cps1_vh_start || type != REGION_GFX1)
		return NULL;
#ifndef ESP_PLATFORM
	if (getenv("CPS1STREAM") && !strcmp(getenv("CPS1STREAM"), "0"))
		return NULL;
#endif
	for (i = 0; i < entries; i++)
	{
		const struct RomModule *r = &first[i];
		if (!r->name || r->name == (char *)-1 || (r->length & ROMFLAG_MASK) || r->offset != expect)
			return NULL;
		expect += r->length;
	}
	if (expect != size || size % (4 * 16384))
		return NULL;
	cps1gfx_rom = first;
	cps1gfx_entries = entries;
	return (unsigned char *)malloc(16);
}

struct cps1_rd { void *r; int idx; unsigned left; };

static int cps1_rd_open(struct cps1_rd *g, unsigned offset, unsigned char *scratch, unsigned scratch_len)
{
	unsigned skip;
	for (g->idx = 0; g->idx < cps1gfx_entries; g->idx++)
		if (offset < cps1gfx_rom[g->idx].offset + cps1gfx_rom[g->idx].length)
			break;
	if (g->idx >= cps1gfx_entries || !(g->r = mamego_romrd_open(&cps1gfx_rom[g->idx])))
		return -1;
	skip = offset - cps1gfx_rom[g->idx].offset;
	g->left = cps1gfx_rom[g->idx].length - skip;
	while (skip)
	{
		unsigned k = skip < scratch_len ? skip : scratch_len;
		if (mamego_romrd_read(g->r, scratch, k) != (int)k)
			return -1;
		skip -= k;
	}
	return 0;
}

static int cps1_rd_read(struct cps1_rd *g, unsigned char *buf, unsigned n)
{
	while (n)
	{
		unsigned k;
		if (!g->left)
		{
			mamego_romrd_close(g->r);
			g->r = 0;
			if (++g->idx >= cps1gfx_entries || !(g->r = mamego_romrd_open(&cps1gfx_rom[g->idx])))
				return -1;
			g->left = cps1gfx_rom[g->idx].length;
		}
		k = n < g->left ? n : g->left;
		if (mamego_romrd_read(g->r, buf, k) != (int)k)
			return -1;
		buf += k;
		n -= k;
		g->left -= k;
	}
	return 0;
}

#define CPS1_BLOCK 8192   /* i's per block: 16 KB per quarter, 64 KB of tiles */
static int cps1_gfx_start_stream(int size)
{
	extern unsigned char *mamego_flash_store(const unsigned char *data, size_t len, size_t *offset);
	extern size_t mamego_flash_offset;
	struct cps1_rd q[4];
	unsigned char *in = malloc(4 * 2 * CPS1_BLOCK);
	uint32_t *out = malloc(2 * CPS1_BLOCK * sizeof(uint32_t));
	unsigned total = size / 4, written = 0, base, k, qn;   /* words */
	size_t offset = mamego_flash_offset;
	int ret = -1;

	memset(q, 0, sizeof(q));
	cps1_gfx = 0;
	cps1_gfx_hi = 0;
	cps1_gfx_split = ~0u;
	if (!in || !out)
		goto done;
	{
		/* tiles in PSRAM when they leave room for the rest (scroll-2 bitmap,
		   frame surfaces: ~2.5 MB): reading them from the flash mapping is
		   slower (Final Fight, Carrier Air Wing: 2 MB of tiles) */
		size_t need = (size_t)total * 4;
#ifdef ESP_PLATFORM
		extern size_t mamego_psram_free(size_t *largest);
		size_t largest = 0, freemem = mamego_psram_free(&largest);
		int fits = largest >= need && freemem >= need + 2000 * 1024;
		printf("cps1: PSRAM %u KB free (largest %u) for %u KB of tiles: %s\n", (unsigned)(freemem / 1024),
			(unsigned)(largest / 1024), (unsigned)(need / 1024), fits ? "PSRAM" : "flash");
#else
		int fits = !getenv("CPS1FLASH");
#endif
		if (fits && (cps1_gfx_hi = malloc(need)) != NULL)
		{
			cps1_gfx_split = 0;
			printf("cps1: %u KB of tiles in PSRAM\n", (unsigned)(need / 1024));
		}
	}
	for (qn = 0; qn < 4; qn++)
		if (cps1_rd_open(&q[qn], qn * (size / 4), in, 2 * CPS1_BLOCK) != 0)
			goto done;

	for (base = 0; base < (unsigned)size / 8; base += CPS1_BLOCK)
	{
		const unsigned char *q0 = in, *q1 = in + 2 * CPS1_BLOCK, *q2 = in + 4 * CPS1_BLOCK, *q3 = in + 6 * CPS1_BLOCK;
		for (qn = 0; qn < 4; qn++)
			if (cps1_rd_read(&q[qn], in + qn * 2 * CPS1_BLOCK, 2 * CPS1_BLOCK) != 0)
				goto done;
		for (k = 0; k < CPS1_BLOCK; k++)
		{
			int nchar = (base + k) / 8, j;
			uint32_t d0 = 0, d1 = 0, pens = 0;
			for (j = 0; j < 8; j++)
			{
				int mask = 0x80 >> j, n0 = 0, n1 = 0;
				if (q1[2*k] & mask)     n0 |= 1;   /* the first word: quarters 1 and 3 */
				if (q1[2*k+1] & mask)   n0 |= 2;
				if (q3[2*k] & mask)     n0 |= 4;
				if (q3[2*k+1] & mask)   n0 |= 8;
				if (q0[2*k] & mask)     n1 |= 1;   /* the second: quarters 0 and 2 */
				if (q0[2*k+1] & mask)   n1 |= 2;
				if (q2[2*k] & mask)     n1 |= 4;
				if (q2[2*k+1] & mask)   n1 |= 8;
				d0 |= n0 << (28 - j * 4);
				d1 |= n1 << (28 - j * 4);
				pens |= (1 << n0) | (1 << n1);
			}
			out[2*k] = d0;
			out[2*k+1] = d1;
			cps1_char_pen_usage[nchar] |= pens;
			cps1_tile16_pen_usage[nchar/2] |= pens;
			cps1_tile32_pen_usage[nchar/8] |= pens;
		}
		/* 64 KB of tiles: to flash while it has room, then to PSRAM */
		if (!cps1_gfx_hi)
		{
			uint32_t *p = (uint32_t *)mamego_flash_store((unsigned char *)out, 2 * CPS1_BLOCK * 4, &offset);
			if (p && (!cps1_gfx || p == cps1_gfx + written))
			{
				if (!cps1_gfx)
					cps1_gfx = p;
				written += 2 * CPS1_BLOCK;
				continue;
			}
			cps1_gfx_split = written;
			if (!(cps1_gfx_hi = malloc((size_t)(total - written) * 4)))
				goto done;
		}
		memcpy(cps1_gfx_hi + (written - cps1_gfx_split), out, 2 * CPS1_BLOCK * 4);
		written += 2 * CPS1_BLOCK;
	}
	mamego_flash_offset = offset;
	if (cps1_gfx_split == 0)
		;
	else if (cps1_gfx_split == ~0u)
		printf("cps1: %u KB of tiles streamed to flash\n", total * 4 / 1024);
	else
		printf("cps1: %u KB of tiles streamed: %u KB in flash, %u KB in PSRAM\n",
			total * 4 / 1024, cps1_gfx_split * 4 / 1024, (total - cps1_gfx_split) * 4 / 1024);
	ret = 0;
done:
	for (qn = 0; qn < 4; qn++)
		mamego_romrd_close(q[qn].r);
	free(in);
	free(out);
	if (ret)
		printf("cps1: streaming the graphics failed\n");
	return ret;
}
#endif

int cps1_gfx_start(void)
{
	uint32_t dwval;
	int size=memory_region_length(REGION_GFX1);
	unsigned char *data = memory_region(REGION_GFX1);
	int i,j,nchar,penusage,gfxsize;

	gfxsize=size/4;

	/* Set up maximum values */
	cps1_max_char  =(gfxsize/2)/8;
	cps1_max_tile16=(gfxsize/4)/8;
	cps1_max_tile32=(gfxsize/16)/8;

#ifdef MAMEGO
	if (cps1gfx_rom)
	{
		/* streamed: pen usage arrays first, then the tiles */
		cps1_char_pen_usage=(cps1_pen_t*)calloc(cps1_max_char, sizeof(cps1_pen_t));
		cps1_tile16_pen_usage=(cps1_pen_t*)calloc(cps1_max_tile16, sizeof(cps1_pen_t));
		cps1_tile32_pen_usage=(cps1_pen_t*)calloc(cps1_max_tile32, sizeof(cps1_pen_t));
		if (!cps1_char_pen_usage || !cps1_tile16_pen_usage || !cps1_tile32_pen_usage)
			return -1;
		cps1_gfx_flash = 1; /* cps1_gfx is not ours to free (flash) */
		if (cps1_gfx_start_stream(size) != 0)
			return -1;
		goto free_rom;
	}
	/* mame-go: the decoded tiles are read-only once built, so they go to the
	   mamerom flash partition 64 KB at a time instead of a second PSRAM copy
	   of the graphics (Final Fight, King of Dragons: 2-4 MB). The ROM region
	   stays in PSRAM until the tiles are built (it is freed right after,
	   REGIONFLAG_DISPOSE). Falls back to PSRAM when the partition is too small. */
	{
		extern unsigned char *mamego_flash_store(const unsigned char *data, size_t len, size_t *offset);
		extern size_t mamego_flash_offset;
		cps1_gfx_flash = 0;
		cps1_gfx_chunk = (uint32_t*)malloc(CPS1_CHUNK_WORDS*sizeof(uint32_t));
		if (cps1_gfx_chunk && !(gfxsize % CPS1_CHUNK_WORDS))
		{
			size_t offset = mamego_flash_offset;
			cps1_gfx_flash = 1;
			cps1_gfx = 0;
			cps1_gfx_offset = offset;
		}
	}
	if (!cps1_gfx_flash)
#endif
	cps1_gfx=(uint32_t*)malloc(gfxsize*sizeof(uint32_t));
	if (!cps1_gfx
#ifdef MAMEGO
		&& !cps1_gfx_flash
#endif
		)
	{
		return -1;
	}

	cps1_char_pen_usage=(cps1_pen_t*)malloc(cps1_max_char*sizeof(cps1_pen_t));
	if (!cps1_char_pen_usage)
	{
		return -1;
	}
	memset(cps1_char_pen_usage, 0, cps1_max_char*sizeof(cps1_pen_t));

	cps1_tile16_pen_usage=(cps1_pen_t*)malloc(cps1_max_tile16*sizeof(cps1_pen_t));
	if (!cps1_tile16_pen_usage)
		return -1;
	memset(cps1_tile16_pen_usage, 0, cps1_max_tile16*sizeof(cps1_pen_t));

	cps1_tile32_pen_usage=(cps1_pen_t*)malloc(cps1_max_tile32*sizeof(cps1_pen_t));
	if (!cps1_tile32_pen_usage)
	{
		return -1;
	}
	memset(cps1_tile32_pen_usage, 0, cps1_max_tile32*sizeof(cps1_pen_t));

	{
		for (i=0; i<gfxsize/2; i++)
		{
			nchar=i/8;  /* 8x8 char number */
		   dwval=0;
		   for (j=0; j<8; j++)
		   {
				int n,mask;
				n=0;
				mask=0x80>>j;
				if (*(data+size/4)&mask)	   n|=1;
				if (*(data+size/4+1)&mask)	 n|=2;
				if (*(data+size/2+size/4)&mask)    n|=4;
				if (*(data+size/2+size/4+1)&mask)  n|=8;
				dwval|=n<<(28-j*4);
				penusage=1<<n;
				cps1_char_pen_usage[nchar]|=penusage;
				cps1_tile16_pen_usage[nchar/2]|=penusage;
				cps1_tile32_pen_usage[nchar/8]|=penusage;
		   }
		   CPS1_GFX_OUT(2*i)=dwval;
		   dwval=0;
		   for (j=0; j<8; j++)
		   {
				int n,mask;
				n=0;
				mask=0x80>>j;
				if (*(data)&mask)	  n|=1;
				if (*(data+1)&mask)	n|=2;
				if (*(data+size/2)&mask)   n|=4;
				if (*(data+size/2+1)&mask) n|=8;
				dwval|=n<<(28-j*4);
				penusage=1<<n;
				cps1_char_pen_usage[nchar]|=penusage;
				cps1_tile16_pen_usage[nchar/2]|=penusage;
				cps1_tile32_pen_usage[nchar/8]|=penusage;
		   }
		   CPS1_GFX_OUT(2*i+1)=dwval;
		   data+=2;
#ifdef MAMEGO
		   if (cps1_gfx_flash && !((2*i+2) % CPS1_CHUNK_WORDS))
		   {
				/* chunk full: into flash, contiguous with the previous ones */
				extern unsigned char *mamego_flash_store(const unsigned char *data, size_t len, size_t *offset);
				size_t before = cps1_gfx_offset;
				uint32_t *p = (uint32_t*)mamego_flash_store((unsigned char*)cps1_gfx_chunk, CPS1_CHUNK_WORDS*4, &cps1_gfx_offset);
				if (!p || (cps1_gfx && p != cps1_gfx + (2*i+2 - CPS1_CHUNK_WORDS)))
				{
					/* no room (or not contiguous): start again in PSRAM */
					cps1_gfx_offset = before;
					free(cps1_gfx_chunk);
					cps1_gfx_chunk = 0;
					cps1_gfx_flash = 0;
					cps1_gfx = 0;
					memset(cps1_char_pen_usage, 0, cps1_max_char*sizeof(cps1_pen_t));
					memset(cps1_tile16_pen_usage, 0, cps1_max_tile16*sizeof(cps1_pen_t));
					memset(cps1_tile32_pen_usage, 0, cps1_max_tile32*sizeof(cps1_pen_t));
					logerror("cps1: tiles do not fit the flash partition, using PSRAM\n");
					cps1_gfx=(uint32_t*)malloc(gfxsize*sizeof(uint32_t));
					if (!cps1_gfx)
						return -1;
					data = memory_region(REGION_GFX1);
					i = -1;
					continue;
				}
				if (!cps1_gfx)
					cps1_gfx = p;
		   }
#endif
		}
	}
#ifdef MAMEGO
	if (cps1_gfx_flash)
	{
		extern size_t mamego_flash_offset;
		mamego_flash_offset = cps1_gfx_offset;
		free(cps1_gfx_chunk);
		cps1_gfx_chunk = 0;
		printf("cps1: %d KB of decoded tiles served from flash\n", gfxsize*4/1024);
	}
free_rom:
	{
		/* the ROM is not read again: free it now rather than after vh_start
		   (REGIONFLAG_DISPOSE), before the 2 MB scroll2 bitmap is allocated */
		int r;
		for (r = 0; r < MAX_MEMORY_REGIONS; r++)
			if ((Machine->memory_region_type[r] & ~REGIONFLAG_MASK) == REGION_GFX1 && Machine->memory_region[r])
			{
				MAMEGO_REGION_FREE(r);
				Machine->memory_region[r] = 0;
				Machine->memory_region_length[r] = 0;
			}
	}
#endif
	return 0;
}

void cps1_gfx_stop(void)
{
#ifdef MAMEGO
	if (cps1_gfx_flash)
		cps1_gfx = 0; /* in flash */
	cps1_gfx_flash = 0;
	if (cps1_gfx_hi)
		free(cps1_gfx_hi);
	cps1_gfx_hi = 0;
	cps1_gfx_split = ~0u;
	cps1gfx_rom = 0;
#endif
	if (cps1_gfx)
	{
		free(cps1_gfx);
	}
	if (cps1_char_pen_usage)
	{
		free(cps1_char_pen_usage);
	}
	if (cps1_tile16_pen_usage)
	{
		free(cps1_tile16_pen_usage);
	}
	if (cps1_tile32_pen_usage)
	{
		free(cps1_tile32_pen_usage);
	}
}


#ifdef MAMEGO
void cps1_draw_gfx_opaque(
	struct osd_bitmap *dest,const struct GfxElement *gfx,
	unsigned int code,
	int color,
	int flipx,int flipy,
	int sx,int sy,
	int tpens,
	cps1_pen_t *pusage,
	const int size,
	const int max,
	const int delta,
	const int srcdelta);
void cps1_draw_gfx_opaque16(
	struct osd_bitmap *dest,const struct GfxElement *gfx,
	unsigned int code,
	int color,
	int flipx,int flipy,
	int sx,int sy,
	int tpens,
	cps1_pen_t *pusage,
	const int size,
	const int max,
	const int delta,
	const int srcdelta);
#endif

void cps1_draw_gfx(
	struct osd_bitmap *dest,const struct GfxElement *gfx,
	unsigned int code,
	int color,
	int flipx,int flipy,
	int sx,int sy,
	int tpens,
	cps1_pen_t *pusage,
	const int size,
	const int max,
	const int delta,
	const int srcdelta)
{
#ifdef MAMEGO
	/* a tile that uses no transparent pen needs no per-pixel test */
	if (code < max && (pusage[code] & ~tpens & 0xffff) == 0)
	{
		cps1_draw_gfx_opaque(dest,gfx,code,color,flipx,flipy,sx,sy,tpens,pusage,size,max,delta,srcdelta);
		return;
	}
#endif
	#define DATATYPE unsigned char
	#define IF_NOT_TRANSPARENT(n,x,y) if (tpens & (0x01 << n))
	#define SELF_INCLUDE
	#include "cps1.c"
	#undef SELF_INCLUDE
	#undef DATATYPE
	#undef IF_NOT_TRANSPARENT
}

void cps1_draw_gfx16(
	struct osd_bitmap *dest,const struct GfxElement *gfx,
	unsigned int code,
	int color,
	int flipx,int flipy,
	int sx,int sy,
	int tpens,
	cps1_pen_t *pusage,
	const int size,
	const int max,
	const int delta,
	const int srcdelta)
{
#ifdef MAMEGO
	/* a tile that uses no transparent pen needs no per-pixel test */
	if (code < max && (pusage[code] & ~tpens & 0xffff) == 0)
	{
		cps1_draw_gfx_opaque16(dest,gfx,code,color,flipx,flipy,sx,sy,tpens,pusage,size,max,delta,srcdelta);
		return;
	}
#endif
	#define DATATYPE unsigned short
	#define IF_NOT_TRANSPARENT(n,x,y) if (tpens & (0x01 << n))
	#define SELF_INCLUDE
	#include "cps1.c"
	#undef SELF_INCLUDE
	#undef DATATYPE
	#undef IF_NOT_TRANSPARENT
}

void cps1_draw_gfx_pri(
	struct osd_bitmap *dest,const struct GfxElement *gfx,
	unsigned int code,
	int color,
	int flipx,int flipy,
	int sx,int sy,
	int tpens,
	cps1_pen_t *pusage,
	const int size,
	const int max,
	const int delta,
	const int srcdelta,
	struct osd_bitmap *pribm)
{
	#define DATATYPE unsigned char
	#define IF_NOT_TRANSPARENT(n,x,y) if ((tpens & (0x01 << n)) && pribm->line[y][x] == 0)
	#define SELF_INCLUDE
	#include "cps1.c"
	#undef SELF_INCLUDE
	#undef DATATYPE
	#undef IF_NOT_TRANSPARENT
}

void cps1_draw_gfx16_pri(
	struct osd_bitmap *dest,const struct GfxElement *gfx,
	unsigned int code,
	int color,
	int flipx,int flipy,
	int sx,int sy,
	int tpens,
	cps1_pen_t *pusage,
	const int size,
	const int max,
	const int delta,
	const int srcdelta,
	struct osd_bitmap *pribm)
{
	#define DATATYPE unsigned short
	#define IF_NOT_TRANSPARENT(n,x,y) if ((tpens & (0x01 << n)) && pribm->line[y][x] == 0)
	#define SELF_INCLUDE
	#include "cps1.c"
	#undef SELF_INCLUDE
	#undef DATATYPE
	#undef IF_NOT_TRANSPARENT
}

/*

This is an optimized version that doesn't take into account transparency

Draws complete tiles without checking transparency. Used for scroll 2 low
priority rendering.

*/
void cps1_draw_gfx_opaque(
	struct osd_bitmap *dest,const struct GfxElement *gfx,
	unsigned int code,
	int color,
	int flipx,int flipy,
	int sx,int sy,
	int tpens,
	cps1_pen_t *pusage,
	const int size,
	const int max,
	const int delta,
	const int srcdelta)
{
	#define DATATYPE unsigned char
	#define IF_NOT_TRANSPARENT(n,x,y)
	#define DW_OPAQUE
	#define SELF_INCLUDE
	#include "cps1.c"
	#undef SELF_INCLUDE
	#undef DW_OPAQUE
	#undef DATATYPE
	#undef IF_NOT_TRANSPARENT
}

void cps1_draw_gfx_opaque16(
	struct osd_bitmap *dest,const struct GfxElement *gfx,
	unsigned int code,
	int color,
	int flipx,int flipy,
	int sx,int sy,
	int tpens,
	cps1_pen_t *pusage,
	const int size,
	const int max,
	const int delta,
	const int srcdelta)
{
	#define DATATYPE unsigned short
	#define IF_NOT_TRANSPARENT(n,x,y)
	#define DW_OPAQUE
	#define SELF_INCLUDE
	#include "cps1.c"
	#undef SELF_INCLUDE
	#undef DW_OPAQUE
	#undef DATATYPE
	#undef IF_NOT_TRANSPARENT
}

static INLINE void cps1_draw_scroll1(
	struct osd_bitmap *dest,
	unsigned int code, int color,
	int flipx, int flipy,int sx, int sy, int tpens)
{
	if (dest->depth==16)
	{
		cps1_draw_gfx16(dest,
			Machine->gfx[0],
			code,color,flipx,flipy,sx,sy,
			tpens,cps1_char_pen_usage,8, cps1_max_char, 16, 1);
	}
	else
	{
		cps1_draw_gfx(dest,
			Machine->gfx[0],
			code,color,flipx,flipy,sx,sy,
			tpens,cps1_char_pen_usage,8, cps1_max_char, 16, 1);
	}
}

static INLINE void cps1_draw_tile16(struct osd_bitmap *dest,
	const struct GfxElement *gfx,
	unsigned int code, int color,
	int flipx, int flipy,int sx, int sy, int tpens)
{
	if (dest->depth==16)
	{
		cps1_draw_gfx16(dest,
			gfx,
			code,color,flipx,flipy,sx,sy,
			tpens,cps1_tile16_pen_usage,16, cps1_max_tile16, 16*2,0);
	}
	else
	{
		cps1_draw_gfx(dest,
			gfx,
			code,color,flipx,flipy,sx,sy,
			tpens,cps1_tile16_pen_usage,16, cps1_max_tile16, 16*2,0);
	}
}

static INLINE void cps1_draw_tile16_pri(struct osd_bitmap *dest,
	const struct GfxElement *gfx,
	unsigned int code, int color,
	int flipx, int flipy,int sx, int sy, int tpens)
{
	if (dest->depth==16)
	{
		cps1_draw_gfx16_pri(dest,
			gfx,
			code,color,flipx,flipy,sx,sy,
			tpens,cps1_tile16_pen_usage,16, cps1_max_tile16, 16*2,0, priority_bitmap);
	}
	else
	{
		cps1_draw_gfx_pri(dest,
			gfx,
			code,color,flipx,flipy,sx,sy,
			tpens,cps1_tile16_pen_usage,16, cps1_max_tile16, 16*2,0, priority_bitmap);
	}
}

static INLINE void cps1_draw_tile32(struct osd_bitmap *dest,
	const struct GfxElement *gfx,
	unsigned int code, int color,
	int flipx, int flipy,int sx, int sy, int tpens)
{
	if (dest->depth==16)
	{
		cps1_draw_gfx16(dest,
			gfx,
			code,color,flipx,flipy,sx,sy,
			tpens,cps1_tile32_pen_usage,32, cps1_max_tile32, 16*2*4,0);
	}
	else
	{
		cps1_draw_gfx(dest,
			gfx,
			code,color,flipx,flipy,sx,sy,
			tpens,cps1_tile32_pen_usage,32, cps1_max_tile32, 16*2*4,0);
	}
}

static INLINE void cps1_draw_blank16(struct osd_bitmap *dest, int sx, int sy )
{
	int i,j;

	if (Machine->orientation & ORIENTATION_SWAP_XY)
	{
		int temp;
		temp=sx;
		sx=sy;
		sy=dest->height-temp-16;
	}

	if (cps1_flip_screen)
	{
		/* Handle flipped screen */
		sx=dest->width-sx-16;
		sy=dest->height-sy-16;
	}

	if (dest->depth==16)
	{
		for (i=15; i>=0; i--)
		{
			register unsigned short *bm=(unsigned short *)dest->line[sy+i]+sx;
			for (j=15; j>=0; j--)
			{
				*bm=palette_transparent_pen;
				bm++;
			}
		}
	}
	else
	{
		for (i=15; i>=0; i--)
		{
			register unsigned char *bm=dest->line[sy+i]+sx;
			for (j=15; j>=0; j--)
			{
				*bm=palette_transparent_pen;
				bm++;
			}
		}
	}
}

static INLINE void cps1_draw_tile16_bmp(struct osd_bitmap *dest,
	const struct GfxElement *gfx,
	unsigned int code, int color,
	int flipx, int flipy,int sx, int sy)
{
	if (dest->depth==16)
	{
		cps1_draw_gfx_opaque16(dest,
			gfx,
			code,color,flipx,flipy,sx,sy,
			-1,cps1_tile16_pen_usage,16, cps1_max_tile16, 16*2,0);
	}
	else
	{
		cps1_draw_gfx_opaque(dest,
			gfx,
			code,color,flipx,flipy,sx,sy,
			-1,cps1_tile16_pen_usage,16, cps1_max_tile16, 16*2,0);
	}
}




static int cps1_transparency_scroll[4];



#if CPS1_DUMP_VIDEO
void cps1_dump_video(void)
{
	FILE *fp;
	fp=fopen("SCROLL1.DMP", "w+b");
	if (fp)
	{
		fwrite(cps1_scroll1, cps1_scroll1_size, 1, fp);
		fclose(fp);
	}
	fp=fopen("SCROLL2.DMP", "w+b");
	if (fp)
	{
		fwrite(cps1_scroll2, cps1_scroll2_size, 1, fp);
		fclose(fp);
	}
	fp=fopen("SCROLL3.DMP", "w+b");
	if (fp)
	{
		fwrite(cps1_scroll3, cps1_scroll3_size, 1, fp);
		fclose(fp);
	}
	fp=fopen("OBJ.DMP", "w+b");
	if (fp)
	{
		fwrite(cps1_obj, cps1_obj_size, 1, fp);
		fclose(fp);
	}

	fp=fopen("OTHER.DMP", "w+b");
	if (fp)
	{
		fwrite(cps1_other, cps1_other_size, 1, fp);
		fclose(fp);
	}

	fp=fopen("PALETTE.DMP", "w+b");
	if (fp)
	{
		fwrite(cps1_palette, cps1_palette_size, 1, fp);
		fclose(fp);
	}

	fp=fopen("OUTPUT.DMP", "w+b");
	if (fp)
	{
		fwrite(cps1_output, cps1_output_size, 1, fp);
		fclose(fp);
	}
	fp=fopen("VIDEO.DMP", "w+b");
	if (fp)
	{
		fwrite(cps1_gfxram, cps1_gfxram_size, 1, fp);
		fclose(fp);
	}

}
#endif


static INLINE void cps1_get_video_base(void )
{
	int layercontrol;

	/* Re-calculate the VIDEO RAM base */
	cps1_scroll1=cps1_base(CPS1_SCROLL1_BASE,cps1_scroll1_size);
	cps1_scroll2=cps1_base(CPS1_SCROLL2_BASE,cps1_scroll2_size);
	cps1_scroll3=cps1_base(CPS1_SCROLL3_BASE,cps1_scroll3_size);
	cps1_obj=cps1_base(CPS1_OBJ_BASE,cps1_obj_size);
	cps1_palette=cps1_base(CPS1_PALETTE_BASE,cps1_palette_size);
	cps1_other=cps1_base(CPS1_OTHER_BASE,cps1_other_size);

	/* Get scroll values */
	scroll1x=cps1_port(CPS1_SCROLL1_SCROLLX);
	scroll1y=cps1_port(CPS1_SCROLL1_SCROLLY);
	scroll2x=cps1_port(CPS1_SCROLL2_SCROLLX);
	scroll2y=cps1_port(CPS1_SCROLL2_SCROLLY);
	scroll3x=cps1_port(CPS1_SCROLL3_SCROLLX);
	scroll3y=cps1_port(CPS1_SCROLL3_SCROLLY);

	/* Get transparency registers */
	if (cps1_game_config->priority1)
	{
		cps1_transparency_scroll[0]=cps1_port(cps1_game_config->priority0);
		cps1_transparency_scroll[1]=cps1_port(cps1_game_config->priority1);
		cps1_transparency_scroll[2]=cps1_port(cps1_game_config->priority2);
		cps1_transparency_scroll[3]=cps1_port(cps1_game_config->priority3);
	}

	/* Get layer enable bits */
	layercontrol=cps1_port(cps1_game_config->layer_control);
	cps1_layer_enabled[0]=1;
	cps1_layer_enabled[1]=layercontrol & cps1_game_config->scrl1_enable_mask;
	cps1_layer_enabled[2]=layercontrol & cps1_game_config->scrl2_enable_mask;
	cps1_layer_enabled[3]=layercontrol & cps1_game_config->scrl3_enable_mask;
}


/***************************************************************************

  Start the video hardware emulation.

***************************************************************************/

int cps1_vh_start(void)
{
	int i;

#ifdef MAMEGO
	{
		/* generic idle-loop skip (m68kcpu.c, z80.c), as for the Neo Geo:
		   I/O is the 0x800000 block (inputs, sound latch, CPS-A/B registers) */
		extern unsigned int m68ki_idle_enable, m68ki_idle_io_lo, m68ki_idle_io_hi;
		extern unsigned z80_idle_enable;
		extern size_t (*mamego_driver_state)(unsigned char *, size_t, int);
		extern size_t cps1_mamego_state(unsigned char *buf, size_t size, int mode);
		/* exact save states on the YM2151 boards (drivers/cps1.c) */
		if (Machine->drv->sound[0].sound_type == SOUND_YM2151)
			mamego_driver_state = cps1_mamego_state;
		m68ki_idle_enable = 1;
		m68ki_idle_io_lo = 0x800000;
		m68ki_idle_io_hi = 0x8fffff;
		/* not the Z80: it writes the YM2151 / OKI through memory, not OUT, so
		   a loop feeding them can look idle (audio changed on the PC) */
		z80_idle_enable = 0;
#ifndef ESP_PLATFORM
		if (getenv("M68KIDLE") && !strcmp(getenv("M68KIDLE"), "0")) m68ki_idle_enable = 0;
#endif
	}
#endif
	cps1_init_machine();

	if (cps1_gfx_start())
	{
		return -1;
	}

	cps1_scroll2_bitmap=bitmap_alloc(CPS1_SCROLL2_WIDTH*16,CPS1_SCROLL2_HEIGHT*16);
	if (!cps1_scroll2_bitmap)
	{
		return -1;
	}
	cps1_scroll2_old=(unsigned char*)malloc(cps1_scroll2_size);
	if (!cps1_scroll2_old)
	{
		return -1;
	}
	memset(cps1_scroll2_old, 0xff, cps1_scroll2_size);


	cps1_old_palette=(unsigned char *)malloc(cps1_palette_size);
	if (!cps1_old_palette)
	{
		return -1;
	}
	memset(cps1_old_palette, 0x00, cps1_palette_size);
	for (i = 0;i < cps1_palette_entries*16;i++)
	{
	   palette_change_color(i,0,0,0);
	}

	cps1_buffered_obj = (unsigned char*)malloc(cps1_obj_size);
	if (!cps1_buffered_obj)
	{
		return -1;
	}
	memset(cps1_buffered_obj, 0x00, cps1_obj_size);

	memset(cps1_gfxram, 0, cps1_gfxram_size);   /* Clear GFX RAM */
	memset(cps1_output, 0, cps1_output_size);   /* Clear output ports */

	/* Put in some defaults */
	WRITE_WORD(&cps1_output[0x00], 0x9200);
	WRITE_WORD(&cps1_output[0x02], 0x9000);
	WRITE_WORD(&cps1_output[0x04], 0x9040);
	WRITE_WORD(&cps1_output[0x06], 0x9080);
	WRITE_WORD(&cps1_output[0x08], 0x9100);
	WRITE_WORD(&cps1_output[0x0a], 0x90c0);

	if (!cps1_game_config)
	{
		//logerror("cps1_game_config hasn't been set up yet");
		return -1;
	}


	/* Set up old base */
	cps1_get_video_base();   /* Calculate base pointers */
	cps1_get_video_base();   /* Calculate old base pointers */


	for (i=0; i<4; i++)
	{
		cps1_transparency_scroll[i]=0x0000;
	}
	return 0;
}

#ifdef MAMEGO
/* mame-go save state (drivers/cps1.c): the sprite list buffered at the last
   vblank and the flip state; on load the palette and scroll 2 caches are
   rebuilt from video RAM */
size_t cps1_mamego_video_state(unsigned char *buf, size_t size, int mode) /* 0 size, 1 save, 2 load */
{
	size_t pos = 0, len = cps1_buffered_obj ? cps1_obj_size : 0;
	int i;
	if (mode && pos + len + sizeof(cps1_flip_screen) > size)
		return 0;
	if (mode == 1)
	{
		memcpy(buf, cps1_buffered_obj, len);
		memcpy(buf + len, &cps1_flip_screen, sizeof(cps1_flip_screen));
	}
	else if (mode == 2)
	{
		memcpy(cps1_buffered_obj, buf, len);
		memcpy(&cps1_flip_screen, buf + len, sizeof(cps1_flip_screen));
		cps1_get_video_base();
		if (cps1_old_palette && cps1_palette)
			for (i = 0; i < cps1_palette_size; i += 2)
				WRITE_WORD(&cps1_old_palette[i], ~READ_WORD(&cps1_palette[i]));
		if (cps1_scroll2_old)
			memset(cps1_scroll2_old, 0xff, cps1_scroll2_size);
	}
	return len + sizeof(cps1_flip_screen);
}
#endif

/***************************************************************************

  Stop the video hardware emulation.

***************************************************************************/
void cps1_vh_stop(void)
{
#ifdef MAMEGO
	{ extern void cps1snd_disable(void); cps1snd_disable(); }
	{ extern size_t (*mamego_driver_state)(unsigned char *, size_t, int); mamego_driver_state = 0; }
	{ extern unsigned int m68ki_idle_enable; extern unsigned z80_idle_enable; m68ki_idle_enable = 0; z80_idle_enable = 0; }
#endif
	if (cps1_old_palette)
		free(cps1_old_palette);
	if (cps1_scroll2_bitmap)
		bitmap_free(cps1_scroll2_bitmap);
	if (cps1_scroll2_old)
		free(cps1_scroll2_old);
	if (cps1_buffered_obj)
		free(cps1_buffered_obj);
	cps1_gfx_stop();
}

/***************************************************************************

  Build palette from palette RAM

  12 bit RGB with a 4 bit brightness value.

***************************************************************************/

void cps1_build_palette(void)
{
	int offset;

	for (offset = 0; offset < cps1_palette_entries*16; offset++)
	{
		int palette = READ_WORD (&cps1_palette[offset * 2]);

		if (palette != READ_WORD (&cps1_old_palette[offset * 2]) )
		{
		   int red, green, blue, bright;

		   bright= (palette>>12);
		   if (bright) bright += 2;

		   red   = ((palette>>8)&0x0f) * bright;
		   green = ((palette>>4)&0x0f) * bright;
		   blue  = (palette&0x0f) * bright;

		   palette_change_color (offset, red, green, blue);
		   WRITE_WORD(&cps1_old_palette[offset * 2], palette);
		}
	}
}

/***************************************************************************

  Scroll 1 (8x8)

  Attribute word layout:
  0x0001	colour
  0x0002	colour
  0x0004	colour
  0x0008	colour
  0x0010	colour
  0x0020	X Flip
  0x0040	Y Flip
  0x0080
  0x0100
  0x0200
  0x0400
  0x0800
  0x1000
  0x2000
  0x4000
  0x8000


***************************************************************************/

static INLINE void cps1_palette_scroll1(unsigned short *base)
{
	int x,y, offs, offsx;

	int scrlxrough=(scroll1x>>3)+8;
	int scrlyrough=(scroll1y>>3);
	int basecode=cps1_game_config->bank_scroll1*0x08000;

	for (x=0; x<0x36; x++)
	{
		 offsx=(scrlxrough+x)*0x80;
		 offsx&=0x1fff;

		 for (y=0; y<0x20; y++)
		 {
			int code, colour, offsy;
			int n=scrlyrough+y;
			offsy=( (n&0x1f)*4 | ((n&0x20)*0x100)) & 0x3fff;
			offs=offsy+offsx;
			offs &= 0x3fff;
			code=basecode+READ_WORD(&cps1_scroll1[offs]);
			colour=READ_WORD(&cps1_scroll1[offs+2]);
			if (code < cps1_max_char)
			{
				base[colour&0x1f] |=
					  cps1_char_pen_usage[code]&0x7fff;
			}
		}
	}
}

void cps1_render_scroll1(struct osd_bitmap *bitmap,int priority)
{
	int x,y, offs, offsx, sx, sy, ytop;

	int scrlxrough=(scroll1x>>3)+4;
	int scrlyrough=(scroll1y>>3);
	int base=cps1_game_config->bank_scroll1*0x08000;
	const int spacechar=cps1_game_config->space_scroll1;


	sx=-(scroll1x&0x07);
	ytop=-(scroll1y&0x07)+32;

	for (x=0; x<0x35; x++)
	{
		 sy=ytop;
		 offsx=(scrlxrough+x)*0x80;
		 offsx&=0x1fff;

		 for (y=0; y<0x20; y++)
		 {
			int code, offsy, colour;
			int n=scrlyrough+y;
			offsy=( (n&0x1f)*4 | ((n&0x20)*0x100)) & 0x3fff;
			offs=offsy+offsx;
			offs &= 0x3fff;

			code  =READ_WORD(&cps1_scroll1[offs]);
			colour=READ_WORD(&cps1_scroll1[offs+2]);

			if (code != 0x20 && code != spacechar)
			{
				int transp;

				/* 0x0020 appears to never be drawn */
				if (priority)
				{
					transp=cps1_transparency_scroll[(colour & 0x0180)>>7];
					cps1_draw_scroll1(priority_bitmap,
							code+base,
							colour&0x1f,
							colour&0x20,
							colour&0x40,
							sx,sy,transp);
				}
				else
				{
					transp = 0x7fff;
					cps1_draw_scroll1(bitmap,
							code+base,
							colour&0x1f,
							colour&0x20,
							colour&0x40,
							sx,sy,transp);
				}
			 }
			 sy+=8;
		 }
		 sx+=8;
	}
}



/***************************************************************************

								Sprites
								=======

  Sprites are represented by a number of 8 byte values

  xx xx yy yy nn nn aa aa

  where xxxx = x position
		yyyy = y position
		nnnn = tile number
		aaaa = attribute word
					0x0001	colour
					0x0002	colour
					0x0004	colour
					0x0008	colour
					0x0010	colour
					0x0020	X Flip
					0x0040	Y Flip
					0x0080	unknown
					0x0100	X block size (in sprites)
					0x0200	X block size
					0x0400	X block size
					0x0800	X block size
					0x1000	Y block size (in sprites)
					0x2000	Y block size
					0x4000	Y block size
					0x8000	Y block size

  The end of the table (may) be marked by an attribute value of 0xff00.

***************************************************************************/

void cps1_find_last_sprite(void)    /* Find the offset of last sprite */
{
	int offset=6;
	/* Locate the end of table marker */
	while (offset < cps1_obj_size)
	{
		int colour=READ_WORD(&cps1_buffered_obj[offset]);
		if (colour == 0xff00)
		{
			/* Marker found. This is the last sprite. */
			cps1_last_sprite_offset=offset-6-8;
			return;
		}
		offset+=8;
	}
	/* Sprites must use full sprite RAM */
	cps1_last_sprite_offset=cps1_obj_size-8;
}

/* Find used colours */
void cps1_palette_sprites(unsigned short *base)
{
	int i;

	for (i=cps1_last_sprite_offset; i>=0; i-=8)
	{
		int x=READ_WORD(&cps1_buffered_obj[i]);
		int y=READ_WORD(&cps1_buffered_obj[i+2]);
		if (x && y)
		{
			int colour=READ_WORD(&cps1_buffered_obj[i+6]);
			int col=colour&0x1f;
			unsigned int code=READ_WORD(&cps1_buffered_obj[i+4]);
			if (cps1_game_config->kludge == 7)
			{
				code += 0x4000;
			}
			if (cps1_game_config->kludge == 1 && code >= 0x01000)
			{
				code += 0x4000;
			}
			if (cps1_game_config->kludge == 2 && code >= 0x02a00)
			{
				code += 0x4000;
			}

			if ( colour & 0xff00 )
			{
				int nys, nxs;
				int nx=(colour & 0x0f00) >> 8;
				int ny=(colour & 0xf000) >> 12;
				nx++;
				ny++;

				if (colour & 0x40)   /* Y Flip */					      /* Y flip */
				{
					if (colour &0x20)
					{
					for (nys=0; nys<ny; nys++)
					{
						for (nxs=0; nxs<nx; nxs++)
						{
							int cod=code+(nx-1)-nxs+0x10*(ny-1-nys);
							base[col] |=
							cps1_tile16_pen_usage[cod % cps1_max_tile16];
						}
					}
				}
				else
				{
					for (nys=0; nys<ny; nys++)
					{
						for (nxs=0; nxs<nx; nxs++)
						{
							int cod=code+nxs+0x10*(ny-1-nys);
							base[col] |=
							cps1_tile16_pen_usage[cod % cps1_max_tile16];
						}
					}
				}
			}
			else
			{
				if (colour &0x20)
				{
					for (nys=0; nys<ny; nys++)
					{
						for (nxs=0; nxs<nx; nxs++)
						{
							int cod=code+(nx-1)-nxs+0x10*nys;
							base[col] |=
							cps1_tile16_pen_usage[cod % cps1_max_tile16];
						}
					}
				}
				else
				{
					for (nys=0; nys<ny; nys++)
					{
						for (nxs=0; nxs<nx; nxs++)
						{
							int cod=code+nxs+0x10*nys;
							base[col] |=
							cps1_tile16_pen_usage[cod % cps1_max_tile16];
						}
					}
				}
			}
			base[col]&=0x7fff;
			}
			else
			{
				base[col] |=
				cps1_tile16_pen_usage[code % cps1_max_tile16]&0x7fff;
			}
		}
	}
}



void cps1_render_sprites(struct osd_bitmap *bitmap)
{
	const int mask=0x7fff;
	int i;
//mish
	/* Draw the sprites */
	for (i=cps1_last_sprite_offset; i>=0; i-=8)
	{
		int x=READ_WORD(&cps1_buffered_obj[i]);
		int y=READ_WORD(&cps1_buffered_obj[i+2]);
		if (x && y )
		{
			unsigned int code=READ_WORD(&cps1_buffered_obj[i+4]);
			int colour=READ_WORD(&cps1_buffered_obj[i+6]);
			int col=colour&0x1f;

			y &= 0x1ff;
			if (y > 450) y -= 0x200;

			/* in cawing, skyscrapers parts on level 2 have all the top bits of the */
			/* x coordinate set. Does this have a special meaning? */
			x &= 0x1ff;
			if (x > 450) x -= 0x200;

			x-=0x20;
			y+=0x20;

			if (cps1_game_config->kludge == 7)
			{
				code += 0x4000;
			}
			if (cps1_game_config->kludge == 1 && code >= 0x01000)
			{
				code += 0x4000;
			}
			if (cps1_game_config->kludge == 2 && code >= 0x02a00)
			{
				code += 0x4000;
			}

			if (colour & 0xff00 )
			{
				/* handle blocked sprites */
				int nx=(colour & 0x0f00) >> 8;
				int ny=(colour & 0xf000) >> 12;
				int nxs,nys,sx,sy;
				nx++;
				ny++;

				if (colour & 0x40)
				{
					/* Y flip */
					if (colour &0x20)
					{
						for (nys=0; nys<ny; nys++)
						{
							for (nxs=0; nxs<nx; nxs++)
							{
								sx = x+nxs*16;
								sy = y+nys*16;
								if (sx > 450) sx -= 0x200;
								if (sy > 450) sy -= 0x200;

								cps1_draw_tile16_pri(bitmap,Machine->gfx[1],
									code+(nx-1)-nxs+0x10*(ny-1-nys),
									col&0x1f,
									1,1,
									sx,sy,mask);
							}
						}
					}
					else
					{
						for (nys=0; nys<ny; nys++)
						{
							for (nxs=0; nxs<nx; nxs++)
							{
								sx = x+nxs*16;
								sy = y+nys*16;
								if (sx > 450) sx -= 0x200;
								if (sy > 450) sy -= 0x200;

								cps1_draw_tile16_pri(bitmap,Machine->gfx[1],
									code+nxs+0x10*(ny-1-nys),
									col&0x1f,
									0,1,
									sx,sy,mask );
							}
						}
					}
				}
				else
				{
					if (colour &0x20)
					{
						for (nys=0; nys<ny; nys++)
						{
							for (nxs=0; nxs<nx; nxs++)
							{
								sx = x+nxs*16;
								sy = y+nys*16;
								if (sx > 450) sx -= 0x200;
								if (sy > 450) sy -= 0x200;

								cps1_draw_tile16_pri(bitmap,Machine->gfx[1],
									code+(nx-1)-nxs+0x10*nys,
									col&0x1f,
									1,0,
									sx,sy,mask
									);
							}
						}
					}
					else
					{
						for (nys=0; nys<ny; nys++)
						{
							for (nxs=0; nxs<nx; nxs++)
							{
								sx = x+nxs*16;
								sy = y+nys*16;
								if (sx > 450) sx -= 0x200;
								if (sy > 450) sy -= 0x200;

								cps1_draw_tile16_pri(bitmap,Machine->gfx[1],
									code+nxs+0x10*nys,
									col&0x1f,
									0,0,
									sx,sy, mask);
							}
						}
					}
				}
			}
			else
			{
				/* Simple case... 1 sprite */
				cps1_draw_tile16_pri(bitmap,Machine->gfx[1],
					   code,
					   col&0x1f,
					   colour&0x20,colour&0x40,
					   x,y,mask);
			}
		}
	}
}



/***************************************************************************

  Scroll 2 (16x16 layer)

  Attribute word layout:
  0x0001	colour
  0x0002	colour
  0x0004	colour
  0x0008	colour
  0x0010	colour
  0x0020	X Flip
  0x0040	Y Flip
  0x0080	??? Priority
  0x0100	??? Priority
  0x0200
  0x0400
  0x0800
  0x1000
  0x2000
  0x4000
  0x8000


***************************************************************************/

static INLINE void cps1_palette_scroll2(unsigned short *base)
{
	int offs, code, colour;
	int basecode=cps1_game_config->bank_scroll2*0x04000;

	for (offs=cps1_scroll2_size-4; offs>=0; offs-=4)
	{
		code=basecode+READ_WORD(&cps1_scroll2[offs]);
		colour=READ_WORD(&cps1_scroll2[offs+2])&0x1f;
		if (code < cps1_max_tile16)
		{
			base[colour] |= cps1_tile16_pen_usage[code];
		}
	}
}

void cps1_render_scroll2_bitmap(struct osd_bitmap *bitmap)
{
	int sx, sy;
	int ny=(scroll2y>>4);	  /* Rough Y */
	int base=cps1_game_config->bank_scroll2*0x04000;
	const int startcode=cps1_game_config->start_scroll2;
	const int endcode=cps1_game_config->end_scroll2;
	const int kludge=cps1_game_config->kludge;

	for (sx=CPS1_SCROLL2_WIDTH-1; sx>=0; sx--)
	{
		int n=ny;
		for (sy=0x09*2-1; sy>=0; sy--)
		{
			long newvalue;
			int offsy, offsx, offs, colour, code;

			n&=0x3f;
			offsy  = ((n&0x0f)*4 | ((n&0x30)*0x100))&0x3fff;
			offsx=(sx*0x040)&0xfff;
			offs=offsy+offsx;

			colour=READ_WORD(&cps1_scroll2[offs+2]);

			newvalue=*(long*)(&cps1_scroll2[offs]);
			if ( newvalue != *(long*)(&cps1_scroll2_old[offs]) )
			{
				*(long*)(&cps1_scroll2_old[offs])=newvalue;
				code=READ_WORD(&cps1_scroll2[offs]);
				if ( code >= startcode && code <= endcode
					/*
					MERCS has an gap in the scroll 2 layout
					(bad tiles at start of level 2)*/
					&&	!(kludge == 4 && (code >= 0x1e00 && code < 0x5400))
					)
				{
					code += base;
					cps1_draw_tile16_bmp(bitmap,
						Machine->gfx[2],
						code,
						colour&0x1f,
						colour&0x20,colour&0x40,
						16*sx, 16*n);
				}
				else
				{
					cps1_draw_blank16(bitmap, 16*sx, 16*n);
				}
				//cps1_print_debug_tile_info(bitmap, 16*sx, 16*n, colour,1);
			}
			n++;
		}
	}
}


void cps1_render_scroll2_high(struct osd_bitmap *bitmap)
{
#ifdef LAYER_DEBUG
	static int s=0;
#endif
	int sx, sy;
	int nxoffset=(scroll2x&0x0f)+32;    /* Smooth X */
	int nyoffset=(scroll2y&0x0f);    /* Smooth Y */
	int nx=(scroll2x>>4);	  /* Rough X */
	int ny=(scroll2y>>4)-4;	/* Rough Y */
	int base=cps1_game_config->bank_scroll2*0x04000;

	for (sx=0; sx<0x32/2+4; sx++)
	{
		for (sy=0; sy<0x09*2; sy++)
		{
			int offsy, offsx, offs, colour, code, transp;
			int n;
			n=ny+sy+2;
			offsy  = ((n&0x0f)*4 | ((n&0x30)*0x100))&0x3fff;
			offsx=((nx+sx)*0x040)&0xfff;
			offs=offsy+offsx;
			offs &= 0x3fff;

			code=READ_WORD(&cps1_scroll2[offs]);
			colour=READ_WORD(&cps1_scroll2[offs+2]);

			transp=cps1_transparency_scroll[(colour & 0x0180)>>7];

			cps1_draw_tile16(priority_bitmap,
						Machine->gfx[2],
						code+base,
						colour&0x1f,
						colour&0x20,colour&0x40,
						16*sx-nxoffset,
						16*sy-nyoffset,
						transp);
		}
	}
}

void cps1_render_scroll2_low(struct osd_bitmap *bitmap)
{
	  int scrly=-(scroll2y-0x20);
	  int scrlx=-(scroll2x+0x40-0x20);

	  if (cps1_flip_screen)
	  {
			scrly=(CPS1_SCROLL2_HEIGHT*16)-scrly;
	  }

	  cps1_render_scroll2_bitmap(cps1_scroll2_bitmap);

	  copyscrollbitmap(bitmap,cps1_scroll2_bitmap,1,&scrlx,1,&scrly,&Machine->visible_area,TRANSPARENCY_PEN,palette_transparent_pen);
}


void cps1_render_scroll2_distort(struct osd_bitmap *bitmap)
{
	int scrly=-scroll2y;
	int i,scrollx[1024];
	int otheroffs;

/*
	Games known to use row scrolling:

	SF2
	Mega Twins (underwater, cave)
	Carrier Air Wing (hazy background at beginning of mission 8)
	Magic Sword (fire on floor 3; screen distort after continue)
	Varth (title screen)
*/

	if (cps1_flip_screen)
		scrly=(CPS1_SCROLL2_HEIGHT*16)-scrly;

	cps1_render_scroll2_bitmap(cps1_scroll2_bitmap);

	otheroffs = cps1_port(CPS1_ROWSCROLL_OFFS);

	for (i = 0;i < 256;i++)
		scrollx[(i - scrly) & 0x3ff] = -(scroll2x+0x40-0x20) - READ_WORD(&cps1_other[(2*(i + otheroffs)) & 0x7ff]);

	scrly+=0x20;

	copyscrollbitmap(bitmap,cps1_scroll2_bitmap,1024,scrollx,1,&scrly,&Machine->visible_area,TRANSPARENCY_PEN,palette_transparent_pen);
}


/***************************************************************************

  Scroll 3 (32x32 layer)

  Attribute word layout:
  0x0001	colour
  0x0002	colour
  0x0004	colour
  0x0008	colour
  0x0010	colour
  0x0020	X Flip
  0x0040	Y Flip
  0x0080
  0x0100
  0x0200
  0x0400
  0x0800
  0x1000
  0x2000
  0x4000
  0x8000

***************************************************************************/

void cps1_palette_scroll3(unsigned short *base)
{
	int sx,sy;
	int nx=(scroll3x>>5)+1;
	int ny=(scroll3y>>5)-1;
	int basecode=cps1_game_config->bank_scroll3*0x01000;

	for (sx=0; sx<0x32/4+2; sx++)
	{
		for (sy=0; sy<0x20/4+2; sy++)
		{
			int offsy, offsx, offs, colour, code;
			int n;
			n=ny+sy;
			offsy  = ((n&0x07)*4 | ((n&0xf8)*0x0100))&0x3fff;
			offsx=((nx+sx)*0x020)&0x7ff;
			offs=offsy+offsx;
			offs &= 0x3fff;
			code=basecode+READ_WORD(&cps1_scroll3[offs]);
			if (cps1_game_config->kludge == 2 && code >= 0x01500)
			{
				code -= 0x1000;
			}
			colour=READ_WORD(&cps1_scroll3[offs+2]);
			if (code < cps1_max_tile32)
			{
				base[colour&0x1f] |= cps1_tile32_pen_usage[code];
			}
		}
	}
}


void cps1_render_scroll3(struct osd_bitmap *bitmap, int priority)
{
	int sx,sy;
	int nxoffset=scroll3x&0x1f;
	int nyoffset=scroll3y&0x1f;
	int nx=(scroll3x>>5)+1;
	int ny=(scroll3y>>5)-1;
	int basecode=cps1_game_config->bank_scroll3*0x01000;
	const int startcode=cps1_game_config->start_scroll3;
	const int endcode=cps1_game_config->end_scroll3;

	for (sx=1; sx<0x32/4+2; sx++)
	{
		for (sy=1; sy<0x20/4+2; sy++)
		{
			int offsy, offsx, offs, colour, code;
			int n;
			n=ny+sy;
			offsy  = ((n&0x07)*4 | ((n&0xf8)*0x0100))&0x3fff;
			offsx=((nx+sx)*0x020)&0x7ff;
			offs=offsy+offsx;
			offs &= 0x3fff;
			code=READ_WORD(&cps1_scroll3[offs]);
			if (code >= startcode && code <= endcode)
			{
				int transp;

				code+=basecode;
				if (cps1_game_config->kludge == 2 && code >= 0x01500)
				{
					   code -= 0x1000;
				}
				colour=READ_WORD(&cps1_scroll3[offs+2]);
				if (priority)
				{
					transp=cps1_transparency_scroll[(colour & 0x0180)>>7];
					cps1_draw_tile32(priority_bitmap,Machine->gfx[3],
							code,
							colour&0x1f,
							colour&0x20,colour&0x40,
							32*sx-nxoffset,32*sy-nyoffset,
							transp);
				}
				else
				{
					transp = 0x7fff;
					cps1_draw_tile32(bitmap,Machine->gfx[3],
							code,
							colour&0x1f,
							colour&0x20,colour&0x40,
							32*sx-nxoffset,32*sy-nyoffset,
							transp);
				}
			}
		}
	}
}



void cps1_render_layer(struct osd_bitmap *bitmap, int layer, int distort)
{
	if (cps1_layer_enabled[layer])
	{
		switch (layer)
		{
			case 0:
				cps1_render_sprites(bitmap);
				break;
			case 1:
				cps1_render_scroll1(bitmap, 0);
				break;
			case 2:
				if (distort)
					cps1_render_scroll2_distort(bitmap);
				else
					cps1_render_scroll2_low(bitmap);
				break;
			case 3:
				cps1_render_scroll3(bitmap, 0);
				break;
		}
	}
}

void cps1_render_high_layer(struct osd_bitmap *bitmap, int layer)
{
	if (cps1_layer_enabled[layer])
	{
		switch (layer)
		{
			case 0:
				/* there are no high priority sprites */
				break;
			case 1:
				cps1_render_scroll1(bitmap, 1);
				break;
			case 2:
				cps1_render_scroll2_high(bitmap);
				break;
			case 3:
				cps1_render_scroll3(bitmap, 1);
				break;
		}
	}
}


/***************************************************************************

	Refresh screen

***************************************************************************/

void cps1_vh_screenrefresh(struct osd_bitmap *bitmap,int full_refresh)
{
	unsigned short palette_usage[cps1_palette_entries];
	int layercontrol,l0,l1,l2,l3;
	int i,offset;
	int distort_scroll2=0;
	int videocontrol=cps1_port(0x22);
	int old_flip;


	old_flip=cps1_flip_screen;
	cps1_flip_screen=videocontrol&0x8000;
	if (old_flip != cps1_flip_screen)
	{
		 /* Mark all of scroll 2 as dirty */
		memset(cps1_scroll2_old, 0xff, cps1_scroll2_size);
	}

	layercontrol = READ_WORD(&cps1_output[cps1_game_config->layer_control]);

	distort_scroll2 = videocontrol & 0x01;

	/* Get video memory base registers */
	cps1_get_video_base();

	/* Find the offset of the last sprite in the sprite table */
	cps1_find_last_sprite();

	/* Build palette */
	cps1_build_palette();

	/* Compute the used portion of the palette */
	memset (palette_usage, 0, sizeof (palette_usage));
	cps1_palette_sprites (&palette_usage[cps1_obj_palette]);
	if (cps1_layer_enabled[1])
		cps1_palette_scroll1 (&palette_usage[cps1_scroll1_palette]);
	if (cps1_layer_enabled[2])
		cps1_palette_scroll2 (&palette_usage[cps1_scroll2_palette]);
	else
		memset(cps1_scroll2_old, 0xff, cps1_scroll2_size);
	if (cps1_layer_enabled[3])
		cps1_palette_scroll3 (&palette_usage[cps1_scroll3_palette]);

	for (i = offset = 0; i < cps1_palette_entries; i++)
	{
		int usage = palette_usage[i];
		if (usage)
		{
			int j;
			for (j = 0; j < 15; j++)
			{
				if (usage & (1 << j))
					palette_used_colors[offset++] = PALETTE_COLOR_USED;
				else
					palette_used_colors[offset++] = PALETTE_COLOR_UNUSED;
			}
			palette_used_colors[offset++] = PALETTE_COLOR_TRANSPARENT;
		}
		else
		{
			memset (&palette_used_colors[offset], PALETTE_COLOR_UNUSED, 16);
			offset += 16;
		}
	}

	if (palette_recalc ())
	{
		 /* Mark all of scroll 2 as dirty */
		memset(cps1_scroll2_old, 0xff, cps1_scroll2_size);
	}

	/* Blank screen */
//	fillbitmap(bitmap,palette_transparent_pen,&Machine->visible_area);
// TODO: the draw functions don't clip correctly at the sides of the screen, so
// for now let's clear the whole bitmap otherwise ctrl-f11 would show wrong counts
	fillbitmap(bitmap,palette_transparent_pen,0);


	/* Draw layers (0 = sprites, 1-3 = tilemaps) */
	l0 = (layercontrol >> 0x06) & 03;
	l1 = (layercontrol >> 0x08) & 03;
	l2 = (layercontrol >> 0x0a) & 03;
	l3 = (layercontrol >> 0x0c) & 03;

	fillbitmap(priority_bitmap,0,NULL);

	cps1_render_layer(bitmap,l0,distort_scroll2);
	if (l1 == 0) cps1_render_high_layer(bitmap,l0);	/* prepare mask for sprites */
	cps1_render_layer(bitmap,l1,distort_scroll2);
	if (l2 == 0) cps1_render_high_layer(bitmap,l1);	/* prepare mask for sprites */
	cps1_render_layer(bitmap,l2,distort_scroll2);
	if (l3 == 0) cps1_render_high_layer(bitmap,l2);	/* prepare mask for sprites */
	cps1_render_layer(bitmap,l3,distort_scroll2);

#if CPS1_DUMP_VIDEO
	if (keyboard_pressed(KEYCODE_F))
	{
		cps1_dump_video();
	}
#endif
}

void cps1_eof_callback(void)
{
	/* Get video memory base registers */
	cps1_get_video_base();

	/* Mish: 181099: Buffer sprites for next frame - the hardware must do
		this at the end of vblank */
	memcpy(cps1_buffered_obj,cps1_obj,cps1_obj_size);
}


#ifndef MAMEGO /* mame-go: CPS1 only (CPS2 sets need decryption keys and more memory) */
#include "cps2.c"
#endif



#else	/* SELF_INCLUDE */
/* don't put this file in the makefile, it is #included by cps1.c to */
/* generate 8-bit and 16-bit versions                                */

{
	int i, j;
	uint32_t dwval;
	uint32_t *src;
	const unsigned short *paldata;
	uint32_t n;
	DATATYPE *bm;

	/* max is the tile count: code == max is one past the end (harmless in
	   RAM, a fault past the end of the flash mapping on mame-go) */
	if ( code >= max || (tpens & pusage[code])==0)
	{
		/* Do not draw blank object */
		return;
	}

	if (Machine->orientation & ORIENTATION_SWAP_XY)
	{
		int temp;
		temp=sx;
		sx=sy;
		sy=dest->height-temp-size;
		temp=flipx;
		flipx=flipy;
		flipy=!temp;
	}

	if (cps1_flip_screen)
	{
		/* Handle flipped screen */
		flipx=!flipx;
		flipy=!flipy;
		sx=dest->width-sx-size;
		sy=dest->height-sy-size;
	}

	if (sx<0 || sx > dest->width-size || sy<0 || sy>dest->height-size )
	{
		/* Don't draw clipped tiles (for sprites) */
		return;
	}

	paldata=&gfx->colortable[gfx->color_granularity * color];
#ifdef MAMEGO
	/* 8 pixels all in the transparent pen draw nothing: CPS1 tiles are
	   mostly edges of that pen (15, sometimes 0) */
#ifdef DW_OPAQUE
#define DW_SKIP(d) 0
#else
	const int dwskip = !(tpens & 0x8000) || !(tpens & 1);
	const uint32_t dwskipval = !(tpens & 0x8000) ? 0xffffffffu : 0;
#define DW_SKIP(d) (dwskip && (d) == dwskipval)
#endif
	{
		/* streamed tiles: the first part in flash, the rest in PSRAM (a tile
		   never straddles: the split is a multiple of 64 KB) */
		uint32_t w = code*delta;
		src = w < cps1_gfx_split ? cps1_gfx + w : cps1_gfx_hi + (w - cps1_gfx_split);
	}
#else
	src = cps1_gfx+code*delta;
#define DW_SKIP(d) 0
#endif

	if (Machine->orientation & ORIENTATION_SWAP_XY)
	{
		int bmdelta,dir;

		bmdelta = (dest->line[1] - dest->line[0]);
		dir = 1;
		if (flipy)
		{
			bmdelta = -bmdelta;
			dir = -1;
			sy += size-1;
		}
		if (flipx) sx+=size-1;
		for (i=0; i<size; i++)
		{
			int ny=sy;
			for (j=0; j<size/8; j++)
			{
				dwval=*src;
				if (!DW_SKIP(dwval))
				{
					n=(dwval>>28)&0x0f;
					bm = (DATATYPE *)dest->line[ny]+sx;
					IF_NOT_TRANSPARENT(n,sx,ny) bm[0]=paldata[n];
					n=(dwval>>24)&0x0f;
					bm = (DATATYPE *)(((unsigned char *)bm) + bmdelta);
					IF_NOT_TRANSPARENT(n,sx,ny+dir) bm[0]=paldata[n];
					n=(dwval>>20)&0x0f;
					bm = (DATATYPE *)(((unsigned char *)bm) + bmdelta);
					IF_NOT_TRANSPARENT(n,sx,ny+2*dir) bm[0]=paldata[n];
					n=(dwval>>16)&0x0f;
					bm = (DATATYPE *)(((unsigned char *)bm) + bmdelta);
					IF_NOT_TRANSPARENT(n,sx,ny+3*dir) bm[0]=paldata[n];
					n=(dwval>>12)&0x0f;
					bm = (DATATYPE *)(((unsigned char *)bm) + bmdelta);
					IF_NOT_TRANSPARENT(n,sx,ny+4*dir) bm[0]=paldata[n];
					n=(dwval>>8)&0x0f;
					bm = (DATATYPE *)(((unsigned char *)bm) + bmdelta);
					IF_NOT_TRANSPARENT(n,sx,ny+5*dir) bm[0]=paldata[n];
					n=(dwval>>4)&0x0f;
					bm = (DATATYPE *)(((unsigned char *)bm) + bmdelta);
					IF_NOT_TRANSPARENT(n,sx,ny+6*dir) bm[0]=paldata[n];
					n=dwval&0x0f;
					bm = (DATATYPE *)(((unsigned char *)bm) + bmdelta);
					IF_NOT_TRANSPARENT(n,sx,ny+7*dir) bm[0]=paldata[n];
				}
				if (flipy) ny-=8;
				else ny+=8;
				src++;
			}
			if (flipx) sx--;
			else sx++;
			src+=srcdelta;
		}
	}
	else
	{
		if (flipy) sy+=size-1;
		if (flipx)
		{
			sx+=size;
			for (i=0; i<size; i++)
			{
				int x,y;
				x=sx;
				if (flipy) y=sy-i;
				else y=sy+i;
				bm=(DATATYPE *)dest->line[y]+sx;
				for (j=0; j<size/8; j++)
				{
					dwval=*src;
					if (!DW_SKIP(dwval))
					{
						n=(dwval>>28)&0x0f;
						IF_NOT_TRANSPARENT(n,x-1,y) bm[-1]=paldata[n];
						n=(dwval>>24)&0x0f;
						IF_NOT_TRANSPARENT(n,x-2,y) bm[-2]=paldata[n];
						n=(dwval>>20)&0x0f;
						IF_NOT_TRANSPARENT(n,x-3,y) bm[-3]=paldata[n];
						n=(dwval>>16)&0x0f;
						IF_NOT_TRANSPARENT(n,x-4,y) bm[-4]=paldata[n];
						n=(dwval>>12)&0x0f;
						IF_NOT_TRANSPARENT(n,x-5,y) bm[-5]=paldata[n];
						n=(dwval>>8)&0x0f;
						IF_NOT_TRANSPARENT(n,x-6,y) bm[-6]=paldata[n];
						n=(dwval>>4)&0x0f;
						IF_NOT_TRANSPARENT(n,x-7,y) bm[-7]=paldata[n];
						n=dwval&0x0f;
						IF_NOT_TRANSPARENT(n,x-8,y) bm[-8]=paldata[n];
					}
					bm-=8;
					x-=8;
					src++;
				}
				src+=srcdelta;
			}
		}
		else
		{
			for (i=0; i<size; i++)
			{
				int x,y;
				x=sx;
				if (flipy) y=sy-i;
				else y=sy+i;
				bm=(DATATYPE *)dest->line[y]+sx;
				for (j=0; j<size/8; j++)
				{
					dwval=*src;
					if (!DW_SKIP(dwval))
					{
						n=(dwval>>28)&0x0f;
						IF_NOT_TRANSPARENT(n,x+0,y) bm[0]=paldata[n];
						n=(dwval>>24)&0x0f;
						IF_NOT_TRANSPARENT(n,x+1,y) bm[1]=paldata[n];
						n=(dwval>>20)&0x0f;
						IF_NOT_TRANSPARENT(n,x+2,y) bm[2]=paldata[n];
						n=(dwval>>16)&0x0f;
						IF_NOT_TRANSPARENT(n,x+3,y) bm[3]=paldata[n];
						n=(dwval>>12)&0x0f;
						IF_NOT_TRANSPARENT(n,x+4,y) bm[4]=paldata[n];
						n=(dwval>>8)&0x0f;
						IF_NOT_TRANSPARENT(n,x+5,y) bm[5]=paldata[n];
						n=(dwval>>4)&0x0f;
						IF_NOT_TRANSPARENT(n,x+6,y) bm[6]=paldata[n];
						n=dwval&0x0f;
						IF_NOT_TRANSPARENT(n,x+7,y) bm[7]=paldata[n];
					}
					bm+=8;
					x+=8;
					src++;
				}
				src+=srcdelta;
			}
		}
	}
}
#undef DW_SKIP
#endif	/* SELF_INCLUDE */
