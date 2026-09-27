#include "hw/Battery.h"
#include <rg_input.h>
namespace battery { void begin(){} void update(){} float volts(){rg_battery_t b=rg_input_read_battery();return b.volts;} float rawVolts(){return volts();} float divider(){return 0;} int percent(){rg_battery_t b=rg_input_read_battery();return(int)(b.level*100.0f+0.5f);} int charging(){rg_battery_t b=rg_input_read_battery();return b.charging?1:-1;} float calibrate(float){return 0;} }
