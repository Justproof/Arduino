#include "Display_ST7789.h"
#include "RTC_PCF85063.h"
#include "Gyro_QMI8658.h"
#include "LVGL_Driver.h"
#include "PWR_Key.h"
#include "BAT_Driver.h"
#include "ClockApp.h"

// === Fill these in ===
static const char* WIFI_SSID = "tooth-decay";
static const char* WIFI_PASS = "betterbrush2026";
static const char* TZ_STRING = "CST6CDT,M3.2.0,M11.1.0";  // US Central
// ======================

void DriverTask(void *parameter) {
  while (1) {
    PWR_Loop();
    BAT_Get_Volts();
    PCF85063_Loop();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("ClockUI booting...");

  PWR_Init();
  // Force peripheral power on (USB-only mode — no battery, no button held)
  pinMode(7, OUTPUT);
  digitalWrite(7, HIGH);
  delay(50);
  BAT_Init();
  I2C_Init();
  PCF85063_Init();
  Backlight_Init();
  LCD_Init();
  Lvgl_Init();

  clock_app_create_ui();
  clock_app_init(WIFI_SSID, WIFI_PASS, TZ_STRING);

  xTaskCreatePinnedToCore(DriverTask, "DriverTask", 4096, NULL, 3, NULL, 0);
  Serial.println("setup() done");
}

void loop() {
  Lvgl_Loop();
  vTaskDelay(pdMS_TO_TICKS(5));
}
