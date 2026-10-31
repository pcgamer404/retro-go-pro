// Presents a random trail event. Most just need acknowledging; a broken wagon
// part offers the choice of fitting a spare or making camp to repair.
#pragma once
#include "compat/Arduino.h"

#include "compat/LovyanGFX.hpp"

#include "ui/Screen.h"
#include "ui/Widgets.h"

class EventScreen : public Screen {
public:
    void onEnter() override;
    void render(LGFX_Sprite& g) override;
    void onTap(int16_t x, int16_t y) override;
    void onDpad(int dx, int dy) override { if(!resolved_ && (dy || dx)) selected_=1-selected_; }
    void onConfirm(int16_t, int16_t) override;

private:
    bool     resolved_ = false;
    int      daysLost_ = 0;
    ui::Rect ok_{}, spare_{}, repair_{};
    int selected_=0;
};
