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
static lv_obj_t* lbl_np_artist = nullptr;  // "Artist • Album"
static lv_obj_t* lbl_np_header = nullptr;  // scope status line
static lv_obj_t* lbl_np_time   = nullptr;  // "1:26 / 4:14"
static lv_obj_t* lbl_np_play   = nullptr;  // label inside play/pause btn

/* ---- Signal: oscilloscope music tab ----------------------------------------
   The board never sees the audio (the iPhone plays it), so the trace is
   synthesized, but every parameter is regulated by real AMS data: play/pause
   ramps the amplitude, playback rate drives the scroll speed, the iPhone
   volume scales the trace, and each track's title+artist hash seeds the
   waveform's shape so every song draws its own signature. */

#define WAVE_W    480
#define WAVE_H    150
#define WAVE_STEP 4
#define WAVE_PTS  (WAVE_W / WAVE_STEP + 1)

static const lv_color_t COL_TRACE  = lv_color_hex(0x4AF08A);
static const lv_color_t COL_DIM    = lv_color_hex(0x559A72);
static const lv_color_t COL_BRIGHT = lv_color_hex(0xD9F5E4);
static const lv_color_t COL_GRID   = lv_color_hex(0x0E3320);
static const lv_color_t COL_BG     = lv_color_hex(0x04120A);

static lv_obj_t*  wave_line = nullptr;
static lv_point_t wave_pts[WAVE_PTS];
static float wv_phase = 0, wv_ramp = 0, wv_surge = 0, wv_env_t = 0;
static struct { float w1, w2, w3, k1, k2, k3, om1, om2, speed; } wv;
static uint32_t wv_rng = 1;
static String   np_last_key;

static float wv_rand() {  // xorshift32 -> [0,1)
  wv_rng ^= wv_rng << 13; wv_rng ^= wv_rng >> 17; wv_rng ^= wv_rng << 5;
  return (wv_rng & 0xFFFFFF) / 16777216.0f;
}

static void wave_reseed(const String& title, const String& artist) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < title.length();  i++) { h ^= (uint8_t)title[i];  h *= 16777619u; }
  for (size_t i = 0; i < artist.length(); i++) { h ^= (uint8_t)artist[i]; h *= 16777619u; }
  wv_rng = h ? h : 1;

  float c1 = 1.5f + 3.5f * wv_rand();          // fundamental: cycles across screen
  wv.k1 = c1 * 2.0f * PI / WAVE_W;
  wv.k2 = wv.k1 * (1.8f + 1.4f * wv_rand());
  wv.k3 = wv.k1 * (3.2f + 2.2f * wv_rand());
  float w1 = 0.55f + 0.25f * wv_rand();
  float w2 = 0.20f + 0.20f * wv_rand();
  float w3 = 0.08f + 0.12f * wv_rand();
  float sum = w1 + w2 + w3;                    // normalize so peaks stay in band
  wv.w1 = w1 / sum; wv.w2 = w2 / sum; wv.w3 = w3 / sum;
  wv.om1 = 0.4f + 1.2f * wv_rand();            // amplitude swell LFOs
  wv.om2 = 0.9f + 2.1f * wv_rand();
  wv.speed = 2.2f + 2.2f * wv_rand();          // scroll rad/s at playback rate 1.0
}

static void wave_cb(lv_timer_t*) {
  const float dt = 0.033f;
  bool playing = BLE_AMS_IsConnected() && BLE_AMS_IsPlaying();
  float rate = BLE_AMS_GetPlaybackRate();
  if (playing && rate <= 0.0f) rate = 1.0f;    // some players omit the rate

  wv_ramp += ((playing ? 1.0f : 0.0f) - wv_ramp) * fminf(1.0f, dt * 5.0f);
  wv_surge *= expf(-dt * 2.2f);
  wv_env_t += dt;
  wv_phase += dt * wv.speed * rate;

  float vol  = BLE_AMS_GetVolume();
  float volf = (vol >= 0.0f) ? (0.35f + 0.65f * vol) : 0.75f;
  float env  = 0.62f + 0.38f * sinf(wv_env_t * wv.om1) * sinf(wv_env_t * wv.om2 + 1.3f);
  float amp  = 62.0f * wv_ramp * volf * env * (1.0f + wv_surge);

  for (int i = 0; i < WAVE_PTS; i++) {
    float x = i * WAVE_STEP;
    float y = wv.w1 * sinf(wv.k1 * x + wv_phase)
            + wv.w2 * sinf(wv.k2 * x + wv_phase * 1.6f + 1.1f)
            + wv.w3 * sinf(wv.k3 * x + wv_phase * 0.7f + 2.4f);
    wave_pts[i].x = (lv_coord_t)x;
    wave_pts[i].y = (lv_coord_t)(WAVE_H / 2 - amp * y);
  }
  lv_line_set_points(wave_line, wave_pts, WAVE_PTS);
}

