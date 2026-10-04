#ifndef LIBRETRO_CORE_OPTIONS_H__
#define LIBRETRO_CORE_OPTIONS_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <retro_inline.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 ********************************
 * Core Option Definitions
 ********************************
 *
 * This file declares the core options in libretro Core Options v2
 * format and provides a version-dispatching helper
 * (libretro_set_core_options) that registers them via the best API
 * the running frontend supports.  In order of preference:
 *
 *   v2 frontend  -> RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2 (env 67)
 *   v1 frontend  -> RETRO_ENVIRONMENT_SET_CORE_OPTIONS    (env 53)
 *   legacy       -> RETRO_ENVIRONMENT_SET_VARIABLES       (env 16)
 *
 * The v2 struct types are forward-declared below in case the bundled
 * libretro.h is old enough to lack them (this codebase's libretro.h
 * tops out at env 66, so the v2 environment IDs and types are
 * provided locally; on a newer libretro.h the existing declarations
 * win because we ifdef around the env-ID guard).  Sublabels (info
 * text) describe what each option does and surface in the frontend's
 * detail pane when the user navigates to the option.
 */

/* ---------- v2 forward-compat ---------- */

#ifndef RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2 67

struct retro_core_option_v2_category
{
   const char *key;
   const char *desc;
   const char *info;
};

struct retro_core_option_v2_definition
{
   const char *key;
   const char *desc;
   const char *desc_categorized;
   const char *info;
   const char *info_categorized;
   const char *category_key;
   struct retro_core_option_value values[RETRO_NUM_CORE_OPTION_VALUES_MAX];
   const char *default_value;
};

struct retro_core_options_v2
{
   struct retro_core_option_v2_category   *categories;
   struct retro_core_option_v2_definition *definitions;
};
#endif /* RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2 */

/* ---------- definitions ---------- */

static struct retro_core_option_v2_definition option_defs_us[] = {
   {
      "mame2000-frameskip",
      "Frameskip",
      NULL,
      "Skip rendering frames to keep the audio output in sync when "
      "the host cannot run the game at full speed.  'auto' uses the "
      "frontend's audio-buffer hint; 'threshold' skips when the "
      "audio buffer drops below the percentage configured below.",
      NULL,
      NULL,
      {
         { "disabled",  NULL },
         { "auto",      NULL },
         { "threshold", NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "mame2000-frameskip_threshold",
      "Frameskip Threshold (%)",
      NULL,
      "When Frameskip is set to 'threshold', skip a frame whenever "
      "the audio output buffer falls below this percentage of full.  "
      "Lower values skip more aggressively.",
      NULL,
      NULL,
      {
         { "30", NULL },
         { "40", NULL },
         { "50", NULL },
         { "60", NULL },
         { NULL, NULL },
      },
      "30"
   },
   {
      "mame2000-frameskip_interval",
      "Frameskip Interval",
      NULL,
      "Maximum number of consecutive frames that may be skipped.  "
      "Lower values keep visual updates smoother at the cost of "
      "more audio drops when the host is overloaded.",
      NULL,
      NULL,
      {
         { "1", NULL }, { "2", NULL }, { "3", NULL }, { "4", NULL },
         { "5", NULL }, { "6", NULL }, { "7", NULL }, { "8", NULL },
         { "9", NULL },
         { NULL, NULL },
      },
      "1"
   },
   {
      "mame2000-skip_disclaimer",
      "Skip Disclaimer",
      NULL,
      "Hide MAME's legal disclaimer screen at game startup.",
      NULL,
      NULL,
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "mame2000-show_gameinfo",
      "Show Game Information",
      NULL,
      "Show MAME's driver-status and game-information screens at "
      "startup before the game begins running.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "mame2000-sample_rate",
      "Sample rate hint (Restart)",
      NULL,
      "Output audio sample rate, in Hz.  22050 is the legacy MAME "
      "0.78 default; higher rates increase audio fidelity at the "
      "cost of additional CPU.  \"Auto\" asks the frontend for its "
      "target rate and rounds to the nearest standard rate, falling "
      "back to 48000 if the frontend does not report one.  \"Manual\" "
      "currently behaves like \"Auto\".  Changing this value requires "
      "a core restart.",
      NULL,
      NULL,
      {
         { "auto",   "Auto" },
         { "manual", "Manual" },
         { "8000",   NULL },
         { "11025",  NULL },
         { "22050",  NULL },
         { "32000",  NULL },
         { "44100",  NULL },
         { "48000",  NULL },
         { "96000",  NULL },
         { NULL, NULL },
      },
      "22050"
   },
   {
      "mame2000-stereo",
      "Stereo (Restart)",
      NULL,
      "Output stereo audio.  Disable for slightly lower CPU on "
      "games that don't use stereo mixing.  Changing this value "
      "requires a core restart.",
      NULL,
      NULL,
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "mame2000-qsound_output_filter",
      "QSound Output Filter",
      NULL,
      "Run the QSound voice mix through a 95-tap windowed-sinc "
      "low-pass FIR (8 kHz cutoff) and per-channel output delay "
      "line before sending it to the host.  Smooths the upper-band "
      "aliasing the 8-bit ROM samples produce on transients and "
      "applies the algorithm's slight L/R offset for a wider "
      "stereo image.  Costs roughly 15-20% extra CPU during active "
      "QSound audio.  This option is hidden in the menu for games "
      "that don't use the QSound sound chip.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },

   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};

static struct retro_core_options_v2 options_us = {
   NULL,            /* no categories */
   option_defs_us
};

/* ---------- dispatch helper ---------- */

/* Register the option set with the frontend via the best-supported
 * API.  The frontend keeps its own copy of the structures so the
 * temporary v1 array we build for the v1 fallback is safe to free
 * after the env call; the legacy SET_VARIABLES fallback uses static
 * storage because the frontend may keep pointers to the value strings. */
static INLINE void libretro_set_core_options(retro_environment_t environ_cb)
{
   unsigned version = 0;

   if (!environ_cb)
      return;

   if (environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) &&
       (version >= 2))
   {
      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_us);
      return;
   }

