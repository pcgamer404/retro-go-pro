#include "screens/TrailMenuScreen.h"

#include "SaveGame.h"
#include "game/Advice.h"
#include "game/Session.h"
#include "game/Sim.h"
#include "screens/AbandonScreen.h"
#include "screens/HuntScreen.h"
#include "screens/Hud.h"
#include "screens/MapScreen.h"
#include "screens/MessageScreen.h"
#include "screens/PaceRationsScreen.h"
#include "screens/RestScreen.h"
#include "screens/RiverScreen.h"
#include "screens/SettingsScreen.h"
#include "screens/SuppliesScreen.h"
#include "screens/TravelingScreen.h"
#include "ui/App.h"
#include "ui/ScreenStack.h"
#include "ui/Theme.h"

void TrailMenuScreen::onEnter() {
    selected_ = 0;
    const auto k = game::sim.here().kind;
    showTalk_ = !game::sim.departed &&
                (k == game::Stop::Settlement || k == game::Stop::Landmark);
    showHunt_ = game::g.vehicle.bullets >= 1 && !game::sim.mustCross;
    if (!game::sim.atOregon()) savegame::save();   // checkpoint
}

void TrailMenuScreen::render(LGFX_Sprite& g) {
    g.fillScreen(theme::BG);
    hud::drawTrailStatus(g);

    const game::Sim& s = game::sim;

    // where the party is
    g.setFont(&fonts::Font2);
    g.setTextDatum(textdatum_t::top_left);
    if (!s.departed) {
        g.setTextColor(theme::ACCENT);
        g.drawString(String("You are at ") + s.here().name, theme::MARGIN, 26);
    } else {
        g.setTextColor(theme::INK);
        g.drawString(String("Heading for ") + s.nextName(), theme::MARGIN, 26);
    }
    g.setTextColor(theme::INK_DIM);
    char sub[52];
    snprintf(sub, sizeof(sub), "%d mi to the next stop   -   %d to Oregon",
             s.milesToNextStop(), s.milesRemaining());
    g.drawString(sub, theme::MARGIN, 42);

    // primary action
    const int16_t W = 320 - 2 * theme::MARGIN;
    go_ = {theme::MARGIN, 60, W, 34};
    ui::drawButton(g, go_,
                   s.mustCross      ? "Cross the river"
                   : s.departed     ? "Continue on the trail"
                                    : "Head out on the trail",
                   selected_==0);

    // 2 x 2 grid
    const int16_t colW = (W - 8) / 2, x2 = theme::MARGIN + colW + 8;
    supplies_    = {theme::MARGIN, 98,  colW, 32};
    map_         = {x2,            98,  colW, 32};
    paceRations_ = {theme::MARGIN, 136, colW, 32};
    rest_        = {x2,            136, colW, 32};
    ui::drawButton(g, supplies_, "Supplies", selected_==1);
    ui::drawButton(g, map_, "Map", selected_==2);
    ui::drawButton(g, paceRations_, "Pace & rations", selected_==3);
    ui::drawButton(g, rest_, "Rest", selected_==4);

    // context row
    hunt_ = talk_ = {};
    const int16_t cy = 174;
    if (showHunt_ && showTalk_) {
        hunt_ = {theme::MARGIN, cy, colW, 32};
        talk_ = {x2, cy, colW, 32};
        ui::drawButton(g, hunt_, "Hunt", selected_==5);
        ui::drawButton(g, talk_, "Talk to people", selected_==(showHunt_?6:5));
    } else if (showHunt_) {
        hunt_ = {theme::MARGIN, cy, W, 32};
        ui::drawButton(g, hunt_, "Hunt for food", selected_==5);
    } else if (showTalk_) {
        talk_ = {theme::MARGIN, cy, W, 32};
        ui::drawButton(g, talk_, "Talk to people", selected_==(showHunt_?6:5));
    }

    // utilities row: settings + give up
    settings_ = {theme::MARGIN, 210, colW, 26};
    abandon_  = {x2,            210, colW, 26};
    ui::drawButton(g, settings_, "Settings", selected_==optionCount()-2);
    ui::drawButton(g, abandon_, "Give up", selected_==optionCount()-1);
}

int TrailMenuScreen::optionCount() const { return 7 + (showHunt_ ? 1 : 0) + (showTalk_ ? 1 : 0); }

void TrailMenuScreen::onDpad(int dx, int dy) {
    const int n = optionCount();
    if (dy < 0) selected_ = (selected_ + n - 1) % n;
    else if (dy > 0) selected_ = (selected_ + 1) % n;
    else if (dx < 0) {
        if (selected_ == 2) selected_ = 1;
        else if (selected_ == 4) selected_ = 3;
        else if (selected_ == 6 && showHunt_ && showTalk_) selected_ = 5;
        else if (selected_ == n - 1) selected_ = n - 2;
    } else if (dx > 0) {
        if (selected_ == 1) selected_ = 2;
        else if (selected_ == 3) selected_ = 4;
        else if (selected_ == 5 && showHunt_ && showTalk_) selected_ = 6;
        else if (selected_ == n - 2) selected_ = n - 1;
    }
}
void TrailMenuScreen::onConfirm(int16_t, int16_t) { activateSelected(); }
void TrailMenuScreen::activateSelected() {
    int i=0;
    if (selected_==i++) { if (game::sim.mustCross) app::screens.push(new RiverScreen()); else app::screens.push(new TravelingScreen()); return; }
    if (selected_==i++) { app::screens.push(new SuppliesScreen()); return; }
    if (selected_==i++) { app::screens.push(new MapScreen()); return; }
    if (selected_==i++) { app::screens.push(new PaceRationsScreen()); return; }
    if (selected_==i++) { app::screens.push(new RestScreen()); return; }
    if (showHunt_ && selected_==i++) { app::screens.push(new HuntScreen()); return; }
    if (showTalk_ && selected_==i++) { app::screens.push(new MessageScreen(game::sim.here().name, game::randomAdvice())); return; }
    if (selected_==i++) { app::screens.push(new SettingsScreen()); return; }
    app::screens.push(new AbandonScreen());
}

void TrailMenuScreen::onTap(int16_t x, int16_t y) {
    if (go_.contains(x,y)) selected_=0;
    else if (supplies_.contains(x,y)) selected_=1;
    else if (map_.contains(x,y)) selected_=2;
    else if (paceRations_.contains(x,y)) selected_=3;
    else if (rest_.contains(x,y)) selected_=4;
    else if (showHunt_ && hunt_.w && hunt_.contains(x,y)) { selected_=5; if(showTalk_){} }
    else if (showTalk_ && talk_.w && talk_.contains(x,y)) { selected_=showHunt_?6:5; }
    else if (settings_.contains(x,y)) selected_=optionCount()-2;
    else if (abandon_.contains(x,y)) selected_=optionCount()-1;
    else return;
    activateSelected();
}
