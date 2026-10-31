// "You have reached ___." Shown on arrival at a trail stop: a drawn landmark, a
// line of flavor, and — at forts — a way into the store.
#pragma once
#include "compat/Arduino.h"

#include "compat/LovyanGFX.hpp"

#include "ui/Screen.h"
#include "ui/Widgets.h"

class LandmarkScreen : public Screen {
public:
    void onEnter() override;
    void render(LGFX_Sprite& g) override;
    void onTap(int16_t x, int16_t y) override;
    void onDpad(int dx, int dy) override { if(hasStore_ && (dy || dx)) selected_=1-selected_; }
    void onConfirm(int16_t, int16_t) override { activate(); }

private:
    bool     hasStore_ = false;
    ui::Rect go_{}, store_{};
    int selected_=1;
    void activate();
};
