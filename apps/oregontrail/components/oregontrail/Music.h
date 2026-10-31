#pragma once
#include <stddef.h>
#include "hw/Audio.h"
namespace music { struct Song{const audio::Note*notes;size_t len;}; extern Song landmark[18]; extern Song tombstone; bool loadAll(); }
