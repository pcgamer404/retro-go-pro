/**
 * @file cnc_gamehooks.cpp
 * @brief Debug console commands that look into the game state (compiled with
 * the game headers).
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */
#include "function.h"

#ifdef CNC_GAME_RA
// in a mission (Red Alert has no InMainLoop)
#define InMainLoop GameActive
#endif

// "retrieve <file>": looks a file up in the registered MIX files
extern "C" void Debug_Retrieve(const char* name)
{
    MFCD* mix = nullptr;
    void* ptr = nullptr;
    int offset = 0, size = 0;
    bool found = MFCD::Offset(name, &ptr, &mix, &offset, &size);
    printf("%s: found %d, mix %s, data %p, offset %d, size %d\n",
           name,
           found,
           mix ? mix->Filename : "-",
           ptr,
           offset,
           size);
    if (ptr) {
        const uint8_t* p = (const uint8_t*)ptr;
        printf("  %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
               p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
    }
}

// Game logic frame counter (statistics)
extern "C" int Debug_Game_Frame(void)
{
    return Frame;
}

static void listObjects(const char* kind, ObjectClass* obj)
{
    COORDINATE c = obj->Center_Coord();
    printf("  %s %s house %d at cell %d,%d strength %d%s%s\n",
           kind,
           obj->Class_Of().IniName,
           obj->Owner(),
           Coord_XCell(c),
           Coord_YCell(c),
           obj->Strength,
           obj->IsInLimbo ? " limbo" : "",
           obj->IsSelected ? " selected" : "");
}

// "units": lists the infantry, vehicles and aircraft
extern "C" void Debug_Units(void)
{
    printf("Frame %d, tactical cell %d,%d\n", Frame, Coord_XCell(Map.TacticalCoord), Coord_YCell(Map.TacticalCoord));
    for (int i = 0; i < Infantry.Count(); i++) {
        listObjects("inf", Infantry.Ptr(i));
    }
    for (int i = 0; i < Units.Count(); i++) {
        listObjects("unit", Units.Ptr(i));
    }
    for (int i = 0; i < Aircraft.Count(); i++) {
        listObjects("air", Aircraft.Ptr(i));
    }
}

// "win": the player wins the current mission (to reach the score screen quickly)
extern "C" void Cnc_Debug_Win(void)
{
    if (PlayerPtr != nullptr && InMainLoop) {
        PlayerPtr->Flag_To_Win();
        printf("mission flagged as won\n");
    } else {
        printf("not in a mission\n");
    }
}

// Fonts of the flash copy progress screen (cnc_video.cpp), once the mixfile holding
// them is registered; null before
extern "C" const void* cncProgressFont(int big)
{
#ifdef CNC_GAME_RA
    // LOCAL.MIX fonts: available from the first copy (LOCAL.MIX itself)
    static const char* const names[2][2] = {{"8POINT.FNT", "6POINT.FNT"}, {"VCR.FNT", "8POINT.FNT"}};
#else
    static const char* const names[2][2] = {{"8FAT.FNT", "6POINT.FNT"}, {"12GRNGRD.FNT", "8FAT.FNT"}};
#endif
    for (const char* name : names[big ? 1 : 0]) {
        void const* font = MFCD::Retrieve(name);
        if (font != nullptr) {
            return font;
        }
    }
    return nullptr;
}

// cnc_video.cpp: the game palette is not black (the screen of the game can be seen)
extern "C" bool videoGamePaletteVisible(void);
extern "C" void videoHideConsole(void);

#define STANDBY_Y      136     // progress line, under the "Please stand by" of the title page
#define STANDBY_ROWS   24

// Progress of a flash copy (cnc_flashcache.cpp) drawn on the game screen, in the style of
// its "Please stand by" (percent < 0: done, the screen is restored). Returns false if the
// game screen can't be used yet (no fonts or palette).
extern "C" bool cncStandBy(const char* detail, int percent)
{
    static uint8_t saved[320 * STANDBY_ROWS];
    static bool active;
    if (!active) {
        if (percent < 0 || Font6Ptr == nullptr || GradFont6Ptr == nullptr || !videoGamePaletteVisible()
            || SeenBuff.Get_Width() < 320) {
            return false;
        }
    }
    // the address of the visible page is only valid while it is locked
    if (!SeenBuff.Lock()) {
        return false;
    }
    int pitch = SeenBuff.Get_Width() + SeenBuff.Get_XAdd() + SeenBuff.Get_Pitch();
    uint8_t* strip = (uint8_t*)SeenBuff.Get_Offset() + STANDBY_Y * pitch;
    if (!active) {
        for (int y = 0; y < STANDBY_ROWS; y++) {
            memcpy(saved + y * 320, strip + y * pitch, 320);
        }
        active = true;
    } else {
        for (int y = 0; y < STANDBY_ROWS; y++) {
            memcpy(strip + y * pitch, saved + y * 320, 320);
        }
    }
    SeenBuff.Unlock();
    if (percent < 0) {
        active = false;
        videoHideConsole();
        return true;
    }

    void const* font = FontPtr;
    int xspacing = FontXSpacing, yspacing = FontYSpacing;
    GraphicViewPortClass* page = Set_Logic_Page(SeenBuff);
    char text[80];
    snprintf(text, sizeof(text), "%s  %d%%", detail, percent);
#ifdef CNC_GAME_RA
    RemapControlType* scheme = &ColorRemaps[PCOLOR_DIALOG_BLUE];
    // as the "Please stand by" of the title page (init.cpp)
    Fancy_Text_Print("%s", 160, STANDBY_Y + 2, scheme, TBLACK, TPF_CENTER | TPF_TEXT | TPF_DROPSHADOW, text);
    int frame = scheme->Color, fill = scheme->BrightColor;
#else
    Fancy_Text_Print("%s",
                     160,
                     STANDBY_Y + 2,
                     CC_GREEN,
                     TBLACK,
                     TPF_CENTER | TPF_6PT_GRAD | TPF_DROPSHADOW | TPF_USE_GRAD_PAL,
                     text);
    int frame = CC_GREEN_SHADOW, fill = CC_GREEN;
#endif
    // bar
    const int x0 = 80, y0 = STANDBY_Y + 14, w = 160, h = 6;
    SeenBuff.Draw_Rect(x0, y0, x0 + w - 1, y0 + h - 1, frame);
    int filled = (w - 4) * percent / 100;
    if (filled > 0) {
        SeenBuff.Fill_Rect(x0 + 2, y0 + 2, x0 + 1 + filled, y0 + h - 3, fill);
    }
    Set_Logic_Page(page);
    Set_Font(font);
    FontXSpacing = xspacing;
    FontYSpacing = yspacing;
    videoHideConsole();
    return true;
}

// cnc_flashcache.cpp
extern "C" void cncFlashCacheSetStandBy(bool on);

// init.cpp: the title page ("please stand by") is shown while the main mixfiles are
// cached: the copies to flash are made then, the deferred ones first.
void Cnc_Stand_By_Begin(void)
{
    cncFlashCacheSetStandBy(true);
    MFCD::Flash_Copy_Pending();
}

void Cnc_Stand_By_End(void)
{
    MFCD::Flash_Copy_Pending();
    cncFlashCacheSetStandBy(false);
}
