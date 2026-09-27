// Set the travelling pace and the food ration. (ChangePace.cs / ChangeRations.cs)
#pragma once
#include "compat/Arduino.h"

#include "compat/LovyanGFX.hpp"

#include "game/Session.h"
#include "screens/Hud.h"
#include "ui/App.h"
#include "ui/Screen.h"
#include "ui/ScreenStack.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

class PaceRationsScreen : public Screen {
public:
    void render(LGFX_Sprite& g) override {
        g.fillScreen(theme::BG);
        hud::drawTrailStatus(g);

        static const char* pace[3] = {"Steady - normal pace",
                                      "Strenuous - 50% more a day",
                                      "Grueling - double, hard on all"};
        static const char* rat[3] = {"Filling - 3 lb each",
                                     "Meager - 2 lb each",
                                     "Bare bones - 1 lb each"};
        const int16_t W = 320 - 2 * theme::MARGIN;

        g.setFont(&fonts::Font2);
        g.setTextDatum(textdatum_t::top_left);
        g.setTextColor(theme::ACCENT);
        g.drawString("TRAVELLING PACE", theme::MARGIN, 24);
        const int curP = static_cast<int>(game::g.vehicle.pace);
        for (int i = 0; i < 3; ++i) {
            paceR_[i] = {theme::MARGIN, (int16_t)(40 + i * 28), W, 24};
            ui::drawButton(g, paceR_[i], pace[i], i == curP || selected_==i);
        }

        g.setTextDatum(textdatum_t::top_left);
        g.setTextColor(theme::ACCENT);
        g.drawString("FOOD RATIONS", theme::MARGIN, 128);
        const int curR = static_cast<int>(game::g.vehicle.rations);
        for (int i = 0; i < 3; ++i) {
            ratR_[i] = {theme::MARGIN, (int16_t)(144 + i * 24), W, 20};
            ui::drawButton(g, ratR_[i], rat[i], i == curR || selected_==i+3);
        }

        back_ = {(int16_t)(160 - 70), 214, 140, 24};
        ui::drawButton(g, back_, "Back to the trail", selected_==6);
    }

    void onDpad(int, int dy) override { if(dy<0) selected_=(selected_+6)%7; else if(dy>0) selected_=(selected_+1)%7; }
    void onConfirm(int16_t, int16_t) override { activate(); }

    void onTap(int16_t x, int16_t y) override {
        for (int i = 0; i < 3; ++i) {
            if (paceR_[i].contains(x, y)) {
                game::g.vehicle.pace = static_cast<game::Pace>(i);
                return;
            }
            if (ratR_[i].contains(x, y)) {
                game::g.vehicle.rations = static_cast<game::Rations>(i);
                return;
            }
        }
        if (back_.contains(x, y)) app::screens.pop();
    }

private:
    ui::Rect paceR_[3], ratR_[3], back_{};
    int selected_=0;
    void activate(){ if(selected_<3) game::g.vehicle.pace=(game::Pace)selected_; else if(selected_<6) game::g.vehicle.rations=(game::Rations)(selected_-3); else app::screens.pop(); }
};
