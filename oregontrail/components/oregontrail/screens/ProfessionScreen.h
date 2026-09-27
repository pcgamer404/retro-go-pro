// Pick the party leader's profession — sets starting cash and the final-score
// multiplier (ProfessionSelector.cs). First screen of the new-game flow.
#pragma once
#include "compat/Arduino.h"

#include "compat/LovyanGFX.hpp"

#include "game/Session.h"
#include "screens/MessageScreen.h"
#include "screens/MonthScreen.h"
#include "ui/App.h"
#include "ui/Screen.h"
#include "ui/ScreenStack.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

class ProfessionScreen : public Screen {
public:
    void onEnter() override {
        menu_.clear();
        sel_ = 0;
        for (int i = 0; i < 3; ++i) {
            const auto& info = game::profInfo(static_cast<game::Profession>(i));
            menu_.add(String(info.label) + "   $" + info.startCash);
        }
        menu_.add("How do these differ?");
        menu_.setBounds(theme::MARGIN, 74, 320 - 2 * theme::MARGIN, 38);
    }

    void render(LGFX_Sprite& g) override {
        g.fillScreen(theme::BG);
        ui::drawCentered(g, "WHO WILL YOU BE?", 160, 10, theme::ACCENT, 4);
        ui::drawCentered(g, "Your trade sets your starting money.", 160, 46,
                         theme::INK_DIM, 2);
        menu_.render(g, sel_);
    }

    void onTap(int16_t x, int16_t y) override {
        const int i = menu_.hitTest(x, y);
        if (i < 0) return;
        sel_ = i;
        activateSelected();
    }

    void onDpad(int dx, int dy) override {
        if (dy < 0 || dx < 0) sel_ = (sel_ + 3) % 4;
        if (dy > 0 || dx > 0) sel_ = (sel_ + 1) % 4;
    }

    void onConfirm(int16_t, int16_t) override { activateSelected(); }

private:
    void activateSelected() {
        if (sel_ >= 0 && sel_ < 3) {
            game::g.beginNewGame(static_cast<game::Profession>(sel_));
            app::screens.replace(new MonthScreen());
        } else if (sel_ == 3) {
            app::screens.push(new MessageScreen(
                "The choices",
                "Banker from Boston: $1600, no score bonus.\n"
                "Carpenter from Ohio: $800, double score.\n"
                "Farmer from Illinois: $400, triple score.\n\n"
                "Less money is a harder trip but scores far higher if you "
                "make it."));
        }
    }

private:
    ui::MenuList menu_;
    int sel_ = -1;
};
