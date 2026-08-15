#include "ClockApp.h"
#include "RTC_PCF85063.h"
#include <WiFi.h>
#include <time.h>
#include <esp_sntp.h>

static lv_obj_t* lbl_time   = nullptr;
static lv_obj_t* lbl_date   = nullptr;
static lv_obj_t* lbl_status = nullptr;
static lv_obj_t* meter      = nullptr;
static lv_meter_indicator_t* hand_h = nullptr;
static lv_meter_indicator_t* hand_m = nullptr;
static lv_meter_indicator_t* hand_s = nullptr;

static volatile bool ntp_synced = false;

static void on_ntp_sync(struct timeval*) {
  ntp_synced = true;
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);
  datetime_t d = {
    (uint16_t)(t.tm_year + 1900),
    (uint8_t)(t.tm_mon + 1),
    (uint8_t)t.tm_mday,
    (uint8_t)t.tm_wday,
    (uint8_t)t.tm_hour,
    (uint8_t)t.tm_min,
    (uint8_t)t.tm_sec
  };
  PCF85063_Set_All(d);
}

static void tick_cb(lv_timer_t*) {
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);

  if (t.tm_year < 120) {
    if (lbl_status) {
      lv_label_set_text(lbl_status,
        WiFi.status() == WL_CONNECTED ? "syncing NTP..." : "no WiFi");
    }
    return;
  }

  if (lbl_status) lv_label_set_text(lbl_status, "");

  char buf[32];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  lv_label_set_text(lbl_time, buf);
  strftime(buf, sizeof(buf), "%a %d %b %Y", &t);
  lv_label_set_text(lbl_date, buf);

  if (meter) {
    lv_meter_set_indicator_value(meter, hand_s, t.tm_sec);
    lv_meter_set_indicator_value(meter, hand_m, t.tm_min);
    lv_meter_set_indicator_value(meter, hand_h, (t.tm_hour % 12) * 5 + t.tm_min / 12);
  }
}

static void build_digital(lv_obj_t* page) {
  lv_obj_set_style_pad_all(page, 10, 0);
  lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lbl_time = lv_label_create(page);
  lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_48, 0);
  lv_label_set_text(lbl_time, "--:--:--");

  lbl_date = lv_label_create(page);
  lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_20, 0);
  lv_obj_set_style_pad_top(lbl_date, 10, 0);
  lv_label_set_text(lbl_date, "");

  lbl_status = lv_label_create(page);
  lv_obj_set_style_text_color(lbl_status, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_set_style_pad_top(lbl_status, 20, 0);
  lv_label_set_text(lbl_status, "booting...");
}

static void build_analog(lv_obj_t* page) {
  meter = lv_meter_create(page);
  lv_obj_center(meter);
  lv_obj_set_size(meter, 220, 220);

  lv_meter_scale_t* scale = lv_meter_add_scale(meter);
  lv_meter_set_scale_ticks(meter, scale, 61, 1, 8, lv_palette_main(LV_PALETTE_GREY));
  lv_meter_set_scale_major_ticks(meter, scale, 5, 2, 16, lv_color_black(), 8);
  lv_meter_set_scale_range(meter, scale, 0, 60, 360, 270);

  hand_h = lv_meter_add_needle_line(meter, scale, 7, lv_color_black(), -30);
  hand_m = lv_meter_add_needle_line(meter, scale, 5, lv_palette_main(LV_PALETTE_GREY), -15);
  hand_s = lv_meter_add_needle_line(meter, scale, 2, lv_palette_main(LV_PALETTE_RED), 0);
}

static void wifi_task(void* arg) {
  const char** creds = (const char**)arg;
  const char* ssid = creds[0];
  const char* pass = creds[1];

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  if (WiFi.status() == WL_CONNECTED) {
    sntp_set_time_sync_notification_cb(on_ntp_sync);
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  }

  vTaskDelete(NULL);
}

void clock_app_init(const char* ssid, const char* pass, const char* tz) {
  // Seed system time from RTC (will be a no-op without a battery installed)
  PCF85063_Read_Time(&datetime);
  if (datetime.year >= 2024) {
    struct tm t = {};
    t.tm_year = datetime.year - 1900;
    t.tm_mon  = datetime.month - 1;
    t.tm_mday = datetime.day;
    t.tm_hour = datetime.hour;
    t.tm_min  = datetime.minute;
    t.tm_sec  = datetime.second;
    time_t epoch = mktime(&t);
    struct timeval tv = { epoch, 0 };
    settimeofday(&tv, nullptr);
  }

  setenv("TZ", tz, 1);
  tzset();

  static const char* creds[2];
  creds[0] = ssid;
  creds[1] = pass;
  xTaskCreatePinnedToCore(wifi_task, "wifi_task", 4096, (void*)creds, 1, NULL, 0);
}

void clock_app_create_ui(void) {
  lv_obj_t* tv = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 30);
  lv_obj_t* t1 = lv_tabview_add_tab(tv, "Digital");
  lv_obj_t* t2 = lv_tabview_add_tab(tv, "Analog");
  build_digital(t1);
  build_analog(t2);

  lv_timer_create(tick_cb, 500, nullptr);
}
