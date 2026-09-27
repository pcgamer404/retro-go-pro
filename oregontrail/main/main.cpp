#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <esp_system.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <rg_system.h>
#include <rg_display.h>
#include <rg_input.h>
#include <rg_storage.h>
#include <rg_gui.h>
#include <rg_settings.h>
#include "compat/LovyanGFX.hpp"
#include "AssetPaths.h"
#include "Settings.h"
#include "SaveGame.h"
#include "Music.h"
#include "game/Session.h"
#include "game/Sim.h"
#include "game/Store.h"
#include "hw/Audio.h"
#include "hw/Battery.h"
#include "hw/Storage.h"
#include "hw/Touch.h"
#include "ui/App.h"
#include "ui/ScreenStack.h"
#include "ui/Theme.h"
#include "screens/TitleScreen.h"
#include "screens/MainMenuScreen.h"
#include "screens/MonthScreen.h"
#include "screens/NameEntryScreen.h"
#include "screens/ProfessionScreen.h"
#include "screens/EventScreen.h"
#include "screens/GameOverScreen.h"
#include "screens/StoreScreen.h"
#include "screens/TrailMenuScreen.h"

namespace app { ScreenStack screens; bool wantRecal=false,wantSleep=false,wantRestart=false; void setBrightness(int level){rg_display_set_backlight((display_backlight_t)(level<1?1:level>100?100:level));} }
static LGFX_Sprite frames[2]; static LGFX_Sprite* frame=&frames[0]; static int renderIndex=0; static uint32_t lastFrame=0;
static bool resolve_data_root(char*out,size_t n){rg_app_t*app=rg_system_get_app();const char*sel=app?app->romPath:nullptr;if(sel&&*sel){struct stat st;if(stat(sel,&st)==0&&S_ISDIR(st.st_mode)){snprintf(out,n,"%s",sel);return true;}snprintf(out,n,"%s",sel);char*slash=strrchr(out,'/');if(slash)*slash=0;else {slash=strrchr(out,'\\');if(slash)*slash=0;}if(out[0])return true;}snprintf(out,n,"%s/oregontrail",RG_BASE_PATH_ROMS);return true;}
static bool screenshot_handler(const char*filename,int w,int h){rg_display_sync();return rg_surface_save_image_file(frame->surface(),filename,w,h);} static bool save_state_handler(const char*){savegame::save();return true;} static bool load_state_handler(const char*){return savegame::load();} static bool reset_handler(bool){rg_system_restart();return true;}
static void event_handler(int event,void*){if(event==RG_EVENT_SHUTDOWN)audio::setMuted(true);}
static void options_handler(rg_gui_option_t*dest){*dest++=(rg_gui_option_t){0,"Brightness","-",RG_DIALOG_FLAG_NORMAL,[](rg_gui_option_t*o,rg_gui_event_t e){if(e==RG_DIALOG_PREV||e==RG_DIALOG_NEXT){int v=(int)rg_settings_get_number(NS_APP,"ot_brightness",100)+(e==RG_DIALOG_NEXT?10:-10);if(v<10)v=10;if(v>100)v=100;rg_settings_set_number(NS_APP,"ot_brightness",v);rg_settings_commit();app::setBrightness(v);return RG_DIALOG_REDRAW;}snprintf(o->value,16,"%d%%",(int)rg_settings_get_number(NS_APP,"ot_brightness",100));return RG_DIALOG_VOID;}};*dest++=(rg_gui_option_t){0,"Volume","-",RG_DIALOG_FLAG_NORMAL,[](rg_gui_option_t*o,rg_gui_event_t e){if(e==RG_DIALOG_PREV||e==RG_DIALOG_NEXT){int v=(int)rg_settings_get_number(NS_APP,"ot_volume",7)+(e==RG_DIALOG_NEXT?1:-1);if(v<0)v=0;if(v>10)v=10;rg_settings_set_number(NS_APP,"ot_volume",v);rg_settings_commit();audio::setLevel(v);return RG_DIALOG_REDRAW;}snprintf(o->value,16,"%d/10",(int)rg_settings_get_number(NS_APP,"ot_volume",7));return RG_DIALOG_VOID;}};*dest++=RG_DIALOG_END;}
static void oregon_task(void*){uint32_t prev=0;for(;;){uint32_t now=rg_system_timer();uint32_t dt=prev?(uint32_t)((now-prev)/1000):33;prev=now;if(dt>100)dt=100;battery::update();auto p=touch::poll();if(p.menuPressed){audio::click();rg_gui_game_menu();}else if(p.backPressed){audio::click();if(app::screens.depth()>1)app::screens.pop();}else {
if(p.upPressed)app::screens.dispatchDpad(0,-1);
if(p.downPressed)app::screens.dispatchDpad(0,1);
if(p.leftPressed)app::screens.dispatchDpad(-1,0);
if(p.rightPressed)app::screens.dispatchDpad(1,0);
if(p.pressed){audio::click();app::screens.dispatchConfirm(p.x,p.y);}}// rg_display_submit() queues the surface pointer for the display task.  The
// display task reads that same buffer asynchronously, so rendering into it
// again before the previous transfer finishes can cause visible tearing/flicker.
// Wait for the previous frame to finish before reusing the framebuffer.
rg_display_sync();
app::screens.update(dt,*frame);
rg_display_submit(frame->surface(),0);
renderIndex ^= 1;
frame = &frames[renderIndex];
rg_system_tick(0);vTaskDelay(pdMS_TO_TICKS(16));}}
extern "C" void app_main(){const rg_config_t config={.sampleRate=22050,.frameRate=60,.storageRequired=true,.romRequired=false,.handlers={.loadState=load_state_handler,.saveState=save_state_handler,.reset=reset_handler,.screenshot=screenshot_handler,.event=event_handler,.options=options_handler},.mallocAlwaysInternal=0};rg_app_t*appx=rg_system_init(&config);char root[256];resolve_data_root(root,sizeof(root));ot_set_data_root(root);rg_storage_mkdir(root);rg_storage_mkdir(RG_BASE_PATH_SAVES "/oregontrail");prefs::load();audio::begin();audio::setLevel(prefs::volume);battery::begin();touch::begin();if(!music::loadAll())RG_LOGW("Oregon Trail music assets incomplete in %s\n",ot_data_root());game::seedRng(esp_random());if(!frames[0].createSprite(320,240)||!frames[1].createSprite(320,240))rg_system_panic("Oregon Trail","Could not allocate frame surfaces");frame=&frames[0];frame->fillScreen(theme::BG);frames[1].fillScreen(theme::BG);app::screens.reset(new TitleScreen());if(!rg_settings_exists(NS_APP,"DispScaling"))rg_display_set_scaling(RG_DISPLAY_SCALING_FULL);rg_task_create("oregontrail",oregon_task,nullptr,24*1024,1,RG_TASK_PRIORITY_1,0);while(1)vTaskDelay(pdMS_TO_TICKS(1000));}
