#include "Gyro_QMI8658.h"
#include "RTC_PCF85063.h"
#include "LVGL_Driver.h"
#include "BAT_Driver.h"
#include "TCA9554PWR.h"
#include "SD_Card.h"
#include "ClockApp.h"
#include "BLE_Clock.h"

// WIFI_SSID / WIFI_PASS live in secrets.h, which is gitignored.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Copy secrets.example.h to secrets.h and fill in your WiFi credentials."
#endif

static const char* TZ_STRING = "CST6CDT,M3.2.0,M11.1.0";  // US Central

void Driver_Loop(void *parameter) {
  uint32_t last_ble = 0;
  while (1) {
    RTC_Loop();
    BAT_Get_Volts();
    if (millis() - last_ble > 1000) {
      BLE_Clock_Notify();
      last_ble = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== ClockUI (2.8C) boot ===");
  Serial.flush();

  Serial.println("[1] Flash_test");      Serial.flush(); Flash_test();
  Serial.println("[2] BAT_Init");        Serial.flush(); BAT_Init();
  Serial.println("[3] I2C_Init");        Serial.flush(); I2C_Init();
  delay(120);
  Serial.println("[4] TCA9554PWR_Init"); Serial.flush(); TCA9554PWR_Init(0x00);
  Serial.println("[5] Set_EXIO(8,Low)"); Serial.flush(); Set_EXIO(EXIO_PIN8, Low);
  Serial.println("[6] PCF85063_Init");   Serial.flush(); PCF85063_Init();
  Serial.println("[7] LCD_Init");        Serial.flush(); LCD_Init();
  Serial.println("[8] Lvgl_Init");       Serial.flush(); Lvgl_Init();
  Serial.println("[9] create_ui");       Serial.flush(); clock_app_create_ui();
  Serial.println("[10] clock_init");     Serial.flush(); clock_app_init(WIFI_SSID, WIFI_PASS, TZ_STRING);
  Serial.println("[10.5] BLE_Clock");    Serial.flush(); BLE_Clock_Init("ClockUI");
  xTaskCreatePinnedToCore(Driver_Loop, "Driver_Loop", 4096, NULL, 3, NULL, 0);
  Serial.println("[11] setup() done");   Serial.flush();
}

void loop() {
  Lvgl_Loop();
  delay(5);
}
