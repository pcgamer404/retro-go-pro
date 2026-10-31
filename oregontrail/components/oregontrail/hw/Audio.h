#pragma once
#include <stddef.h>
#include <stdint.h>
namespace audio { struct Note{uint16_t hz;uint16_t ms;}; bool begin();bool ready();void setLevel(int);int level();void setMuted(bool);bool muted();void playSong(const Note*,size_t,bool loop=false);void stopSong();bool songPlaying();void click();void testChime(); }
