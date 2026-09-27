// The hub while on the trail — big touch buttons: press on, plus supplies, map,
// pace & rations, rest, and (in context) hunt / talk. (Travel.cs command menu.)
#pragma once
#include "compat/Arduino.h"

#include "compat/LovyanGFX.hpp"

#include "ui/Screen.h"
#include "ui/Widgets.h"

class TrailMenuScreen : public Screen {
public:
    void onEnter() override;
    void render(LGFX_Sprite& g) override;
    void onTap(int16_t x, int16_t y) override;
    void onDpad(int dx, int dy) override;
    void onConfirm(int16_t, int16_t) override;

private:
    ui::Rect go_{}, supplies_{}, map_{}, paceRations_{}, rest_{};
    ui::Rect hunt_{}, talk_{}, settings_{}, abandon_{};
    bool     showHunt_ = false, showTalk_ = false;
    int      selected_ = 0;
    int optionCount() const;
    void activateSelected();
};
