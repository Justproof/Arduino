#pragma once
#include <lvgl.h>

void clock_app_init(const char* wifi_ssid, const char* wifi_pass, const char* tz_string);
void clock_app_create_ui(void);

/** True once SNTP has set the system clock. */
bool clock_app_time_synced(void);

/** True once WiFi has powered down and BLE may safely claim the radio. */
bool clock_app_radio_free(void);
