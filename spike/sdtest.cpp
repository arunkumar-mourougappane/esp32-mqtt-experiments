// Test build for the Tab5's microSD slot. Two questions, before any feature is built on it:
//   1. does the card mount, and how fast and how steadily does it write (a slow or stalling write would hold up a task)?
//   2. does it coexist with Wi-Fi? The ESP32-C6 talks to the P4 over esp-hosted on SDIO slot 1 of the same SDMMC controller
//      that the card (slot 0) uses, so each is checked before and after the other starts, and while both are busy.
// Build and flash: pio run -e sdtest -t upload, then watch the serial port. Needs a FAT-formatted card in the slot.
#include <M5Unified.h>
#include <WiFi.h>
#include <SD_MMC.h>

static void say(const char *fmt, ...) {
  char b[200]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
  Serial.println(b);
  static int line = 0;
  M5.Display.setCursor(10, 10 + 22 * (line++ % 55)); M5.Display.print(b);
}

static int scan(const char *when) {
  uint32_t t = millis();
  int n = WiFi.scanNetworks();
  say("[sd] wifi scan %s: %d networks in %u ms", when, n, (unsigned)(millis() - t));
  return n;
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0); M5.Display.setBrightness(150); M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(2); M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  Serial.begin(115200);
  delay(1500);
  say("[sd] start");

  WiFi.mode(WIFI_STA);
  const int before = scan("before the card");

  SD_MMC.setPowerChannel(4);                      // the P4 powers the slot's IO through on-chip LDO channel 4
  bool ok = SD_MMC.begin("/sdcard", false, false, 20000);           // 4-bit, 20 MHz
  if (!ok) { say("[sd] 4-bit mount failed, trying 1-bit"); ok = SD_MMC.begin("/sdcard", true, false, 20000); }
  if (!ok) { say("[sd] MOUNT FAILED (is a FAT card in the slot?)"); return; }
  const uint8_t type = SD_MMC.cardType();
  say("[sd] mounted: type %s, %llu MB total, %llu MB used", type == CARD_SDHC ? "SDHC" : type == CARD_SD ? "SD" : type == CARD_MMC ? "MMC" : "?",
      SD_MMC.cardSize() / (1024 * 1024), SD_MMC.usedBytes() / (1024 * 1024));

  const int after = scan("after mounting the card");
  say("[sd] wifi %s the card was mounted", (before > 0 && after > 0) ? "still works after" : "BROKEN by");

  // throughput and the worst single write
  static uint8_t buf[4096];
  for (int i = 0; i < 4096; i++) buf[i] = (uint8_t)(i * 7);
  File f = SD_MMC.open("/sdtest.bin", FILE_WRITE);
  if (!f) { say("[sd] cannot create the test file"); return; }
  uint32_t worst = 0, t0 = millis();
  for (int i = 0; i < 256; i++) {                 // 1 MB
    uint32_t t = micros(); f.write(buf, sizeof buf); uint32_t d = micros() - t; if (d > worst) worst = d;
  }
  uint32_t tf = micros(); f.flush(); uint32_t flushUs = micros() - tf;
  f.close();
  uint32_t ms = millis() - t0;
  say("[sd] wrote 1 MB in %u ms (%u kB/s), slowest 4 kB write %u us, flush %u us", (unsigned)ms, (unsigned)(1024UL * 1000 / (ms ? ms : 1)), (unsigned)worst, (unsigned)flushUs);

  f = SD_MMC.open("/sdtest.bin", FILE_READ);
  uint32_t bad = 0, rd = 0; t0 = millis();
  while (f.available()) { int n = f.read(buf, sizeof buf); for (int i = 0; i < n; i++) if (buf[i] != (uint8_t)(i * 7)) bad++; rd += n; }
  f.close();
  say("[sd] read back %u kB in %u ms, %u wrong bytes", (unsigned)(rd / 1024), (unsigned)(millis() - t0), (unsigned)bad);
  SD_MMC.remove("/sdtest.bin");

  // a few log-sized appends while Wi-Fi is scanning: what a logger would do
  const int mid = scan("while appending");
  say("[sd] wifi %s while the card was written", mid > 0 ? "still works" : "BROKEN");
  say("[sd] done; now logging a line a second and scanning every 10 s");
}

void loop() {
  static uint32_t last, scans, lines, worst, fails;
  if (!SD_MMC.cardType()) { delay(1000); return; }
  if (millis() - last < 1000) { delay(5); return; }
  last = millis();
  uint32_t t = micros();
  File f = SD_MMC.open("/log.txt", FILE_APPEND);
  if (f) { f.printf("%u,%u,%u\n", (unsigned)millis(), (unsigned)ESP.getFreeHeap(), (unsigned)lines); f.close(); lines++; } else fails++;
  uint32_t d = micros() - t; if (d > worst) worst = d;
  if (lines % 10 == 0) {
    const int n = WiFi.scanNetworks(); scans++;
    say("[sd] soak: %u lines, %u open failures, slowest append %u us, %u scans, last scan %d networks", (unsigned)lines, (unsigned)fails, (unsigned)worst, (unsigned)scans, n);
  }
}
