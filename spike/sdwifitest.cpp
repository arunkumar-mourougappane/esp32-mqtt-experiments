// Overlap test for the Tab5's microSD slot and Wi-Fi: does writing to the card disturb the link, and does the link disturb the writes?
// The C6's Wi-Fi runs over esp-hosted on SDIO slot 1 of the same SDMMC controller as the card (slot 0), so the two can collide.
// It joins the Wi-Fi stored by first-run setup (so set the Tab5 up first), then in three phases sends UDP packets to the broker's
// computer at a steady high rate (about 200 packets of 1 kB a second, far more than the real MQTT stream) while a second task
// writes to the card:
//   1. network only         - the baseline for how long a send takes
//   2. network + card       - the same, with the card written continuously from the other core
//   3. network only again
// It reports the slowest send per phase, send failures, card errors, read-back mismatches, and whether Wi-Fi stayed up.
// Build and flash: pio run -e sdwifitest -t upload. A FAT-formatted card must be in the slot.
#include <M5Unified.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <SD_MMC.h>
#include "prov.h"

static prov::Config cfg;
static volatile bool writing = false, writerStop = false;
static volatile uint32_t wBytes = 0, wErr = 0, wWorst = 0, wOps = 0;

static void say(const char *fmt, ...) {
  char b[200]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
  Serial.println(b);
  static int line = 0;
  M5.Display.setCursor(10, 10 + 22 * (line++ % 55)); M5.Display.print(b);
}

// Writes 4 kB blocks in a rolling file, and a small appended log line with a close (what a logger does) every 32 blocks.
static void writerTask(void *) {
  static uint8_t buf[4096];
  File f; int blocks = 0;
  for (;;) {
    if (writerStop) break;
    if (!writing) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    if (!f) { f = SD_MMC.open("/wifitest.bin", FILE_WRITE); if (!f) { wErr = wErr + 1; vTaskDelay(pdMS_TO_TICKS(100)); continue; } blocks = 0; }
    for (int i = 0; i < 4096; i++) buf[i] = (uint8_t)(i + blocks);
    uint32_t t = micros();
    size_t n = f.write(buf, sizeof buf);
    uint32_t d = micros() - t; if (d > wWorst) wWorst = d;
    if (n != sizeof buf) wErr = wErr + 1; else { wBytes = wBytes + n; wOps = wOps + 1; }
    if (++blocks >= 64) { f.close(); blocks = 0; File l = SD_MMC.open("/wifilog.txt", FILE_APPEND); if (l) { l.printf("%u\n", (unsigned)millis()); l.close(); } else wErr = wErr + 1; }
  }
  if (f) f.close();
  vTaskDelete(nullptr);
}

struct Res { uint32_t sent, fail, worstUs, over50ms; int lastErr; };

static Res flood(int sock, const sockaddr_in &to, uint32_t ms) {
  static uint8_t pkt[1024];
  memset(pkt, 0xA5, sizeof pkt);
  Res r = {0, 0, 0, 0, 0};
  uint32_t end = millis() + ms;
  while ((int32_t)(millis() - end) < 0) {
    uint32_t t = micros();
    int n = sendto(sock, pkt, sizeof pkt, 0, (const sockaddr *)&to, sizeof to);
    uint32_t d = micros() - t;
    if (n == (int)sizeof pkt) r.sent++; else { r.fail++; r.lastErr = errno; }
    if (d > r.worstUs) r.worstUs = d;
    if (d > 50000) r.over50ms++;
    vTaskDelay(pdMS_TO_TICKS(5));                  // about 200 packets a second
  }
  return r;
}

void setup() {
  auto cfgM5 = M5.config();
  M5.begin(cfgM5);
  M5.Display.setRotation(0); M5.Display.setBrightness(150); M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(2); M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  Serial.begin(115200);
  delay(1500);
  say("[ov] start");
  prov::begin();
  if (!prov::load(cfg) || cfg.ssid.empty() || cfg.host.empty()) { say("[ov] no stored settings: run first-run setup first"); return; }

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid.c_str(), cfg.wifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) delay(200);
  if (WiFi.status() != WL_CONNECTED) { say("[ov] could not join Wi-Fi in 30 s"); return; }
  say("[ov] joined in %u ms, rssi %d", (unsigned)(millis() - t0), (int)WiFi.RSSI());

  SD_MMC.setPowerChannel(4);
  if (!SD_MMC.begin("/sdcard", false, false, 20000)) { say("[ov] card mount failed"); return; }
  say("[ov] card mounted, %llu MB", SD_MMC.cardSize() / (1024 * 1024));
  SD_MMC.remove("/wifilog.txt");

  IPAddress ip = WiFi.gatewayIP();                 // the router always answers ARP, so the flood measures the radio path, not one host
  say("[ov] flooding the gateway %s", ip.toString().c_str());
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in to = {}; to.sin_family = AF_INET; to.sin_port = htons(9); to.sin_addr.s_addr = (uint32_t)ip;
  { uint8_t probe[16] = {0}; int n = sendto(sock, probe, sizeof probe, 0, (const sockaddr *)&to, sizeof to); say("[ov] probe send: %d (errno %d)", n, n < 0 ? errno : 0); }
  Res a = flood(sock, to, 10000);
  say("[ov] 1 network only: %u sent, %u failed, slowest send %u us, %u over 50 ms, last errno %d", (unsigned)a.sent, (unsigned)a.fail, (unsigned)a.worstUs, (unsigned)a.over50ms, a.lastErr);

  xTaskCreatePinnedToCore(writerTask, "sdw", 8192, nullptr, 2, nullptr, 0);
  wBytes = wErr = wWorst = wOps = 0; writing = true;
  Res b = flood(sock, to, 30000);
  writing = false; vTaskDelay(pdMS_TO_TICKS(100));
  say("[ov] 2 network + card: %u sent, %u failed, slowest send %u us, %u over 50 ms, last errno %d", (unsigned)b.sent, (unsigned)b.fail, (unsigned)b.worstUs, (unsigned)b.over50ms, b.lastErr);
  say("[ov]   card: %u kB in %u writes, %u errors, slowest write %u us", (unsigned)(wBytes / 1024), (unsigned)wOps, (unsigned)wErr, (unsigned)wWorst);
  const bool wifiStill = WiFi.status() == WL_CONNECTED;

  Res c = flood(sock, to, 10000);
  say("[ov] 3 network only: %u sent, %u failed, slowest send %u us, %u over 50 ms, last errno %d", (unsigned)c.sent, (unsigned)c.fail, (unsigned)c.worstUs, (unsigned)c.over50ms, c.lastErr);

  writerStop = true; vTaskDelay(pdMS_TO_TICKS(200));
  // read back what was written: every block must match its pattern
  File f = SD_MMC.open("/wifitest.bin", FILE_READ);
  uint32_t bad = 0, blocks = 0; static uint8_t buf[4096];
  while (f && f.available()) {
    int n = f.read(buf, sizeof buf); if (n != (int)sizeof buf) break;
    for (int i = 0; i < 4096; i++) if (buf[i] != (uint8_t)(i + (int)(blocks % 64))) { bad++; }
    blocks++;
  }
  if (f) f.close();
  say("[ov] read back %u blocks, %u wrong bytes", (unsigned)blocks, (unsigned)bad);
  SD_MMC.remove("/wifitest.bin"); SD_MMC.remove("/wifilog.txt");
  say("[ov] Wi-Fi stayed up through the card phase: %s, now %s", wifiStill ? "yes" : "NO", WiFi.status() == WL_CONNECTED ? "up" : "DOWN");
  say("[ov] done");
}

void loop() { delay(1000); }
