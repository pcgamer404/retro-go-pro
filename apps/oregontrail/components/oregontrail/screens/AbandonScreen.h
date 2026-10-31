// "Give up and turn back" confirmation, reached from the trail menu.
#pragma once
#include "compat/Arduino.h"

#include "compat/LovyanGFX.hpp"

#include "SaveGame.h"
#include "screens/MainMenuScreen.h"
#include "ui/App.h"
#include "ui/Screen.h"
#include "ui/ScreenStack.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

class AbandonScreen : public Screen {
public:
    void render(LGFX_Sprite& g) override {
        g.fillScreen(theme::BG);
        ui::drawCentered(g, "GIVE UP THE TRAIL?", 160, 40, theme::WARN, 4);
        ui::drawText(g,
                     "The journey so far will be lost and you will not be scored.",
                     theme::MARGIN, 84, 320 - 2 * theme::MARGIN, theme::INK, 2, 18,
                     true);

        yes_ = {theme::MARGIN, 150, 150, 34};
        no_  = {166, 150, 146, 34};
        ui::drawButton(g, yes_, "Turn back", selected_==0);
        ui::drawButton(g, no_, "Keep going", selected_==1);
    }

    void onDpad(int dx, int dy) override { if(dy || dx) selected_=1-selected_; }
    void onConfirm(int16_t, int16_t) override { activate(); }
    void onTap(int16_t x, int16_t y) override {
        if(yes_.contains(x,y)) selected_=0; else if(no_.contains(x,y)) selected_=1; else return;
        activate();
    }

private:
    ui::Rect yes_{}, no_{};
    int selected_=1;
    void activate(){ if(selected_==0){ savegame::clear(); app::screens.reset(new MainMenuScreen()); } else app::screens.pop(); }
};
