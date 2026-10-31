// A river to cross. Ford it, seal the wagon and float, take the ferry (where
// there is one), or wait for the water to drop.
#pragma once
#include "compat/Arduino.h"

#include "compat/LovyanGFX.hpp"

#include "game/Rivers.h"
#include "ui/Screen.h"
#include "ui/Widgets.h"

class RiverScreen : public Screen {
public:
    void onEnter() override;
    void render(LGFX_Sprite& g) override;
    void onTap(int16_t x, int16_t y) override;
    void onDpad(int, int dy) override;
    void onConfirm(int16_t, int16_t) override;

private:
    const game::RiverInfo* r_ = nullptr;
    bool     showResult_ = false;
    game::CrossResult result_{};
    ui::Rect ford_{}, caulk_{}, ferry_{}, wait_{}, ok_{};
    int selected_=0;

    void doChoice(game::CrossChoice c);
};