static volatile bool ntp_synced = false;
static volatile bool radio_free = false;  // WiFi is down; BLE may start

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
    const String& title  = BLE_AMS_GetTitle();
    const String& artist = BLE_AMS_GetArtist();
    const String& album  = BLE_AMS_GetAlbum();
    bool connected = BLE_AMS_IsConnected();
    bool playing   = connected && BLE_AMS_IsPlaying();

    /* New track: reseed the waveform so this song gets its own trace,
       and kick a brief amplitude surge. */
    String key = title + "\x1F" + artist;
    if (key != np_last_key) {
      np_last_key = key;
      wave_reseed(title, artist);
      wv_surge = 0.9f;
    }

    lv_label_set_text(lbl_np_title, title.length() ? title.c_str() : "—");
    String sub = artist;
    if (album.length()) {
      if (sub.length()) sub += "  " LV_SYMBOL_BULLET "  ";
      sub += album;
    }
    lv_label_set_text(lbl_np_artist, sub.c_str());

    lv_label_set_text(lbl_np_header,
      !connected ? "NO SIGNAL"
                 : (playing ? "CH1 " LV_SYMBOL_BULLET " BLE " LV_SYMBOL_BULLET " PLAYING"
                            : "CH1 " LV_SYMBOL_BULLET " BLE " LV_SYMBOL_BULLET " PAUSED"));

    float dur = BLE_AMS_GetDurationSec();
    if (connected && dur > 0.0f) {
      int el = (int)BLE_AMS_GetElapsedSec();
      if (el < 0) el = 0;
      if (el > (int)dur) el = (int)dur;
      char tbuf[24];
      snprintf(tbuf, sizeof(tbuf), "%d:%02d / %d:%02d",
               el / 60, el % 60, (int)dur / 60, (int)dur % 60);
      lv_label_set_text(lbl_np_time, tbuf);
    } else {
      lv_label_set_text(lbl_np_time, "--:-- / --:--");
    }

    if (lbl_np_play) {
      lv_label_set_text(lbl_np_play, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
  }
}

static void np_btn_cb(lv_event_t* e) {
  AmsRemoteCmd cmd = (AmsRemoteCmd)(intptr_t)lv_event_get_user_data(e);
  BLE_AMS_SendCommand(cmd);
}

static lv_obj_t* mk_scope_btn(lv_obj_t* page, const char* sym, AmsRemoteCmd cmd,
                              lv_coord_t x_ofs, lv_coord_t y_ofs,
                              lv_coord_t w, lv_coord_t h, lv_obj_t** lbl_out) {
  lv_obj_t* btn = lv_btn_create(page);
  lv_obj_set_size(btn, w, h);
  lv_obj_align(btn, LV_ALIGN_CENTER, x_ofs, y_ofs);
  lv_obj_set_style_bg_color(btn, COL_GRID, 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_40, 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_radius(btn, h / 2, 0);
  lv_obj_add_event_cb(btn, np_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)cmd);
  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, sym);
  lv_obj_set_style_text_color(lbl, COL_TRACE, 0);
  lv_obj_center(lbl);
  if (lbl_out) *lbl_out = lbl;
  return btn;
}

