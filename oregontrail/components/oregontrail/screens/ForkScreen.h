// A parting of the ways. The two roads out of a fork node, with the historic
// trade-off spelled out. Choosing sets the branch and returns to travelling.
#pragma once
#include "compat/Arduino.h"

#include "compat/LovyanGFX.hpp"

#include "ui/Screen.h"
#include "ui/Widgets.h"
#include "game/Sim.h"
#include "ui/App.h"
#include "ui/ScreenStack.h"

class ForkScreen : public Screen {
public:
    void render(LGFX_Sprite& g) override;
    void onTap(int16_t x, int16_t y) override;
    void onDpad(int dx, int dy) override { if(dy || dx) selected_=1-selected_; }
    void onConfirm(int16_t, int16_t) override { game::sim.chooseBranch(selected_); app::screens.pop(); }

private:
    ui::Rect a_{}, b_{};
    int selected_=0;
};
