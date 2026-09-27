#include "hw/Audio.h"
#include <rg_audio.h>
#include <rg_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>
namespace audio {
namespace { constexpr int RATE=22050; volatile bool s_ready=false,s_muted=false,s_click=false; int s_level=7; const Note*s_song=nullptr; size_t s_len=0,s_pos=0; bool s_loop=false; TaskHandle_t s_task=nullptr; portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
void submitTone(uint16_t hz,int ms,int volume){if(ms<=0)return;size_t n=(size_t)RATE*ms/1000;const size_t chunk=256;rg_audio_frame_t frames[chunk];double phase=0,step=hz?2.0*M_PI*hz/RATE:0;for(size_t done=0;done<n;){size_t c=(n-done)>chunk?chunk:n-done;for(size_t i=0;i<c;++i){int16_t v=hz?(int16_t)(sinf(phase)*2400.0f*volume/10.0f):0;phase+=step;frames[i].left=v;frames[i].right=v;}rg_audio_submit(frames,c);done+=c;if(s_muted)vTaskDelay(1);}}
void task(void*){for(;;){const Note*song;size_t len,pos;bool loop;portENTER_CRITICAL(&mux);song=s_song;len=s_len;pos=s_pos;loop=s_loop;portEXIT_CRITICAL(&mux);if(s_click){s_click=false;submitTone(1047,35,s_level);continue;}if(!song||!len){vTaskDelay(pdMS_TO_TICKS(5));continue;}if(pos>=len){if(loop)pos=0;else{portENTER_CRITICAL(&mux);s_song=nullptr;s_len=0;s_pos=0;portEXIT_CRITICAL(&mux);continue;}}const Note n=song[pos++];portENTER_CRITICAL(&mux);s_pos=pos;portEXIT_CRITICAL(&mux);submitTone(n.hz,n.ms,s_level);}}
}
bool begin(){if(s_ready)return true;rg_audio_set_sample_rate(RATE);s_ready=true;rg_task_create("ot_audio",task,nullptr,4096,1,RG_TASK_PRIORITY_1,0);return true;} bool ready(){return s_ready;} void setLevel(int l){s_level=l<0?0:l>10?10:l;rg_audio_set_volume(s_level*10);} int level(){return s_level;} void setMuted(bool m){s_muted=m;rg_audio_set_mute(m);} bool muted(){return s_muted;}
void playSong(const Note*n,size_t count,bool loop){if(!n||!count)return;portENTER_CRITICAL(&mux);s_song=n;s_len=count;s_pos=0;s_loop=loop;portEXIT_CRITICAL(&mux);} void stopSong(){portENTER_CRITICAL(&mux);s_song=nullptr;s_len=0;s_pos=0;s_loop=false;portEXIT_CRITICAL(&mux);} bool songPlaying(){return s_song!=nullptr;} void click(){s_click=true;} void testChime(){static const Note n[]={{784,80},{0,25},{1047,120}};playSong(n,3,false);}
}
