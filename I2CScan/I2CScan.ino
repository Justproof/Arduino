#include <Wire.h>

void scan(TwoWire& bus, int sda, int scl, const char* name) {
  bus.end();
  if (!bus.begin(sda, scl, 100000)) {
    Serial.printf("%s begin() FAILED on sda=%d scl=%d\n", name, sda, scl);
    return;
  }
  Serial.printf("%s scanning sda=%d scl=%d ...\n", name, sda, scl);
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    bus.beginTransmission(addr);
    if (bus.endTransmission() == 0) {
      Serial.printf("  0x%02X\n", addr);
      found++;
    }
  }
  Serial.printf("%s done: %d device(s)\n", name, found);
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  // Try the demo's documented pins first
  scan(Wire,  11, 10, "Wire@(11,10)");
  scan(Wire1, 1,   3, "Wire1@(1,3)");

  // Common alternates
  scan(Wire,  8,  9,  "Wire@(8,9)");
  scan(Wire,  18, 19, "Wire@(18,19)");
  scan(Wire,  21, 22, "Wire@(21,22)");
}

void loop() {
  delay(10000);
  Serial.println("---rescanning---");
  scan(Wire,  11, 10, "Wire@(11,10)");
  scan(Wire1, 1,   3, "Wire1@(1,3)");
}
