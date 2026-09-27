// Pick the month to leave Independence. Earlier = milder mountains but you wait
// longer for spring grass; later = risk of snow in the passes.
// (SelectStartingMonthState.cs offers March–July; the original recommends March.)
#pragma once
#include "compat/Arduino.h"

#include "compat/LovyanGFX.hpp"

#include "game/Session.h"
#include "screens/NameEntryScreen.h"
#include "ui/App.h"
#include "ui/Screen.h"
#include "ui/ScreenStack.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

class MonthScreen : public Screen {
public:
    void onEnter() override {
        menu_.clear();
        sel_ = 0;
        for (int i = 0; i < 5; ++i)   // March .. July
            menu_.add(game::monthName(static_cast<game::Month>(i)));
        menu_.setBounds(theme::MARGIN, 72, 320 - 2 * theme::MARGIN, 32);
    }

    void render(LGFX_Sprite& g) override {
        g.fillScreen(theme::BG);
        ui::drawCentered(g, "WHEN DO YOU LEAVE?", 160, 10, theme::ACCENT, 4);
        ui::drawCentered(g, "1848. March is the classic choice.", 160, 46,
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
        if (dy < 0 || dx < 0) sel_ = (sel_ + 4) % 5;
        if (dy > 0 || dx > 0) sel_ = (sel_ + 1) % 5;
    }

    void onConfirm(int16_t, int16_t) override { activateSelected(); }

private:
    void activateSelected() {
        game::g.startMonth = static_cast<game::Month>(sel_);
        app::screens.replace(new NameEntryScreen());
    }

private:
    ui::MenuList menu_;
    int sel_ = -1;
};
