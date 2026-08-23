/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2012 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    Sam Lantinga
    slouken@libsdl.org
*/

#ifndef _SDL_config_minimal_h
#define _SDL_config_minimal_h

//#include "SDL_platform.h"

/* This is the minimal configuration that can be used to build SDL */

/* retro-go-pro note: originally this file hand-rolled int8_t/uint8_t/
 * int16_t/uint16_t/int32_t/uint32_t/size_t itself (freestanding SDL,
 * assumed no libc). On IDF 5.3.5's xtensa toolchain, <stdint.h> already
 * provides real definitions of these, and the hand-rolled ones now
 * conflict (different underlying types). Just use the real headers. */
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

/* Enable the dummy audio driver (src/audio/dummy/\*.c) */
#define SDL_AUDIO_DRIVER_DUMMY	1

/* Enable the stub cdrom driver (src/cdrom/dummy/\*.c) */
#define SDL_CDROM_DISABLED	1

/* Enable the stub joystick driver (src/joystick/dummy/\*.c) */
#define SDL_JOYSTICK_DISABLED	1

/* Enable the stub shared object loader (src/loadso/dummy/\*.c) */
#define SDL_LOADSO_DISABLED	1

/* Enable the stub thread support (src/thread/generic/\*.c) */
#define SDL_THREADS_DISABLED	1

/* Enable the stub timer support (src/timer/dummy/\*.c) */
#define SDL_TIMERS_DISABLED	1

/* Enable the dummy video driver (src/video/dummy/\*.c) */
#define SDL_VIDEO_DRIVER_DUMMY	1
