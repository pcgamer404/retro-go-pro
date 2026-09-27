#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "compat/LovyanGFX.hpp"
#include "AssetPaths.h"
namespace art {
inline int landmarkIndexForNode(int node){if(node<=16)return node;if(node==19)return 17;return 16;}
inline bool drawRel(LGFX_Sprite&g,const char*rel,int x,int y,int w=0,int h=0,float sx=1.0f,float sy=1.0f,int offX=0,int offY=0){char p[320];ot_build_path(p,sizeof(p),rel);return g.drawPngFile(p,x,y,w,h,offX,offY,sx,sy);}
inline int16_t drawLandmark(LGFX_Sprite&g,int node,int16_t y,int16_t h=164){char rel[96];snprintf(rel,sizeof(rel),"art/landmarks/p%d.png",landmarkIndexForNode(node));drawRel(g,rel,0,y,320,h);return y+h;}
inline void drawWagon(LGFX_Sprite&g,int frame,int16_t x,int16_t y,float scale=1.0f){int i=frame<0?0:frame>4?4:frame;char p[96];snprintf(p,sizeof(p),"art/sprites/travelox/%02d.png",i+1);drawRel(g,p,x,y,0,0,scale,scale);}
inline void drawHunter(LGFX_Sprite&g,int pose,int16_t x,int16_t y,float scale=1.0f,bool=false){int i=((pose%24)+24)%24;char p[96];snprintf(p,sizeof(p),"art/sprites/hunter/%02d.png",i+1);drawRel(g,p,x,y,0,0,scale,scale);}
inline void drawAnimal(LGFX_Sprite&g,int frame,int16_t x,int16_t y){int i=((frame%48)+48)%48;char p[96];snprintf(p,sizeof(p),"art/sprites/animals/%02d.png",i+1);drawRel(g,p,x,y);}
inline void drawTerrain(LGFX_Sprite&g,int i,int16_t x,int16_t y){i=((i%14)+14)%14;char p[96];snprintf(p,sizeof(p),"art/sprites/terrain/%02d.png",i+1);drawRel(g,p,x,y);}
inline void drawScenery(LGFX_Sprite&g,int i,int16_t x,int16_t y){i=((i%17)+17)%17;char p[96];snprintf(p,sizeof(p),"art/sprites/scenery/%02d.png",i+1);drawRel(g,p,x,y);}
inline void drawEvent(LGFX_Sprite&g,int n,int16_t x,int16_t y,float scale=1.0f){n=n<1?1:n>7?7:n;char p[96];snprintf(p,sizeof(p),"art/sprites/events/%02d.png",n);drawRel(g,p,x,y,0,0,scale,scale);}

inline void drawLandmarkIndex(LGFX_Sprite&g,int i,int16_t x,int16_t y,int w=0,int h=0,float sx=1.0f,float sy=1.0f){char rel[96];snprintf(rel,sizeof(rel),"art/landmarks/p%d.png",i<0?0:i>17?17:i);drawRel(g,rel,x,y,w,h,sx,sy);}
inline void drawMap(LGFX_Sprite&g,int16_t x,int16_t y,int w,int h,int offX,int offY,float scale){drawRel(g,"art/map.png",x,y,w,h,scale,scale,offX,offY);}
inline void drawFamily(LGFX_Sprite&g,int16_t x,int16_t y,float scale=1.0f){drawRel(g,"art/family.png",x,y,0,0,scale,scale);}
inline void drawBanner(LGFX_Sprite&g,int16_t x,int16_t y){drawRel(g,"art/banner.png",x,y);}
inline void drawTomb(LGFX_Sprite&g,int16_t x,int16_t y,float scale=1.0f){drawRel(g,"art/tombstone.png",x,y,0,0,scale,scale);}
namespace col { constexpr uint16_t sky=0x6D3F,ground=0x5D66,dirt=0x9B85; }
}
