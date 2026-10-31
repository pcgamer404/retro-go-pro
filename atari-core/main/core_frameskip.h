// Shared frameskip policy for every emulator in this app.
//
// retro-go's auto-frameskip (rg_system.c) can climb to 5, i.e. 1 frame drawn out of 6, and it never goes
// back below 1 (and several actions reset it to 1). Cores call core_frameskip() instead of reading
// app->frameskip directly: the auto value is capped by the "Max frameskip" option (default 2, saved per system).
// 0 = never skip except when a frame is already late. Fast-forward (speed option > 1.0) is not capped.
#pragma once

#include <rg_system.h>
#include <stdio.h>
#include <string.h>

#define CORE_MAXSKIP_SETTING "maxskip"

extern int core_maxskip; // defined in main.c

static inline void core_maxskip_load(void)
{
    int v = rg_settings_get_number(NS_APP, CORE_MAXSKIP_SETTING, 2);
    core_maxskip = (v < 0 || v > 5) ? 2 : v;
}

static inline int core_frameskip(const rg_app_t *app)
{
    if (app->speed > 1.01f)
        return app->frameskip;
    return app->frameskip < core_maxskip ? app->frameskip : core_maxskip;
}

static inline rg_gui_event_t core_maxskip_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        core_maxskip += (event == RG_DIALOG_NEXT) ? 1 : -1;
        if (core_maxskip < 0) core_maxskip = 5;
        if (core_maxskip > 5) core_maxskip = 0;
        rg_settings_set_number(NS_APP, CORE_MAXSKIP_SETTING, core_maxskip);
        return RG_DIALOG_REDRAW;
    }
    if (core_maxskip == 0)
        strcpy(option->value, "Late only");
    else
        sprintf(option->value, "%d", core_maxskip);
    return RG_DIALOG_VOID;
}

#define CORE_MAXSKIP_OPTION ((rg_gui_option_t){0, "Max frameskip", (char *)"-", RG_DIALOG_FLAG_NORMAL, &core_maxskip_update_cb})

// For cores that have no options menu of their own
static inline void core_options_handler(rg_gui_option_t *dest)
{
    *dest++ = CORE_MAXSKIP_OPTION;
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}
