#pragma once
#include <stdint.h>
#include "compat/Arduino.h"
#include "compat/LovyanGFX.hpp"
namespace touch { struct Point{int16_t x,y;bool down,pressed,backPressed,upPressed,downPressed,leftPressed,rightPressed,menuPressed;}; void begin();void diag();bool isCalibrated();bool runCalibration(LGFX_Sprite&);Point poll();bool rawSample(int16_t*,int16_t*);bool isTouched();void busResume();int16_t cursorX();int16_t cursorY(); }
