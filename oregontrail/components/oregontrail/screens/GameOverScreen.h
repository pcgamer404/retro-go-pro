// End of the road — arrival in Oregon or the loss of the party. Shows the final
// score (FinalPoints.cs) and records it if it makes the Top Five.
#pragma once
#include "compat/Arduino.h"

#include "compat/LovyanGFX.hpp"

#include "game/Scoring.h"
#include "ui/Screen.h"
#include "ui/Widgets.h"

class GameOverScreen : public Screen {
public:
    explicit GameOverScreen(bool won) : won_(won) {}
    void onEnter() override;
    void render(LGFX_Sprite& g) override;
    void onTap(int16_t x, int16_t y) override;
    void onDpad(int dx, int dy) override { if(dy || dx) selected_=1-selected_; }
    void onConfirm(int16_t, int16_t) override { activate(); }

private:
    bool        won_;
    game::Score score_{};
    int         rank_ = -1;
    ui::Rect    scores_{}, menu_{};
    int selected_=1;
    void activate();
};