   if (version >= 1)
   {
      /* Build a v1 array by stripping the v2-specific
       * (categorized / category_key) fields. */
      size_t i, num_options = 0;
      struct retro_core_option_definition *v1_defs;

      while (option_defs_us[num_options].key)
         num_options++;

      v1_defs = (struct retro_core_option_definition *)
                calloc(num_options + 1, sizeof(*v1_defs));
      if (!v1_defs)
         return;

      for (i = 0; i < num_options; i++)
      {
         v1_defs[i].key           = option_defs_us[i].key;
         v1_defs[i].desc          = option_defs_us[i].desc;
         v1_defs[i].info          = option_defs_us[i].info;
         memcpy(v1_defs[i].values, option_defs_us[i].values,
                sizeof(v1_defs[i].values));
         v1_defs[i].default_value = option_defs_us[i].default_value;
      }

      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, v1_defs);
      free(v1_defs);
      return;
   }

   /* Legacy SET_VARIABLES fallback: build "desc; default|v1|v2|..."
    * strings.  Uses static storage because the frontend may keep
    * pointers to the value strings indefinitely. */
   {
#define MAX_OPTS    32
#define MAX_VAL_LEN 384
      static struct retro_variable vars[MAX_OPTS + 1];
      static char                  values_buf[MAX_OPTS][MAX_VAL_LEN];
      size_t i, j, num_options = 0;

      while (option_defs_us[num_options].key)
         num_options++;
      if (num_options > MAX_OPTS)
         num_options = MAX_OPTS;

      for (i = 0; i < num_options; i++)
      {
         size_t      len;
         const char *def = option_defs_us[i].default_value;

         /* "desc; default" then "|other" for every non-default value */
         len = snprintf(values_buf[i], MAX_VAL_LEN, "%s; %s",
                        option_defs_us[i].desc, def ? def : "");

         for (j = 0; option_defs_us[i].values[j].value &&
                     len < MAX_VAL_LEN - 1; j++)
         {
            if (def && strcmp(option_defs_us[i].values[j].value, def) == 0)
               continue;
            len += snprintf(values_buf[i] + len, MAX_VAL_LEN - len,
                            "|%s", option_defs_us[i].values[j].value);
         }

         vars[i].key   = option_defs_us[i].key;
         vars[i].value = values_buf[i];
      }
      vars[num_options].key   = NULL;
      vars[num_options].value = NULL;

      environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
#undef MAX_OPTS
#undef MAX_VAL_LEN
   }
}

#ifdef __cplusplus
}
#endif

#endif /* LIBRETRO_CORE_OPTIONS_H__ */
