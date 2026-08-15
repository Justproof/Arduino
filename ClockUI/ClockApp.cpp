#include "ClockApp.h"
#include "RTC_PCF85063.h"
#include "BLE_AMS.h"
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

static lv_obj_t* lbl_np_title  = nullptr;
static lv_obj_t* lbl_np_artist = nullptr;
static lv_obj_t* lbl_np_album  = nullptr;
static lv_obj_t* lbl_np_play   = nullptr;  // label inside play/pause btn

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

  if (lbl_np_title) {
    const String& title = BLE_AMS_GetTitle();
    const String& artist = BLE_AMS_GetArtist();
    const String& album = BLE_AMS_GetAlbum();
    lv_label_set_text(lbl_np_title,  title.length()  ? title.c_str()  : "—");
    lv_label_set_text(lbl_np_artist, artist.length() ? artist.c_str() : "");
    lv_label_set_text(lbl_np_album,  album.length()  ? album.c_str()  : "");
    if (lbl_np_play) {
      lv_label_set_text(lbl_np_play,
        BLE_AMS_IsPlaying() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
  }
}

static void np_btn_cb(lv_event_t* e) {
  AmsRemoteCmd cmd = (AmsRemoteCmd)(intptr_t)lv_event_get_user_data(e);
  BLE_AMS_SendCommand(cmd);
}

static void build_nowplaying(lv_obj_t* page) {
  lv_obj_set_style_pad_all(page, 16, 0);
  lv_obj_set_style_pad_top(page, 80, 0);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lbl_np_title = lv_label_create(page);
  lv_obj_set_style_text_font(lbl_np_title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(lbl_np_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(lbl_np_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_size(lbl_np_title, 380, 28);
  lv_label_set_text(lbl_np_title, "—");

  lbl_np_artist = lv_label_create(page);
  lv_obj_set_style_text_color(lbl_np_artist, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_set_style_pad_top(lbl_np_artist, 6, 0);
  lv_obj_set_style_text_align(lbl_np_artist, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(lbl_np_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_size(lbl_np_artist, 380, 26);
  lv_label_set_text(lbl_np_artist, "");

  lbl_np_album = lv_label_create(page);
  lv_obj_set_style_text_color(lbl_np_album, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_set_style_pad_top(lbl_np_album, 2, 0);
  lv_obj_set_style_text_align(lbl_np_album, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(lbl_np_album, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_size(lbl_np_album, 380, 22);
  lv_label_set_text(lbl_np_album, "");

  lv_obj_t* row = lv_obj_create(page);
  lv_obj_set_size(row, 340, 80);
  lv_obj_set_style_pad_top(row, 20, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  struct { const char* sym; AmsRemoteCmd cmd; lv_obj_t** lbl_out; } btns[] = {
    { LV_SYMBOL_PREV, AMS_CMD_PREVIOUS_TRACK,    nullptr },
    { LV_SYMBOL_PLAY, AMS_CMD_TOGGLE_PLAY_PAUSE, &lbl_np_play },
    { LV_SYMBOL_NEXT, AMS_CMD_NEXT_TRACK,        nullptr },
  };
  for (auto& b : btns) {
    lv_obj_t* btn = lv_btn_create(row);
    lv_obj_set_size(btn, 80, 60);
    lv_obj_add_event_cb(btn, np_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)b.cmd);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, b.sym);
    lv_obj_center(lbl);
    if (b.lbl_out) *b.lbl_out = lbl;
  }

  lv_obj_t* vol_row = lv_obj_create(page);
  lv_obj_set_size(vol_row, 260, 70);
  lv_obj_set_style_pad_top(vol_row, 12, 0);
  lv_obj_set_style_border_width(vol_row, 0, 0);
  lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  struct { const char* sym; AmsRemoteCmd cmd; } vol_btns[] = {
    { LV_SYMBOL_VOLUME_MID, AMS_CMD_VOLUME_DOWN },
    { LV_SYMBOL_VOLUME_MAX, AMS_CMD_VOLUME_UP   },
  };
  for (auto& b : vol_btns) {
    lv_obj_t* btn = lv_btn_create(vol_row);
    lv_obj_set_size(btn, 90, 50);
    lv_obj_add_event_cb(btn, np_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)b.cmd);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, b.sym);
    lv_obj_center(lbl);
  }
}

static void build_digital(lv_obj_t* page) {
  lv_obj_set_style_pad_all(page, 20, 0);
  lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lbl_time = lv_label_create(page);
  lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_48, 0);
  lv_label_set_text(lbl_time, "--:--:--");

  lbl_date = lv_label_create(page);
  lv_obj_set_style_text_font(lbl_date, &lv_font_montserrat_20, 0);
  lv_obj_set_style_pad_top(lbl_date, 16, 0);
  lv_label_set_text(lbl_date, "");

  lbl_status = lv_label_create(page);
  lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lbl_status, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_set_style_pad_top(lbl_status, 24, 0);
  lv_label_set_text(lbl_status, "booting...");
}

static void build_analog(lv_obj_t* page) {
  meter = lv_meter_create(page);
  lv_obj_center(meter);
  lv_obj_set_size(meter, 440, 440);

  lv_meter_scale_t* scale = lv_meter_add_scale(meter);
  lv_meter_set_scale_ticks(meter, scale, 61, 2, 14, lv_palette_main(LV_PALETTE_GREY));
  lv_meter_set_scale_range(meter, scale, 0, 60, 360, 270);

  lv_meter_scale_t* hours = lv_meter_add_scale(meter);
  lv_meter_set_scale_ticks(meter, hours, 12, 0, 0, lv_color_black());
  lv_meter_set_scale_major_ticks(meter, hours, 1, 4, 28, lv_color_black(), 14);
  lv_meter_set_scale_range(meter, hours, 1, 12, 330, 300);

  hand_h = lv_meter_add_needle_line(meter, scale, 10, lv_color_black(), -60);
  hand_m = lv_meter_add_needle_line(meter, scale, 7,  lv_palette_main(LV_PALETTE_GREY), -30);
  hand_s = lv_meter_add_needle_line(meter, scale, 3,  lv_palette_main(LV_PALETTE_RED), 0);
}

/**
 * Owns the network for the life of the sketch: keeps the station associated
 * and (re)starts SNTP on every fresh link-up. Never returns — an earlier
 * version gave up after 30s and deleted itself, which left SNTP unstarted
 * whenever association ran long, so the UI sat on "syncing NTP..." forever
 * while WiFi.status() reported a healthy link.
 */
static void wifi_task(void* arg) {
  const char** creds = (const char**)arg;
  const char* ssid = creds[0];
  const char* pass = creds[1];
  const char* tz   = creds[2];

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);  // this board sits at the edge of range

  /* A marginal link produces a disconnect every second or so; collapse the
     repeats and report only when the reason changes or every 30s. */
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    static uint8_t  last_reason = 0;
    static uint32_t last_print  = 0;
    static uint32_t repeats     = 0;
    uint8_t reason = info.wifi_sta_disconnected.reason;
    repeats++;
    if (reason != last_reason || millis() - last_print > 30000) {
      Serial.printf("[NET] disconnect reason=%u (x%u)\n", reason, repeats);
      last_reason = reason;
      last_print  = millis();
      repeats     = 0;
    }
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  int n = WiFi.scanNetworks();
  Serial.printf("[NET] scan found %d networks\n", n);
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssid) {
      Serial.printf("[NET] target '%s' rssi=%d ch=%d enc=%d\n",
                    WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                    WiFi.channel(i), (int)WiFi.encryptionType(i));
    }
  }

  WiFi.begin(ssid, pass);
  Serial.printf("[NET] associating with '%s'\n", ssid);

  bool sntp_running = false;
  uint32_t last_attempt = millis();
  uint32_t last_report  = 0;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!sntp_running) {
        Serial.printf("[NET] up: ip=%s gw=%s dns=%s rssi=%d\n",
                      WiFi.localIP().toString().c_str(),
                      WiFi.gatewayIP().toString().c_str(),
                      WiFi.dnsIP().toString().c_str(),
                      WiFi.RSSI());
        sntp_set_time_sync_notification_cb(on_ntp_sync);
        sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
        /* configTzTime, not configTime: configTime(0,0,...) rewrites TZ to
           UTC and would undo the zone clock_app_init installed. */
        configTzTime(tz, "pool.ntp.org", "time.nist.gov", "time.google.com");
        Serial.println("[NTP] sntp started");
        sntp_running = true;
        last_report = millis();
      } else if (!ntp_synced && millis() - last_report > 10000) {
        Serial.printf("[NTP] still unsynced after %us (sntp reachability=0x%02x)\n",
                      (unsigned)((millis() - last_attempt) / 1000),
                      sntp_getreachability(0));
        last_report = millis();
      }
    } else {
      if (sntp_running) {
        Serial.printf("[NET] link lost (status=%d)\n", WiFi.status());
        sntp_running = false;
      }
      if (millis() - last_attempt > 20000) {
        Serial.printf("[NET] retrying association (status=%d)\n", WiFi.status());
        WiFi.disconnect();
        WiFi.begin(ssid, pass);
        last_attempt = millis();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void clock_app_init(const char* ssid, const char* pass, const char* tz) {
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

  static const char* creds[3];
  creds[0] = ssid;
  creds[1] = pass;
  creds[2] = tz;
  xTaskCreatePinnedToCore(wifi_task, "wifi_task", 4096, (void*)creds, 1, NULL, 0);
}

void clock_app_create_ui(void) {
  lv_obj_t* tv = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 0);
  lv_obj_add_flag(lv_tabview_get_tab_btns(tv), LV_OBJ_FLAG_HIDDEN);
  lv_obj_t* t1 = lv_tabview_add_tab(tv, "Digital");
  lv_obj_t* t2 = lv_tabview_add_tab(tv, "Analog");
  lv_obj_t* t3 = lv_tabview_add_tab(tv, "Music");
  build_digital(t1);
  build_analog(t2);
  build_nowplaying(t3);

  lv_timer_create(tick_cb, 500, nullptr);
}
