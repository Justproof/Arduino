#include "AlbumArt.h"
#include "BLE_AMS.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

static lv_obj_t* img_obj = nullptr;
static lv_img_dsc_t img_dsc;
static uint8_t* current_buf = nullptr;
static SemaphoreHandle_t mtx = nullptr;
static String last_key;

// Filled by fetch task, consumed by AlbumArt_Tick.
static volatile bool pending_swap = false;
static uint8_t* pending_buf = nullptr;
static size_t pending_len = 0;

static void url_encode_into(String& out, const String& s) {
  static const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      out += (char)c;
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
}

static String http_get_text(const String& url) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return String();
  int code = http.GET();
  String body;
  if (code == 200) body = http.getString();
  else Serial.printf("[ART] GET text %d (%s) heap=%u\n",
    code, HTTPClient::errorToString(code).c_str(), ESP.getFreeHeap());
  http.end();
  return body;
}

static size_t http_get_binary(const String& url, uint8_t** out_buf) {
  *out_buf = nullptr;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return 0;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[ART] GET bin %d\n", code);
    http.end();
    return 0;
  }
  int len = http.getSize();
  if (len <= 0 || len > 500000) { http.end(); return 0; }
  uint8_t* buf = (uint8_t*)ps_malloc(len);
  if (!buf) { http.end(); return 0; }

  WiFiClient* stream = http.getStreamPtr();
  size_t total = 0;
  uint32_t deadline = millis() + 10000;
  while (total < (size_t)len && millis() < deadline) {
    int avail = stream->available();
    if (avail > 0) {
      size_t take = (len - total) < (size_t)avail ? (len - total) : (size_t)avail;
      int n = stream->readBytes(buf + total, take);
      if (n <= 0) break;
      total += n;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  http.end();
  if (total != (size_t)len) { free(buf); return 0; }
  *out_buf = buf;
  return total;
}

static void fetch_task(void*) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      String artist = BLE_AMS_GetArtist();
      String title  = BLE_AMS_GetTitle();
      if (artist.length() && title.length()) {
        String key = artist + "|" + title;
        if (key != last_key) {
          last_key = key;
          Serial.printf("[ART] fetching for %s\n", key.c_str());

          String term;
          url_encode_into(term, artist + " " + title);
          String search_url = "https://itunes.apple.com/search?term=" + term + "&entity=song&limit=1";
          String json = http_get_text(search_url);

          int p = json.indexOf("\"artworkUrl100\":\"");
          if (p >= 0) {
            p += 17;
            int e = json.indexOf("\"", p);
            if (e > p) {
              String art_url = json.substring(p, e);
              art_url.replace("\\/", "/");
              art_url.replace("100x100bb", "300x300bb");
              Serial.printf("[ART] url %s\n", art_url.c_str());

              uint8_t* buf = nullptr;
              size_t len = http_get_binary(art_url, &buf);
              if (buf && len > 0) {
                Serial.printf("[ART] downloaded %u bytes\n", (unsigned)len);
                xSemaphoreTake(mtx, portMAX_DELAY);
                if (pending_buf) free(pending_buf);
                pending_buf = buf;
                pending_len = len;
                pending_swap = true;
                xSemaphoreGive(mtx);
              }
            }
          } else {
            Serial.println("[ART] no artworkUrl100 in iTunes response");
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void AlbumArt_Init(lv_obj_t* parent) {
  mtx = xSemaphoreCreateMutex();
  img_obj = lv_img_create(parent);
  lv_obj_set_size(img_obj, 300, 300);
  lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(img_obj, LV_OBJ_FLAG_FLOATING);    // out of flex flow
  lv_obj_set_style_opa(img_obj, LV_OPA_50, 0);        // soften so labels stay readable
  lv_obj_move_background(img_obj);                    // render behind siblings
  xTaskCreatePinnedToCore(fetch_task, "art_fetch", 16384, NULL, 1, NULL, 0);
}

void AlbumArt_Tick() {
  if (!pending_swap) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  uint8_t* old_buf = current_buf;
  current_buf = pending_buf;
  size_t len = pending_len;
  pending_buf = nullptr;
  pending_len = 0;
  pending_swap = false;
  xSemaphoreGive(mtx);

  memset(&img_dsc, 0, sizeof(img_dsc));
  img_dsc.header.cf = LV_IMG_CF_RAW;
  img_dsc.data = current_buf;
  img_dsc.data_size = len;
  lv_img_set_src(img_obj, &img_dsc);
  lv_img_cache_invalidate_src(NULL);
  if (old_buf) free(old_buf);
}
