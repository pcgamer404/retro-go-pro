/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

// ESP32: the video menu only exposes what the handheld can actually use. There
// is one renderer (the software one) and one output size (320x240, see
// SWimp_SetMode), so the driver and video mode selectors of the PC version are
// gone: they only wrote cvars that did nothing, or worse, made the engine
// reload a renderer that does not exist on this port. What is left are the
// real knobs: the size of the 3D view (fewer pixels = faster), the palette
// brightness, the texture detail (higher mip level = fewer texels per span)
// and the water warp effect.

#include "../client/client.h"
#include "../client/qmenu.h"

extern cvar_t *vid_gamma;
extern cvar_t *scr_viewsize;
extern cvar_t *sw_mipcap;
extern cvar_t *sw_waterwarp;

extern void M_ForceMenuOff( void );

static menuframework_s s_video_menu;

static menuslider_s		s_screensize_slider;
static menuslider_s		s_brightness_slider;
static menulist_s		s_detail_box;
static menulist_s		s_waterwarp_box;
static menulist_s		s_sound_box;
static menuaction_s		s_defaults_action;
static menuaction_s		s_done_action;

// sw_mipcap: 1 = the sharpest level the texture cache holds, 3 = the smallest
// (8x8) level. Every step divides the texels read per span by four.
static const char *detail_names[] =
{
	"high",
	"medium",
	"low",
	0
};

static const char *onoff_names[] =
{
	"off",
	"on",
	0
};

/*
====================================================================

MENU INTERACTION

====================================================================
*/

static void ScreenSizeCallback( void *s )
{
	menuslider_s *slider = ( menuslider_s * ) s;

	Cvar_SetValue( "viewsize", slider->curvalue * 10 );
}

static void BrightnessCallback( void *s )
{
	menuslider_s *slider = ( menuslider_s * ) s;
	float gamma = ( 0.8 - ( slider->curvalue/10.0 - 0.5 ) ) + 0.5;

	Cvar_SetValue( "vid_gamma", gamma );
}

static void DetailCallback( void *s )
{
	menulist_s *box = ( menulist_s * ) s;

	// read every frame by the renderer (d_minmip), so it applies immediately
	Cvar_SetValue( "sw_mipcap", 1 + box->curvalue );
}

static void WaterWarpCallback( void *s )
{
	menulist_s *box = ( menulist_s * ) s;

	Cvar_SetValue( "sw_waterwarp", box->curvalue );
}

static void SoundCallback( void *s )
{
	menulist_s *box = ( menulist_s * ) s;

	// s_soundon off skips the mixer and the DMA feed entirely (see snd_dma.c)
	Cvar_SetValue( "s_soundon", box->curvalue );
}

static void ResetDefaults( void *unused )
{
	Cvar_SetValue( "viewsize", 100 );
	Cvar_SetValue( "vid_gamma", 1.0 );
	Cvar_SetValue( "sw_mipcap", 1 );
	Cvar_SetValue( "sw_waterwarp", 1 );
	Cvar_SetValue( "s_soundon", 1 );
	VID_MenuInit();
}

static void DoneAction( void *unused )
{
	M_ForceMenuOff();
}

