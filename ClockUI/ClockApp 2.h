#pragma once
#include <lvgl.h>

void clock_app_init(const char* wifi_ssid, const char* wifi_pass, const char* tz_string);
void clock_app_create_ui(void);
