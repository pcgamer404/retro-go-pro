#include "AssetPaths.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <config.h>
#include <rg_storage.h>
#include <rg_system.h>
#include <rg_storage.h>
namespace { char root[256] = RG_BASE_PATH_ROMS "/oregontrail"; }
const char *ot_data_root(){return root;}
void ot_set_data_root(const char *path){if(!path||!*path)return;snprintf(root,sizeof(root),"%s",path);}
void ot_build_path(char *out,size_t n,const char *rel){snprintf(out,n,"%s/%s",root,rel);}
