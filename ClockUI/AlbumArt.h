#pragma once
#include <lvgl.h>

void AlbumArt_Init(lv_obj_t* parent);
void AlbumArt_Tick();   // called from LVGL task to apply any newly-fetched art
