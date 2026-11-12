// Runs only the setup mode, to try the access point, QR code and setup page on a real phone.
#include <M5Unified.h>
#include "../src/prov.h"
#include "../src/setup_mode.h"

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(180);
  Serial.begin(115200);
  delay(500);
  prov::begin();
  Serial.printf("setuptest: configured=%d\n", (int)prov::configured());
  setupModeRun(prov::configured(), 600000UL, nullptr);       // stored settings present: behaves like "set up again"
}

void loop() {}
