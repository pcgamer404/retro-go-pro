#pragma once
#include "compat/LovyanGFX.hpp"
#include <rg_surface.h>
#include <rg_storage.h>
namespace screenshot {
inline void dump(LGFX_Sprite& frame) {
    char path[256];
    snprintf(path,sizeof(path),"%s/oregontrail/frame.png",RG_BASE_PATH_SAVES);
    rg_storage_mkdir(RG_BASE_PATH_SAVES "/oregontrail");
    rg_surface_save_image_file(frame.surface(),path,320,240);
}
}
