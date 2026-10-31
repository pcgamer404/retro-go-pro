#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdlib.h>
#include <algorithm>
#include <rg_surface.h>
#include <rg_display.h>
#include <rg_system.h>
#include <rg_gui.h>

/* Retro-Go builds LodePNG once in components/retro-go/libs/lodepng.
 * LovyanGFX.hpp is compiled from the main component, so the Oregon Trail
 * component's force-included C compatibility header is not visible here.
 * Declare only the C decoder entry point needed by this C++ compatibility
 * layer; do not include the mixed C/C++ lodepng.h header here. */
#ifdef __cplusplus
extern "C" {
#endif
unsigned lodepng_decode32(unsigned char** out,
                           unsigned* w,
                           unsigned* h,
                           const unsigned char* in,
                           size_t insize);
#ifdef __cplusplus
}
#endif
extern const rg_font_t font_DejaVu12;
extern const rg_font_t font_DejaVu15;
#include "Arduino.h"
namespace lgfx { struct IFont {}; }
namespace fonts { static const lgfx::IFont Font2{}; static const lgfx::IFont Font4{}; static const lgfx::IFont FreeSansBold9pt7b{}; }
enum class textdatum_t { top_left,top_center,top_right,middle_left,middle_center,middle_right,bottom_left,bottom_center,bottom_right };
class LGFX_Sprite {
 struct CachedImage { char path[320]; uint16_t *rgb565; uint8_t *alpha; unsigned w,h; CachedImage():rgb565(nullptr),alpha(nullptr),w(0),h(0){path[0]=0;} };
 inline static CachedImage cache_[96]; inline static int cacheCount_=0;
 rg_surface_t*s_=nullptr; const lgfx::IFont*font_=&fonts::Font2; textdatum_t datum_=textdatum_t::top_left; uint16_t fg_=0xffff,bg_=0;
 bool in(int x,int y)const{return s_&&x>=0&&y>=0&&x<s_->width&&y<s_->height;} uint16_t*pix(int x,int y){return(uint16_t*)((uint8_t*)s_->data+s_->offset+y*s_->stride)+x;}
 const rg_font_t*font()const{return font_==&fonts::Font4?&font_DejaVu15:&font_DejaVu12;}
 const rg_font_glyph_t*glyph(uint32_t c)const{const uint8_t*p=font()->data;auto*g=(const rg_font_glyph_t*)p;while(g->code&&g->code!=c){if(g->width)p+=(((g->width*g->height)-1)/8)+1;p+=sizeof(rg_font_glyph_t);g=(const rg_font_glyph_t*)p;}return g->code==c?g:nullptr;}
 int gw(uint32_t c)const{auto*g=glyph(c);return g?std::max((int)g->width,(int)g->xDelta):font()->width;}
 int fh()const{return font()->height;}
 int glyphDraw(int x,int y,uint32_t c,uint16_t col){auto*g=glyph(c);if(!g)return font()->width;int xo=g->xOffset<0x80?g->xOffset:-(0xff-g->xOffset);const uint8_t*d=g->data;for(int yy=0;yy<g->height;++yy)for(int xx=0;xx<g->width;++xx){size_t bit=(size_t)yy*g->width+xx;if(d[bit/8]&(0x80>>(bit&7))){int px=x+xo+xx,py=y+g->yOffset+yy;if(in(px,py))*pix(px,py)=col;}}return std::max((int)g->width,(int)g->xDelta);}
 void textDraw(const char*t,int x,int y){if(!t)return;int w=textWidth(t),h=fh(),sx=x,sy=y;switch(datum_){case textdatum_t::top_center:sx-=w/2;break;case textdatum_t::top_right:sx-=w;break;case textdatum_t::middle_left:sy-=h/2;break;case textdatum_t::middle_center:sx-=w/2;sy-=h/2;break;case textdatum_t::middle_right:sx-=w;sy-=h/2;break;case textdatum_t::bottom_center:sx-=w/2;sy-=h;break;case textdatum_t::bottom_right:sx-=w;sy-=h;break;default:break;}for(const unsigned char*p=(const unsigned char*)t;*p;++p){if(*p=='\n'){sy+=h;sx=x;continue;}sx+=glyphDraw(sx,sy,*p,fg_);}}
public:
 explicit LGFX_Sprite(void * = nullptr){} ~LGFX_Sprite(){if(s_)rg_surface_free(s_);} void setColorDepth(int){} void setPsram(bool){}
 bool createSprite(int w,int h){if(s_)rg_surface_free(s_);s_=rg_surface_create(w,h,RG_PIXEL_565_LE,MEM_SLOW);return s_!=nullptr;} int width()const{return s_?s_->width:0;} int height()const{return s_?s_->height:0;} void*getBuffer(){return s_?s_->data:nullptr;} rg_surface_t*surface()const{return s_;}
 void pushSprite(int,int){if(s_)rg_display_submit(s_,0);} void fillScreen(uint16_t c){fillRect(0,0,width(),height(),c);}
 void fillRect(int x,int y,int w,int h,uint16_t c){if(!s_)return;int x0=std::max(0,x),y0=std::max(0,y),x1=std::min(width(),x+w),y1=std::min(height(),y+h);for(int yy=y0;yy<y1;++yy){auto*p=pix(x0,yy);for(int xx=x0;xx<x1;++xx)*p++=c;}}
 void drawFastHLine(int x,int y,int w,uint16_t c){fillRect(x,y,w,1,c);} void drawFastVLine(int x,int y,int h,uint16_t c){fillRect(x,y,1,h,c);}
 void drawRect(int x,int y,int w,int h,uint16_t c){drawFastHLine(x,y,w,c);drawFastHLine(x,y+h-1,w,c);drawFastVLine(x,y,h,c);drawFastVLine(x+w-1,y,h,c);}
 void drawLine(int x0,int y0,int x1,int y1,uint16_t c){int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,err=dx+dy;for(;;){if(in(x0,y0))*pix(x0,y0)=c;if(x0==x1&&y0==y1)break;int e=2*err;if(e>=dy){err+=dy;x0+=sx;}if(e<=dx){err+=dx;y0+=sy;}}}
 void drawCircle(int cx,int cy,int r,uint16_t c){for(int a=0;a<360;a+=2){double q=a*3.141592653589793/180.0;int x=cx+(int)lround(cos(q)*r),y=cy+(int)lround(sin(q)*r);if(in(x,y))*pix(x,y)=c;}}
 void fillCircle(int cx,int cy,int r,uint16_t c){for(int y=-r;y<=r;++y){int dx=(int)sqrt((double)r*r-y*y);fillRect(cx-dx,cy+y,2*dx+1,1,c);}}
 void fillEllipse(int cx,int cy,int rx,int ry,uint16_t c){if(ry<=0)return;for(int y=-ry;y<=ry;++y){int dx=(int)(rx*sqrt(std::max(0.0,1.0-(double)y*y/(double)(ry*ry))));fillRect(cx-dx,cy+y,2*dx+1,1,c);}}
 void fillTriangle(int x0,int y0,int x1,int y1,int x2,int y2,uint16_t c){int minx=std::min({x0,x1,x2}),maxx=std::max({x0,x1,x2}),miny=std::min({y0,y1,y2}),maxy=std::max({y0,y1,y2});auto e=[](int ax,int ay,int bx,int by,int px,int py){return(px-ax)*(by-ay)-(py-ay)*(bx-ax);};for(int y=miny;y<=maxy;++y)for(int x=minx;x<=maxx;++x){int a=e(x0,y0,x1,y1,x,y),b=e(x1,y1,x2,y2,x,y),d=e(x2,y2,x0,y0,x,y);if((a>=0&&b>=0&&d>=0)||(a<=0&&b<=0&&d<=0))if(in(x,y))*pix(x,y)=c;}}
 void drawRoundRect(int x,int y,int w,int h,int r,uint16_t c){drawRect(x+r,y,w-2*r,h,c);drawRect(x,y+r,w,h-2*r,c);drawCircle(x+r,y+r,r,c);drawCircle(x+w-r-1,y+r,r,c);drawCircle(x+r,y+h-r-1,r,c);drawCircle(x+w-r-1,y+h-r-1,r,c);}
 void fillRoundRect(int x,int y,int w,int h,int r,uint16_t c){fillRect(x+r,y,w-2*r,h,c);fillRect(x,y+r,w,h-2*r,c);fillCircle(x+r,y+r,r,c);fillCircle(x+w-r-1,y+r,r,c);fillCircle(x+r,y+h-r-1,r,c);fillCircle(x+w-r-1,y+h-r-1,r,c);}
 void setFont(const lgfx::IFont*f){font_=f?f:&fonts::Font2;} void setTextColor(uint16_t c){fg_=c;} void setTextColor(uint16_t f,uint16_t b){fg_=f;bg_=b;} void setTextDatum(textdatum_t d){datum_=d;}
 int textWidth(const char*t){int w=0;if(!t)return 0;for(const unsigned char*p=(const unsigned char*)t;*p;++p)if(*p!='\n')w+=gw(*p);return w;} int textWidth(const String&s){return textWidth(s.c_str());}
 void drawString(const char*t,int x,int y){textDraw(t,x,y);} void drawString(const String&s,int x,int y){textDraw(s.c_str(),x,y);}
 bool drawPng(const uint8_t*data,size_t len,int x,int y,int w=0,int h=0,int offX=0,int offY=0,int=0,float sx=1.0f,float sy=1.0f){if(!s_||!data||!len)return false;unsigned char*rgba=nullptr;unsigned pw=0,ph=0;if(lodepng_decode32(&rgba,&pw,&ph,data,len))return false;int dw=w>0?w:(int)lroundf(pw*sx),dh=h>0?h:(int)lroundf(ph*sy);if(dw<=0)dw=pw;if(dh<=0)dh=ph;double startX=sx>0?(double)offX/sx:0.0,startY=sy>0?(double)offY/sy:0.0;for(int yy=0;yy<dh;++yy){int syi=(int)(startY+yy/sy);if(syi<0||syi>=(int)ph)continue;for(int xx=0;xx<dw;++xx){int sxi=(int)(startX+xx/sx);if(sxi<0||sxi>=(int)pw)continue;const uint8_t*q=rgba+((size_t)syi*pw+sxi)*4;if(q[3]<16)continue;if(in(x+xx,y+yy)){uint16_t rgb=((q[0]&248)<<8)|((q[1]&252)<<3)|(q[2]>>3);if(q[3]<255){uint16_t old=*pix(x+xx,y+yy);int a=q[3];rgb=(uint16_t)(((((rgb>>11)*a)+((old>>11)*(255-a)))/255)<<11)|((((((rgb>>5)&63)*a)+(((old>>5)&63)*(255-a)))/255)<<5)|((((rgb&31)*a)+((old&31)*(255-a)))/255);}*pix(x+xx,y+yy)=rgb;}}}free(rgba);return true;}
 bool drawPngFile(const char*path,int x,int y,int w=0,int h=0,int offX=0,int offY=0,float sx=1.0f,float sy=1.0f){
  if(!path||!s_)return false;
  CachedImage *ci=nullptr;
  for(int i=0;i<cacheCount_;++i) if(strcmp(cache_[i].path,path)==0){ci=&cache_[i];break;}
  if(!ci){
    void*d=nullptr;size_t n=0;if(!rg_storage_read_file(path,&d,&n,0))return false;
    unsigned char*rgba=nullptr;unsigned pw=0,ph=0;if(lodepng_decode32(&rgba,&pw,&ph,(const unsigned char*)d,n)){free(d);return false;}free(d);
    if(cacheCount_<96){ci=&cache_[cacheCount_++];snprintf(ci->path,sizeof(ci->path),"%s",path);ci->w=pw;ci->h=ph;const size_t count=(size_t)pw*ph;ci->rgb565=(uint16_t*)malloc(count*sizeof(uint16_t));ci->alpha=(uint8_t*)malloc(count);if(!ci->rgb565||!ci->alpha){free(ci->rgb565);free(ci->alpha);ci->rgb565=nullptr;ci->alpha=nullptr;--cacheCount_;free(rgba);return false;}for(size_t i=0;i<count;++i){const uint8_t*q=rgba+i*4;ci->rgb565[i]=(uint16_t)(((q[0]&248)<<8)|((q[1]&252)<<3)|(q[2]>>3));ci->alpha[i]=q[3];}free(rgba);}else{free(rgba);return false;}
  }
  unsigned pw=ci->w,ph=ci->h; int dw=w>0?w:(int)lroundf(pw*sx),dh=h>0?h:(int)lroundf(ph*sy);if(dw<=0)dw=pw;if(dh<=0)dh=ph;
  const int isx=(sx>0.0f&&fabsf(sx-lroundf(sx))<0.001f)?(int)lroundf(sx):0;
  const int isy=(sy>0.0f&&fabsf(sy-lroundf(sy))<0.001f)?(int)lroundf(sy):0;
  const int x0=isx?offX/isx:0, y0=isy?offY/isy:0;
  for(int yy=0;yy<dh;++yy){int syi=(isx&&isy)?y0+yy/isy:(int)((sy>0.0f?yy/sy:0.0f)+(sy>0.0f?(double)offY/sy:0.0));if(syi<0||syi>=(int)ph)continue;for(int xx=0;xx<dw;++xx){int sxi=(isx&&isy)?x0+xx/isx:(int)((sx>0.0f?xx/sx:0.0f)+(sx>0.0f?(double)offX/sx:0.0));if(sxi<0||sxi>=(int)pw)continue;const size_t pi=(size_t)syi*pw+sxi;const uint8_t a=ci->alpha[pi];if(a<16)continue;if(in(x+xx,y+yy)){uint16_t rgb=ci->rgb565[pi];if(a<255){uint16_t old=*pix(x+xx,y+yy);const int ia=255-a;const int r=((rgb>>11)*a+(old>>11)*ia+127)/255;const int gg=(((rgb>>5)&63)*a+((old>>5)&63)*ia+127)/255;const int b=((rgb&31)*a+(old&31)*ia+127)/255;rgb=(uint16_t)((r<<11)|(gg<<5)|b);}*pix(x+xx,y+yy)=rgb;}}}return true;
 }
};
class LGFX {public:void init(){}void setRotation(int){}void setBrightness(int v){rg_display_set_backlight((display_backlight_t)std::max(1,std::min(100,v*100/255)));}void fillScreen(uint16_t c){rg_display_clear(c);}};
