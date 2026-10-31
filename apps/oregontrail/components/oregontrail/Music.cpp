#include "Music.h"
#include "AssetPaths.h"
#include <cJSON.h>
#include <config.h>
#include <rg_storage.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <math.h>
namespace music {
Song landmark[18]{}; Song tombstone{};
static bool loadSong(const char *path, Song &out) {
    void *data=nullptr; size_t len=0;
    if (!rg_storage_read_file(path,&data,&len,0)) return false;
    char *txt=(char*)malloc(len+1); if(!txt){free(data);return false;}
    memcpy(txt,data,len); txt[len]=0; free(data);
    cJSON *root=cJSON_Parse(txt); free(txt); if(!root)return false;
    cJSON *events=cJSON_GetObjectItem(root,"events");
    if(!cJSON_IsArray(events)){cJSON_Delete(root);return false;}
    int n=cJSON_GetArraySize(events); audio::Note *notes=(audio::Note*)calloc(n,sizeof(audio::Note));
    if(!notes){cJSON_Delete(root);return false;}
    for(int i=0;i<n;++i){
        cJSON *e=cJSON_GetArrayItem(events,i); cJSON *hz=cJSON_GetObjectItem(e,"hz"); cJSON *ms=cJSON_GetObjectItem(e,"ms");
        int h=hz&&cJSON_IsNumber(hz)?(int)llround(hz->valuedouble):0; int m=ms&&cJSON_IsNumber(ms)?(int)llround(ms->valuedouble):0;
        notes[i].hz=(uint16_t)std::max(0,std::min(65535,h)); notes[i].ms=(uint16_t)std::max(0,std::min(65535,m));
    }
    cJSON_Delete(root); out={notes,(size_t)n}; return true;
}
struct LoadCtx{Song*songs;int count;};
static int music_cb(const rg_scandir_t *f, void *arg) {
    if (!f->is_file || !f->basename)
        return RG_SCANDIR_CONTINUE;

    const char *dot = strrchr(f->basename, '.');
    if (!dot || strcmp(dot, ".json"))
        return RG_SCANDIR_CONTINUE;

    LoadCtx *ctx = (LoadCtx *)arg;
    if (ctx->count >= 18)
        return RG_SCANDIR_STOP;

    if (loadSong(f->path, ctx->songs[ctx->count]))
        ctx->count++;

    return RG_SCANDIR_CONTINUE;
}
bool loadAll(){char dir[320];snprintf(dir,sizeof(dir),"%s/music/landmarks",ot_data_root());LoadCtx ctx{landmark,0};bool ok=rg_storage_scandir(dir,music_cb,&ctx,RG_SCANDIR_FILES|RG_SCANDIR_SORT|RG_SCANDIR_STAT);char p[320];snprintf(p,sizeof(p),"%s/music/tombstone.json",ot_data_root());bool tombOk=loadSong(p,tombstone);return ok&&ctx.count==18&&tombOk;}
}
