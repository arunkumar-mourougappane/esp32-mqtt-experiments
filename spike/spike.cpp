// Throwaway spike for the first-run setup and quick-settings work. NOT the product firmware.
// Questions it answers:
//   1. Does touch work (M5.update polling) and can it share the I2C bus with the IMU and battery sensor?
//   2. Can the C6 (esp-hosted) host a Wi-Fi access point, hand out addresses, and serve a web page + POST?
//   3. Does touch wake the screen after the backlight is turned off?
//   4. How much internal RAM is left while the access point and web server are running?
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_heap_caps.h>

static const char *AP_SSID = "Tab5-Setup-TEST";
static const char *AP_PASS = "testpass123";

static WebServer web(80);
static DNSServer dns;

// I2C stress counters (IMU and battery sensor read with NO mutex while touch is polled).
static volatile uint32_t imuReads, imuBad, inaReads, inaBad, touchPolls, updMaxUs;
static volatile uint32_t httpHits, posts;
static volatile bool screenOff;
static volatile int phones;        // counted from events: softAPgetStationNum() is an RPC to the C6 and can block
static uint32_t lastTouchMs;

static const char PAGE[] =
  "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Tab5 setup test</title><body style='font-family:system-ui;margin:24px'>"
  "<h2>Tab5 setup test page</h2><p>If you can read this, the access point and web server work.</p>"
  "<form method=post action=/save>"
  "<p><label>Name<br><input name=ssid value=Ptolemaios style='font-size:18px;width:100%'></label></p>"
  "<p><label>Pasted text (try a certificate)<br><textarea name=pem rows=8 style='width:100%'></textarea></label></p>"
  "<button style='font-size:18px;padding:12px 20px'>Send</button></form></body>";

static void handleRoot() { httpHits++; web.send(200, "text/html", PAGE); }

static void handleSave() {
  posts++;
  Serial.printf("[spike] POST /save: ssid=%u bytes, pem=%u bytes\n", (unsigned)web.arg("ssid").length(), (unsigned)web.arg("pem").length());
  web.send(200, "text/html", "<!doctype html><meta name=viewport content='width=device-width'><body style='font-family:system-ui;margin:24px'><h2>Received</h2><p>The Tab5 got your data.</p>");
}

static void handleAny() {            // captive-portal probes (generate_204, hotspot-detect, ...) land here
  httpHits++;
  Serial.printf("[spike] redirect %s%s\n", web.hostHeader().c_str(), web.uri().c_str());
  web.sendHeader("Location", "http://192.168.4.1/", true);
  web.send(302, "text/plain", "");
}

static void imuTask(void *) {
  for (;;) {
    imuReads++;
    if (!M5.Imu.update()) imuBad++;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void inaTask(void *) {
  for (;;) {
    int v = M5.Power.getBatteryVoltage();
    inaReads++;
    if (v < 5000 || v > 9000) inaBad++;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

static void onWifiEvent(arduino_event_id_t e, arduino_event_info_t) {
  switch (e) {
    case ARDUINO_EVENT_WIFI_AP_START:        Serial.println("[spike] event: AP started"); break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: phones++; Serial.println("[spike] event: phone joined the AP"); break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: if (phones > 0) phones--; Serial.println("[spike] event: phone left the AP"); break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED: Serial.println("[spike] event: phone was given an address"); break;
    default: break;
  }
}

static void memLine() {
  Serial.printf("[spike] mem: internal free=%u largest=%u | imu %u/%u bad, ina %u/%u bad, touch polls %u, upd max %u us, http %u, posts %u\n",
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
    (unsigned)imuBad, (unsigned)imuReads, (unsigned)inaBad, (unsigned)inaReads, (unsigned)touchPolls, (unsigned)updMaxUs,
    (unsigned)httpHits, (unsigned)posts);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(180);
  Serial.begin(115200);
  delay(300);
  Serial.println("[spike] boot");
  Serial.printf("[spike] touch available: %d, width %d height %d\n", (int)M5.Touch.isEnabled(), (int)M5.Display.width(), (int)M5.Display.height());
  memLine();

  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, 6, 0, 2);
  Serial.printf("[spike] softAP(%s) -> %s, ip %s\n", AP_SSID, ok ? "OK" : "FAILED", WiFi.softAPIP().toString().c_str());
  dns.start(53, "*", WiFi.softAPIP());
  web.on("/", handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.onNotFound(handleAny);
  web.begin();
  memLine();

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(30, 30);
  M5.Display.print("Setup spike");
  char qr[96];
  snprintf(qr, sizeof qr, "WIFI:T:WPA;S:%s;P:%s;;", AP_SSID, AP_PASS);
  M5.Display.fillRect(130, 130, 460, 460, TFT_WHITE);
  M5.Display.qrcode(qr, 150, 150, 420, 3);

  xTaskCreatePinnedToCore(imuTask, "imu", 4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(inaTask, "ina", 4096, nullptr, 2, nullptr, 1);
  lastTouchMs = millis();
}

void loop() {
  static uint32_t lastPoll, lastMem, lastDraw;
  uint32_t now = millis();
  dns.processNextRequest();
  web.handleClient();

  if (now - lastPoll >= 20) {                       // 50 Hz touch polling
    lastPoll = now;
    uint32_t t0 = micros();
    M5.update();
    uint32_t dt = micros() - t0;
    if (dt > updMaxUs) updMaxUs = dt;
    touchPolls++;
    auto t = M5.Touch.getDetail();
    if (t.wasPressed()) {
      Serial.printf("[spike] touch down x=%d y=%d (screen %s)\n", t.x, t.y, screenOff ? "OFF" : "on");
      if (screenOff) { screenOff = false; M5.Display.setBrightness(180); Serial.println("[spike] woke the screen by touch"); }
      lastTouchMs = now;
    }
    if (t.wasFlicked()) Serial.printf("[spike] flick dx=%d dy=%d\n", t.deltaX(), t.deltaY());
    if (t.isPressed()) lastTouchMs = now;
  }

  if (!screenOff && now - lastTouchMs > 25000) {    // idle: turn the backlight off, keep polling touch
    screenOff = true;
    M5.Display.setBrightness(0);
    Serial.println("[spike] screen off after 25 s idle; touch to wake");
  }

  if (now - lastDraw >= 500 && !screenOff) {
    lastDraw = now;
    M5.Display.setTextSize(2);
    M5.Display.setCursor(30, 640);
    M5.Display.printf("phones: %d   http: %u   posts: %u   \n", (int)phones,(unsigned)httpHits, (unsigned)posts);
    M5.Display.printf("Join %s (pass %s)\n", AP_SSID, AP_PASS);
    M5.Display.printf("imu bad %u/%u  ina bad %u/%u   \n", (unsigned)imuBad, (unsigned)imuReads, (unsigned)inaBad, (unsigned)inaReads);
  }

  if (now - lastMem >= 5000) { lastMem = now; memLine(); }
  vTaskDelay(1);
}
