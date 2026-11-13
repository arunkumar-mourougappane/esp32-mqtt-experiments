// Test build for the M5Stack I2C Joystick 2 Unit (STM32G030, address 0x63) on the Tab5's Port A (SCL GPIO54, SDA GPIO53).
// Scans the external I2C bus, reads the unit's firmware version, and prints the raw values it gives so we know the ranges:
//   0x00  X, 0x02 Y     16-bit little-endian ADC (0..65535)
//   0x10  X, 0x11 Y     8-bit
//   0x20  button        0 = pressed, 1 = released
//   0x50  X, 0x52 Y     signed 16-bit "offset" values (position relative to the centre, calibrated by the unit)
//   0xFE  firmware version, 0xFF I2C address
// Build and flash: pio run -e joytest -t upload, then watch the serial port. Move the stick to every corner and press it.
#include <M5Unified.h>

static const uint8_t JOY = 0x63;
static const uint32_t FREQ = 400000;

static bool rd(uint8_t reg, uint8_t *buf, size_t n) { return M5.Ex_I2C.readRegister(JOY, reg, buf, n, FREQ); }

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(150);
  M5.Display.fillScreen(TFT_BLACK);
  Serial.begin(115200);
  delay(1200);
  M5.Power.setExtOutput(true);                 // Grove Port A 5 V (on by default, made explicit here)
  bool busOk = M5.Ex_I2C.begin();              // M5Unified starts the external bus only when an external RTC/IMU is enabled
  delay(200);                                  // let the unit's 5 V and its STM32 come up
  Serial.printf("[joy] Ex_I2C.begin() -> %d, 5 V output %s\n", (int)busOk, M5.Power.getExtOutput() ? "on" : "off");
  Serial.printf("[joy] external I2C: enabled=%d SDA=%d SCL=%d\n", (int)M5.Ex_I2C.isEnabled(), (int)M5.Ex_I2C.getSDA(), (int)M5.Ex_I2C.getSCL());

  bool found[120] = {};
  M5.Ex_I2C.scanID(found, 100000);
  Serial.print("[joy] devices on Port A:");
  int n = 0;
  for (int a = 0; a < 120; a++) if (found[a]) { Serial.printf(" 0x%02X", a); n++; }
  Serial.printf("%s\n", n ? "" : " none");

  uint8_t v = 0, addr = 0;
  bool ok = rd(0xFE, &v, 1) && rd(0xFF, &addr, 1);
  Serial.printf("[joy] unit at 0x63: %s, firmware %u, reported address 0x%02X\n", ok ? "answers" : "NO ANSWER", v, addr);
}

void loop() {
  static uint32_t last, count;
  static int minX = 65535, maxX = 0, minY = 65535, maxY = 0;
  uint8_t b[4], c, o[4], e8[2];
  bool ok = rd(0x00, b, 4) && rd(0x10, e8, 2) && rd(0x20, &c, 1) && rd(0x50, o, 4);
  if (ok) {
    int x = b[0] | (b[1] << 8), y = b[2] | (b[3] << 8);
    int16_t ox = (int16_t)(o[0] | (o[1] << 8)), oy = (int16_t)(o[2] | (o[3] << 8));
    if (x < minX) minX = x; if (x > maxX) maxX = x; if (y < minY) minY = y; if (y > maxY) maxY = y;
    count++;
    if (millis() - last >= 250) {
      last = millis();
      Serial.printf("[joy] adc16 x=%5d y=%5d | adc8 x=%3u y=%3u | offset x=%6d y=%6d | button=%u | seen x %d..%d y %d..%d | %u reads\n",
                    x, y, e8[0], e8[1], ox, oy, c, minX, maxX, minY, maxY, (unsigned)count);
      M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
      M5.Display.setTextSize(3);
      M5.Display.setCursor(30, 60);
      M5.Display.printf("Joystick test\n\nx %5d  y %5d\nbutton %u\n", x, y, c);
    }
  } else if (millis() - last >= 1000) {
    last = millis();
    Serial.println("[joy] read failed (is the unit plugged into Port A?)");
  }
  delay(5);
}