static void build_nowplaying(lv_obj_t* page) {
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(page, COL_BG, 0);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);

  /* Graticule: one measurement-grid line every 48 px, drawn once. */
  for (int p = 48; p < 480; p += 48) {
    lv_obj_t* v = lv_obj_create(page);
    lv_obj_remove_style_all(v);
    lv_obj_set_pos(v, p, 0);
    lv_obj_set_size(v, 1, 480);
    lv_obj_set_style_bg_color(v, COL_GRID, 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);

    lv_obj_t* hz = lv_obj_create(page);
    lv_obj_remove_style_all(hz);
    lv_obj_set_pos(hz, 0, p);
    lv_obj_set_size(hz, 480, 1);
    lv_obj_set_style_bg_color(hz, COL_GRID, 0);
    lv_obj_set_style_bg_opa(hz, LV_OPA_COVER, 0);
  }

  lbl_np_header = lv_label_create(page);
  lv_obj_set_style_text_color(lbl_np_header, COL_DIM, 0);
  lv_obj_set_style_text_letter_space(lbl_np_header, 2, 0);
  lv_obj_align(lbl_np_header, LV_ALIGN_CENTER, 0, -160);
  lv_label_set_text(lbl_np_header, "NO SIGNAL");

  /* The trace. Points are rewritten ~30x/s by wave_cb; only this 480x150
     band ever invalidates, so the rest of the face stays cheap. */
  wave_line = lv_line_create(page);
  lv_obj_set_pos(wave_line, 0, 116);
  lv_obj_set_size(wave_line, WAVE_W, WAVE_H);
  lv_obj_set_style_line_width(wave_line, 3, 0);
  lv_obj_set_style_line_color(wave_line, COL_TRACE, 0);
  lv_obj_set_style_line_rounded(wave_line, true, 0);
  for (int i = 0; i < WAVE_PTS; i++) {
    wave_pts[i].x = (lv_coord_t)(i * WAVE_STEP);
    wave_pts[i].y = WAVE_H / 2;
  }
  lv_line_set_points(wave_line, wave_pts, WAVE_PTS);

  lbl_np_title = lv_label_create(page);
  lv_obj_set_style_text_font(lbl_np_title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lbl_np_title, COL_BRIGHT, 0);
  lv_obj_set_style_text_align(lbl_np_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(lbl_np_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_size(lbl_np_title, 380, 28);
  lv_obj_align(lbl_np_title, LV_ALIGN_CENTER, 0, 36);
  lv_label_set_text(lbl_np_title, "—");

  lbl_np_artist = lv_label_create(page);
  lv_obj_set_style_text_color(lbl_np_artist, COL_DIM, 0);
  lv_obj_set_style_text_align(lbl_np_artist, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(lbl_np_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_size(lbl_np_artist, 380, 24);
  lv_obj_align(lbl_np_artist, LV_ALIGN_CENTER, 0, 66);
  lv_label_set_text(lbl_np_artist, "");

  lbl_np_time = lv_label_create(page);
  lv_obj_set_style_text_color(lbl_np_time, COL_TRACE, 0);
  lv_obj_set_style_text_letter_space(lbl_np_time, 1, 0);
  lv_obj_align(lbl_np_time, LV_ALIGN_CENTER, 0, 94);
  lv_label_set_text(lbl_np_time, "--:-- / --:--");

  mk_scope_btn(page, LV_SYMBOL_VOLUME_MID, AMS_CMD_VOLUME_DOWN, -160, 108, 56, 44, nullptr);
  mk_scope_btn(page, LV_SYMBOL_VOLUME_MAX, AMS_CMD_VOLUME_UP,    160, 108, 56, 44, nullptr);
  mk_scope_btn(page, LV_SYMBOL_PREV, AMS_CMD_PREVIOUS_TRACK,     -88, 146, 64, 52, nullptr);
  mk_scope_btn(page, LV_SYMBOL_PLAY, AMS_CMD_TOGGLE_PLAY_PAUSE,    0, 152, 72, 56, &lbl_np_play);
  mk_scope_btn(page, LV_SYMBOL_NEXT, AMS_CMD_NEXT_TRACK,          88, 146, 64, 52, nullptr);

  wave_reseed("", "");
  lv_timer_create(wave_cb, 33, nullptr);
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
 * Brings the station up, sets the clock from SNTP, then shuts WiFi down and
 * hands the radio and its heap to BLE.
 *
 * WiFi and Bluedroid cannot both live in what is left of internal RAM on this
 * build: whichever starts second fails. WiFi second means createCharacteristic
 * asserts on a NULL semaphore at ~73KB free; BLE second means esp_wifi_start
 * never comes up and the station sits at WL_STOPPED. They also share the one
 * 2.4GHz radio, so overlapping them starved the auth exchange into endless
 * reason=2 deauths. Running them in sequence sidesteps both problems.
 *
 * The clock keeps time afterwards from the PCF85063, which on_ntp_sync writes
 * on every successful sync.
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

  WiFi.begin(ssid, pass);
  Serial.printf("[NET] associating with '%s'\n", ssid);

  /* How long to chase a sync before giving the radio to BLE anyway, so the
     music tab still works on a network that never appears. */
  const uint32_t GIVE_UP_MS = 90000;

  bool sntp_running = false;
  uint32_t t_start      = millis();
  uint32_t last_attempt = millis();
  uint32_t last_report  = 0;

  for (;;) {
    if (ntp_synced || millis() - t_start > GIVE_UP_MS) {
      Serial.printf("[NET] done (synced=%d); powering WiFi down, heap=%u\n",
                    (int)ntp_synced, (unsigned)ESP.getFreeHeap());
      WiFi.disconnect(true, false);
      WiFi.mode(WIFI_OFF);
      vTaskDelay(pdMS_TO_TICKS(300));
      Serial.printf("[NET] wifi off, heap=%u -> BLE may start\n",
                    (unsigned)ESP.getFreeHeap());
      radio_free = true;
      vTaskDelete(NULL);
    }

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

bool clock_app_time_synced(void) {
  return ntp_synced;
}

bool clock_app_radio_free(void) {
  return radio_free;
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
