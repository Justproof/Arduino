#include <WiFi.h>
#include <time.h>
#include <esp_sntp.h>

// === Fill these in ===
static const char* WIFI_SSID = "tooth-decay";
static const char* WIFI_PASS = "betterbrush2026";
static const char* TZ_STRING = "CST6CDT,M3.2.0,M11.1.0";  // US Central
// ======================

static volatile bool ntp_synced = false;
static void on_ntp_sync(struct timeval*) { ntp_synced = true; }

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("=== ESP32-S3 WiFi + NTP smoke test ===");
  Serial.printf("PSRAM free: %d bytes\n", ESP.getFreePsram());

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to '%s'", WIFI_SSID);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("WiFi FAILED (status=%d). Check SSID/password.\n", WiFi.status());
    return;
  }

  Serial.printf("WiFi OK. IP=%s, RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  setenv("TZ", TZ_STRING, 1);
  tzset();
  sntp_set_time_sync_notification_cb(on_ntp_sync);
  configTzTime(TZ_STRING, "pool.ntp.org", "time.nist.gov");
  Serial.println("NTP requested (pool.ntp.org)...");
}

void loop() {
  static uint32_t n = 0;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);

  char buf[64];
  if (t.tm_year < 120) {
    snprintf(buf, sizeof(buf), "waiting for NTP (epoch=%lld)", (long long)now);
  } else {
    strftime(buf, sizeof(buf), "%a %Y-%m-%d %H:%M:%S %Z", &t);
  }

  Serial.printf("tick %lu  wifi=%d  sync=%d  time=%s\n",
                n++, WiFi.status(), ntp_synced ? 1 : 0, buf);
  delay(2000);
}