/*
** VID_MenuInit
*/
void VID_MenuInit( void )
{
	if ( !scr_viewsize )
		scr_viewsize = Cvar_Get( "viewsize", "100", CVAR_ARCHIVE );
	if ( !sw_mipcap )
		sw_mipcap = Cvar_Get( "sw_mipcap", "1", CVAR_ARCHIVE );
	if ( !sw_waterwarp )
		sw_waterwarp = Cvar_Get( "sw_waterwarp", "1", CVAR_ARCHIVE );

	// Do not poke vid_ref here. Cvar_Set would mark it modified, and
	// VID_CheckChanges would tear down and reload the renderer inside a
	// running game (re.Shutdown/R_Init/Mod_Init), leaving the client with
	// freed world models. VID_CheckChanges ignores it once a renderer is
	// active.

	s_screensize_slider.generic.type			= MTYPE_SLIDER;
	s_screensize_slider.generic.x				= 0;
	s_screensize_slider.generic.y				= 0;
	s_screensize_slider.generic.name			= "3D view size";
	s_screensize_slider.generic.callback		= ScreenSizeCallback;
	s_screensize_slider.minvalue				= 3;
	s_screensize_slider.maxvalue				= 10;
	s_screensize_slider.curvalue				= scr_viewsize->value / 10;

	s_brightness_slider.generic.type			= MTYPE_SLIDER;
	s_brightness_slider.generic.x				= 0;
	s_brightness_slider.generic.y				= 10;
	s_brightness_slider.generic.name			= "brightness";
	s_brightness_slider.generic.callback		= BrightnessCallback;
	s_brightness_slider.minvalue				= 5;
	s_brightness_slider.maxvalue				= 13;
	s_brightness_slider.curvalue				= ( 1.3 - vid_gamma->value + 0.5 ) * 10;

	s_detail_box.generic.type					= MTYPE_SPINCONTROL;
	s_detail_box.generic.x						= 0;
	s_detail_box.generic.y						= 20;
	s_detail_box.generic.name					= "texture detail";
	s_detail_box.generic.callback				= DetailCallback;
	s_detail_box.itemnames						= detail_names;
	s_detail_box.curvalue						= ( int ) sw_mipcap->value - 1;
	if ( s_detail_box.curvalue < 0 )
		s_detail_box.curvalue = 0;
	else if ( s_detail_box.curvalue > 2 )
		s_detail_box.curvalue = 2;

	s_waterwarp_box.generic.type				= MTYPE_SPINCONTROL;
	s_waterwarp_box.generic.x					= 0;
	s_waterwarp_box.generic.y					= 30;
	s_waterwarp_box.generic.name				= "water warp";
	s_waterwarp_box.generic.callback			= WaterWarpCallback;
	s_waterwarp_box.itemnames					= onoff_names;
	s_waterwarp_box.curvalue					= sw_waterwarp->value ? 1 : 0;

	s_sound_box.generic.type					= MTYPE_SPINCONTROL;
	s_sound_box.generic.x						= 0;
	s_sound_box.generic.y						= 40;
	s_sound_box.generic.name					= "sound";
	s_sound_box.generic.callback				= SoundCallback;
	s_sound_box.itemnames						= onoff_names;
	s_sound_box.curvalue						= Cvar_VariableValue( "s_soundon" ) ? 1 : 0;

	s_defaults_action.generic.type				= MTYPE_ACTION;
	s_defaults_action.generic.x					= 0;
	s_defaults_action.generic.y					= 50;
	s_defaults_action.generic.name				= "reset to default";
	s_defaults_action.generic.callback			= ResetDefaults;

	s_done_action.generic.type					= MTYPE_ACTION;
	s_done_action.generic.x						= 0;
	s_done_action.generic.y						= 60;
	s_done_action.generic.name					= "done";
	s_done_action.generic.callback				= DoneAction;

	s_video_menu.x		= viddef.width / 2 - 8;
	s_video_menu.nitems	= 0;

	Menu_AddItem( &s_video_menu, ( void * ) &s_screensize_slider );
	Menu_AddItem( &s_video_menu, ( void * ) &s_brightness_slider );
	Menu_AddItem( &s_video_menu, ( void * ) &s_detail_box );
	Menu_AddItem( &s_video_menu, ( void * ) &s_waterwarp_box );
	Menu_AddItem( &s_video_menu, ( void * ) &s_sound_box );
	Menu_AddItem( &s_video_menu, ( void * ) &s_defaults_action );
	Menu_AddItem( &s_video_menu, ( void * ) &s_done_action );

	Menu_Center( &s_video_menu );
}

/*
================
VID_MenuDraw
================
*/
void VID_MenuDraw (void)
{
	int w, h;

	// draw the banner
	re.DrawGetPicSize( &w, &h, "m_banner_video" );
	re.DrawPic( viddef.width / 2 - w / 2, viddef.height / 2 - 110, "m_banner_video" );

	// move cursor to a reasonable starting position
	Menu_AdjustCursor( &s_video_menu, 1 );

	Menu_Draw( &s_video_menu );
}

/*
================
VID_MenuKey
================
*/
const char *VID_MenuKey( int key )
{
	extern void M_PopMenu( void );

	static const char *sound = "misc/menu1.wav";

	switch ( key )
	{
	case K_ESCAPE:
		M_PopMenu();
		return NULL;
	case K_UPARROW:
		s_video_menu.cursor--;
		Menu_AdjustCursor( &s_video_menu, -1 );
		break;
	case K_DOWNARROW:
		s_video_menu.cursor++;
		Menu_AdjustCursor( &s_video_menu, 1 );
		break;
	case K_LEFTARROW:
		Menu_SlideItem( &s_video_menu, -1 );
		break;
	case K_RIGHTARROW:
		Menu_SlideItem( &s_video_menu, 1 );
		break;
	case K_ENTER:
		Menu_SelectItem( &s_video_menu );
		break;
	}

	return sound;
}
