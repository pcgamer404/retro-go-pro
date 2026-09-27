#include "hw/Touch.h"
#include <rg_input.h>
#include <rg_system.h>
#include "hw/Display.h"
#include "ui/Theme.h"
namespace touch { namespace { int16_t x=160,y=120; uint32_t last=0; uint32_t prev=0; bool aPrev=false; }
void begin(){x=160;y=120;last=millis();} void diag(){} bool isCalibrated(){return true;} bool runCalibration(LGFX_Sprite&){return true;} bool rawSample(int16_t*,int16_t*){return false;} bool isTouched(){return false;} void busResume(){}
Point poll(){uint32_t keys=rg_input_read_gamepad();uint32_t now=millis();uint32_t dt=now-last;last=now;int step=dt>100?12:6;bool up=(keys&RG_KEY_UP)!=0,down=(keys&RG_KEY_DOWN)!=0,left=(keys&RG_KEY_LEFT)!=0,right=(keys&RG_KEY_RIGHT)!=0;bool upPressed=up&&!(prev&RG_KEY_UP);bool downPressed=down&&!(prev&RG_KEY_DOWN);bool leftPressed=left&&!(prev&RG_KEY_LEFT);bool rightPressed=right&&!(prev&RG_KEY_RIGHT);if(left)x-=step;if(right)x+=step;if(up)y-=step;if(down)y+=step;if(x<0)x=0;if(x>=OT_W)x=OT_W-1;if(y<0)y=0;if(y>=OT_H)y=OT_H-1;bool a=(keys&RG_KEY_A)!=0;bool pressed=a&&!aPrev;aPrev=a;bool backPressed=(keys&RG_KEY_B)&&!(prev&RG_KEY_B);
bool menuPressed=(keys&RG_KEY_MENU)&&!(prev&RG_KEY_MENU);
prev=keys;return{x,y,a,pressed,backPressed,upPressed,downPressed,leftPressed,rightPressed,menuPressed};}
int16_t cursorX(){return x;} int16_t cursorY(){return y;}
}
