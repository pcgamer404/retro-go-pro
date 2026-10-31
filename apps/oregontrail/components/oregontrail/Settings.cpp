#include "Settings.h"
#include <rg_settings.h>
namespace prefs {
int brightness = 100;
int volume = 7;
void load() {
    brightness = (int)rg_settings_get_number(NS_APP, "ot_brightness", brightness);
    volume = (int)rg_settings_get_number(NS_APP, "ot_volume", volume);
    if (brightness < kBrightMin) brightness = kBrightMin;
    if (brightness > kBrightMax) brightness = kBrightMax;
    if (volume < 0) volume = 0;
    if (volume > kVolMax) volume = kVolMax;
}
void save() {
    rg_settings_set_number(NS_APP, "ot_brightness", brightness);
    rg_settings_set_number(NS_APP, "ot_volume", volume);
    rg_settings_commit();
}
}
