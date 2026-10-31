#pragma once
// RG Ports - built into the launcher. Self-contained: everything lives in rg_ports.c.
// Build switch: -DRG_ENABLE_PORTS=0 (see launcher/CMakeLists.txt) removes it completely.

#include "applications.h"

#ifndef RG_ENABLE_PORTS
#define RG_ENABLE_PORTS 1
#endif

#define RG_PORTS_NAME        "rgports"    // tab id + folder name: SD:/roms/rgports/
#define RG_PORTS_TITLE       "RG Ports"   // tab title
#define RG_PORTS_PARTITION   "portslot"   // shared slot partition that ports are flashed into

#if RG_ENABLE_PORTS
// true if this file belongs to the RG Ports tab
bool rg_ports_owns(const retro_file_t *file);
// Replacement for the standard file menu, used for RG Ports files only
void rg_ports_file_menu(retro_file_t *file);
#else
static inline bool rg_ports_owns(const retro_file_t *file) { (void)file; return false; }
static inline void rg_ports_file_menu(retro_file_t *file) { (void)file; }
#endif
