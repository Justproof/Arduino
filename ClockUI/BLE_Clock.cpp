#include "BLE_Clock.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include "RTC_PCF85063.h"
#include "BAT_Driver.h"
#include "BLE_AMS.h"

static const uint16_t UUID_SVC_CTS = 0x1805;
static const uint16_t UUID_CHR_CT  = 0x2A2B;
static const uint16_t UUID_SVC_BAS = 0x180F;
static const uint16_t UUID_CHR_BAT = 0x2A19;

static BLECharacteristic* chrTime = nullptr;
static BLECharacteristic* chrBatt = nullptr;
static bool connected = false;

class SrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    connected = true;
    Serial.println("[BLE] central connected");
  }
  void onDisconnect(BLEServer* s) override {
    connected = false;
    Serial.println("[BLE] central disconnected, re-advertising");
    BLE_AMS_OnDisconnect();
    s->getAdvertising()->start();
  }
};

class SecCb : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t pass_key) override {
    Serial.printf("[BLE] passkey: %06u\n", (unsigned)pass_key);
  }
  bool onConfirmPIN(uint32_t) override { return true; }
  bool onSecurityRequest() override { return true; }
  bool onAuthorizationRequest(uint16_t, uint16_t, bool) override { return true; }
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    Serial.printf("[BLE] auth complete (conn=%u, encrypted=%d, authenticated=%d, bonded=%d)\n",
      desc ? desc->conn_handle : 0,
      desc ? desc->sec_state.encrypted : 0,
      desc ? desc->sec_state.authenticated : 0,
      desc ? desc->sec_state.bonded : 0);
    if (desc && desc->sec_state.encrypted) {
      BLE_AMS_OnBonded(desc->conn_handle);
    }
  }
};

class TimeWriteCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    auto v = c->getValue();
    if (v.length() < 7) return;
    const uint8_t* b = (const uint8_t*)v.c_str();
    datetime_t t = {
      (uint16_t)(b[0] | (b[1] << 8)),
      b[2], b[3], (uint8_t)(v.length() > 7 ? b[7] : 0),
      b[4], b[5], b[6]
    };
    PCF85063_Set_All(t);
  }
};

static void packCurrentTime(uint8_t out[10]) {
  out[0] = datetime.year & 0xFF;
  out[1] = (datetime.year >> 8) & 0xFF;
  out[2] = datetime.month;
  out[3] = datetime.day;
  out[4] = datetime.hour;
  out[5] = datetime.minute;
  out[6] = datetime.second;
  out[7] = datetime.dotw == 0 ? 7 : datetime.dotw;
  out[8] = 0;
  out[9] = 0;
}

static uint8_t voltsToPct(float v) {
  float p = (v - 3.0f) / 1.2f * 100.0f;
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return (uint8_t)p;
}

void BLE_Clock_Init(const char* device_name) {
  Serial.printf("[BLE] init '%s' (heap=%u, psram=%u)\n",
    device_name, ESP.getFreeHeap(), ESP.getFreePsram());
  BLEDevice::init(device_name);
  Serial.printf("[BLE] inited; addr=%s\n", BLEDevice::getAddress().toString().c_str());

  BLEDevice::setSecurityCallbacks(new SecCb());
  BLESecurity* sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  sec->setCapability(ESP_IO_CAP_NONE);   // Just Works pairing
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sec->setForceAuthentication(true);     // server sends Security Request on connect

  BLEServer* srv = BLEDevice::createServer();
  srv->setCallbacks(new SrvCb());

  BLEService* cts = srv->createService(BLEUUID(UUID_SVC_CTS));
  chrTime = cts->createCharacteristic(BLEUUID(UUID_CHR_CT),
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY |
    BLECharacteristic::PROPERTY_WRITE);
  chrTime->addDescriptor(new BLE2902());
  chrTime->setCallbacks(new TimeWriteCb());
  cts->start();

  BLEService* bas = srv->createService(BLEUUID(UUID_SVC_BAS));
  chrBatt = bas->createCharacteristic(BLEUUID(UUID_CHR_BAT),
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY);
  chrBatt->addDescriptor(new BLE2902());
  bas->start();

  BLEAdvertisementData advData;
  advData.setFlags(0x06);  // LE General Discoverable + BR/EDR not supported
  advData.setName(device_name);
  advData.setCompleteServices(BLEUUID(UUID_SVC_CTS));

  BLEAdvertisementData scanData;
  scanData.setCompleteServices(BLEUUID(UUID_SVC_BAS));

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->setMinInterval(0x20);   // 20ms
  adv->setMaxInterval(0x40);   // 40ms
  BLEDevice::startAdvertising();
  Serial.println("[BLE] advertising started");
}

void BLE_Clock_Notify() {
  if (!chrTime || !chrBatt) return;
  uint8_t tbuf[10];
  packCurrentTime(tbuf);
  chrTime->setValue(tbuf, sizeof(tbuf));
  uint8_t pct = voltsToPct(BAT_analogVolts);
  chrBatt->setValue(&pct, 1);
  if (connected) {
    chrTime->notify();
    chrBatt->notify();
  }
}
