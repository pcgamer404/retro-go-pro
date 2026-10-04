/* Minimal subset of the libretro API used by the RACE core sources
 * (logging enum and the log callback type). retro-go provides the rest. */
#ifndef RACE_LIBRETRO_SHIM_H
#define RACE_LIBRETRO_SHIM_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifndef _MAX_PATH
#define _MAX_PATH 512
#endif
#ifndef CZ80
#define CZ80 1
#endif
enum retro_log_level { RETRO_LOG_DEBUG = 0, RETRO_LOG_INFO, RETRO_LOG_WARN, RETRO_LOG_ERROR };
typedef void (*retro_log_printf_t)(enum retro_log_level level, const char *fmt, ...);
#endif
