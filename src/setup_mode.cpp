#include "setup_mode.h"
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include "prov.h"
#include "portal_page.h"

static WebServer web(80);
static DNSServer dns;

static String apSsid, apPass;
static uint32_t logT0;                                   // when setup mode started; every setup log line is relative to it
#define SLOG(fmt, ...) Serial.printf("[setup +%6lu ms] " fmt "\n", (unsigned long)(millis() - logT0), ##__VA_ARGS__)
static volatile int phones;                       // from Wi-Fi events: softAPgetStationNum() is a call into the C6 and can block
static uint32_t startedAt, timeoutMs_;
static bool rerun_;
static uint32_t savedAt;                          // millis() when the settings were saved, 0 = not yet
static std::string savedSsid, savedHost;
static bool savedCert;

// scan results are cached: a scan takes seconds and the page asks repeatedly
static String scanJson = "[]";
static uint32_t scanStarted;
static bool scanning;

// ---------- screen ----------
static uint16_t cBg, cTxt, cDim, cAcc, cOk, cCard, cWhite, cWarn;
static void colours() {
  cBg = M5.Display.color565(14, 17, 22);   cTxt = M5.Display.color565(232, 237, 242); cDim = M5.Display.color565(163, 175, 187);
  cAcc = M5.Display.color565(91, 140, 255); cOk = M5.Display.color565(61, 214, 140);  cCard = M5.Display.color565(23, 28, 35);
  cWhite = TFT_WHITE;                       cWarn = M5.Display.color565(255, 176, 32);
}

static void head(const char *title, const char *sub) {
  M5.Display.fillScreen(cBg);
  M5.Display.setTextDatum(top_center);
  M5.Display.setFont(&fonts::DejaVu40);
  M5.Display.setTextColor(cTxt, cBg);
  M5.Display.drawString(title, 360, 60);
  M5.Display.setFont(&fonts::DejaVu24);
  M5.Display.setTextColor(cDim, cBg);
  if (!sub) return;
  std::string t = sub;
  if (t.size() <= 34) { M5.Display.drawString(sub, 360, 124); return; }            // fits on one line in the 24-point font
  // A long reason (for example why setup opened again) does not fit: two lines in the smaller font, split at a space.
  size_t cut = t.rfind(' ', 44);
  if (cut == std::string::npos || cut < 10) cut = 44;
  M5.Display.setFont(&fonts::DejaVu18);
  M5.Display.drawString(t.substr(0, cut).c_str(), 360, 120);
  if (cut < t.size()) M5.Display.drawString(t.substr(t.find_first_not_of(' ', cut)).c_str(), 360, 152);
}

static void footerLine(const char *text, uint16_t dot) {
  M5.Display.fillRect(0, 1170, 720, 90, cBg);
  M5.Display.fillCircle(60, 1215, 9, dot);
  M5.Display.setTextDatum(middle_left);
  M5.Display.setFont(&fonts::DejaVu24);
  M5.Display.setTextColor(cDim, cBg);
  M5.Display.drawString(text, 90, 1215);
}

static void timeLeftLine(const char *prefix) {
  uint32_t left = timeoutMs_ > millis() - startedAt ? (timeoutMs_ - (millis() - startedAt)) / 1000 : 0;
  char b[80]; snprintf(b, sizeof b, "%s %u:%02u.", prefix, (unsigned)(left / 60), (unsigned)(left % 60));
  footerLine(b, cAcc);
}

static void drawScan(const char *notice) {
  head("Set up this Tab5", notice ? notice : "Scan with your phone's camera");
  M5.Display.fillRoundRect(110, 190, 500, 500, 20, cWhite);
  char qr[96]; snprintf(qr, sizeof qr, "WIFI:T:WPA;S:%s;P:%s;;", apSsid.c_str(), apPass.c_str());
  M5.Display.qrcode(qr, 135, 215, 450, 3);
  M5.Display.fillRoundRect(40, 730, 640, 160, 20, cCard);
  M5.Display.setFont(&fonts::DejaVu24);
  M5.Display.setTextDatum(middle_left);  M5.Display.setTextColor(cDim, cCard);
  M5.Display.drawString("Network", 70, 775);  M5.Display.drawString("Password", 70, 845);
  M5.Display.setTextDatum(middle_right); M5.Display.setTextColor(cTxt, cCard);
  M5.Display.drawString(apSsid, 650, 775);   M5.Display.drawString(apPass, 650, 845);
  M5.Display.setFont(&fonts::DejaVu18);
  M5.Display.setTextDatum(top_center); M5.Display.setTextColor(cDim, cBg);
  M5.Display.drawString("The sign-in page can take up to a minute to open by itself.", 360, 925);
  M5.Display.drawString("To skip the wait, open 192.168.4.1 in your browser.", 360, 957);
  timeLeftLine("Waiting for your phone. Closes in");
}

static void drawRows(const char *title, const char *sub, const char *const rows[4], const char *const vals[4], bool done) {
  head(title, sub);
  for (int i = 0; i < 4; i++) {
    int y = 220 + i * 130;
    M5.Display.fillRoundRect(40, y, 640, 110, 20, cCard);
    M5.Display.fillCircle(95, y + 55, 22, done ? cOk : M5.Display.color565(75, 86, 99));
    if (done) {
      M5.Display.drawLine(84, y + 56, 92, y + 64, cBg); M5.Display.drawLine(85, y + 56, 93, y + 64, cBg);
      M5.Display.drawLine(92, y + 64, 108, y + 46, cBg); M5.Display.drawLine(93, y + 64, 109, y + 46, cBg);
    }
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::DejaVu24); M5.Display.setTextColor(cTxt, cCard);
    M5.Display.drawString(rows[i], 145, y + 35);
    M5.Display.setFont(&fonts::DejaVu18); M5.Display.setTextColor(cDim, cCard);
    M5.Display.drawString(vals[i], 145, y + 76);
  }
}

static void drawJoined() {
  const char *rows[4] = {"Wi-Fi", "Broker", "Certificates", "PIN"};
  const char *vals[4] = {"Waiting for your phone", "Waiting for your phone", "Waiting for your phone", "Waiting for your phone"};
  drawRows("Phone connected", "Finish setup on your phone.", rows, vals, false);
  footerLine("Nothing is saved until you press Save on your phone.", cAcc);
}

static void drawDone() {
  const char *rows[4] = {"Wi-Fi", "Broker", "Certificates", "PIN"};
  static char w[64], b[96], c[40];
  snprintf(w, sizeof w, "%s", savedSsid.c_str());
  snprintf(b, sizeof b, "%s", savedHost.c_str());
  snprintf(c, sizeof c, "%s", savedCert ? "CA, device certificate and key" : "CA certificate");
  const char *vals[4] = {w, b, c, "Saved"};
  drawRows("Setup complete", "The setup network is closed.", rows, vals, true);
}

static void drawClosed() {
  head("Setup closed", "Nothing was saved.");
  M5.Display.setFont(&fonts::DejaVu24);
  M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(cTxt, cBg);
  M5.Display.drawString("Touch the screen to start setup again.", 360, 640);
}

// ---------- scan ----------
static void startScan() {
  if (scanning) return;
  WiFi.scanDelete();
  WiFi.scanNetworks(true);                         // asynchronous
  scanning = true; scanStarted = millis();
  SLOG("scan started (the radio leaves the access point channel while it scans)");
}

static void harvestScan() {
  if (!scanning) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) { if (millis() - scanStarted > 15000) scanning = false; return; }
  scanning = false;
  SLOG("scan finished: %d networks", n);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n && arr.size() < 20; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (JsonObject o : arr) if (s == o["ssid"].as<const char *>()) { dup = true; if (WiFi.RSSI(i) > o["rssi"].as<int>()) o["rssi"] = WiFi.RSSI(i); }
    if (dup) continue;
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = s; o["rssi"] = WiFi.RSSI(i); o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  scanJson = ""; serializeJson(arr, scanJson);
  WiFi.scanDelete();
}

// ---------- certificates ----------
static int rngFn(void *, unsigned char *out, size_t n) { esp_fill_random(out, n); return 0; }

static std::string cnOf(const mbedtls_x509_name *dn) {
  char buf[200] = "";
  mbedtls_x509_dn_gets(buf, sizeof buf, dn);
  std::string s = buf;
  size_t p = s.find("CN=");
  if (p == std::string::npos) return s;
  size_t e = s.find(',', p);
  return s.substr(p + 3, e == std::string::npos ? std::string::npos : e - p - 3);
}

static bool parseCrt(const std::string &pem, mbedtls_x509_crt &crt, std::string &err) {
  if (pem.find("-----BEGIN CERTIFICATE-----") == std::string::npos) { err = "This is not a certificate (no BEGIN CERTIFICATE line)."; return false; }
  int rc = mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem.c_str(), pem.size() + 1);
  if (rc != 0) { err = "The certificate could not be read (error -" + std::to_string(-rc) + ")."; return false; }
  return true;
}

static std::string crtDetail(const mbedtls_x509_crt &c, std::string &name) {
  name = cnOf(&c.subject);
  char b[160];
  snprintf(b, sizeof b, "%s. %s %d-bit. Valid until %04d-%02d-%02d.", name.c_str(), mbedtls_pk_get_name(&c.pk),
           (int)mbedtls_pk_get_bitlen(&c.pk), c.valid_to.year, c.valid_to.mon, c.valid_to.day);
  return b;
}

static bool parseKey(const std::string &pem, mbedtls_pk_context &pk, std::string &err) {
  if (pem.find("PRIVATE KEY-----") == std::string::npos) { err = "This is not a private key (no BEGIN ... PRIVATE KEY line)."; return false; }
  if (pem.find("ENCRYPTED") != std::string::npos) { err = "This key is password protected. Export it without a password."; return false; }
  int rc = mbedtls_pk_parse_key(&pk, (const unsigned char *)pem.c_str(), pem.size() + 1, nullptr, 0, rngFn, nullptr);
  if (rc != 0) { err = "The key could not be read (error -" + std::to_string(-rc) + ")."; return false; }
  return true;
}

// ---------- HTTP ----------
static void sendJson(int code, const String &body) {
  web.sendHeader("Cache-Control", "no-store");
  web.send(code, "application/json", body);
}
static String errJson(const char *msg) {
  JsonDocument d; d["ok"] = false; d["error"] = msg;
  String s; serializeJson(d, s); return s;
}

static void handleRoot() {
  SLOG("GET / (%s)", web.hostHeader().c_str());
  web.send_P(200, "text/html", PORTAL_PAGE);
  SLOG("page sent");
}

static void handleState() {
  SLOG("GET /api/state");
  std::string s = prov::summaryJson();
  uint32_t left = timeoutMs_ > millis() - startedAt ? (timeoutMs_ - (millis() - startedAt)) / 1000 : 0;
  s.insert(s.size() - 1, std::string(",\"rerun\":") + (rerun_ ? "true" : "false") + ",\"left\":" + std::to_string(left));
  sendJson(200, String(s.c_str()));
}

static void handleScan() {
  SLOG("GET /api/scan");
  harvestScan();
  if (!scanning && millis() - scanStarted > 20000) startScan();
  String out = String("{\"scanning\":") + (scanning ? "true" : "false") + ",\"nets\":" + scanJson + "}";
  sendJson(200, out);
}

static void handleCheck() {
  JsonDocument d;
  if (deserializeJson(d, web.arg("plain")) != DeserializationError::Ok) { sendJson(400, errJson("Bad request.")); return; }
  std::string kind = d["kind"] | "", pem = d["pem"] | "", err, name;
  JsonDocument r;
  if (pem.size() > 8192) { sendJson(200, errJson("That file is too large for a certificate or key.")); return; }
  if (kind == "ca" || kind == "crt") {
    mbedtls_x509_crt crt; mbedtls_x509_crt_init(&crt);
    if (!parseCrt(pem, crt, err)) { mbedtls_x509_crt_free(&crt); sendJson(200, errJson(err.c_str())); return; }
    r["ok"] = true; r["detail"] = crtDetail(crt, name); r["name"] = name;
    mbedtls_x509_crt_free(&crt);
  } else if (kind == "key") {
    mbedtls_pk_context pk; mbedtls_pk_init(&pk);
    if (!parseKey(pem, pk, err)) { mbedtls_pk_free(&pk); sendJson(200, errJson(err.c_str())); return; }
    char b[80]; snprintf(b, sizeof b, "%s private key, %d-bit.", mbedtls_pk_get_name(&pk), (int)mbedtls_pk_get_bitlen(&pk));
    r["ok"] = true; r["detail"] = b; r["name"] = "key";
    mbedtls_pk_free(&pk);
  } else { sendJson(400, errJson("Bad request.")); return; }
  String s; serializeJson(r, s); sendJson(200, s);
}

static void handleSave() {
  if (web.arg("plain").length() > 24000) { sendJson(413, errJson("The settings are too large.")); return; }
  JsonDocument d;
  if (deserializeJson(d, web.arg("plain")) != DeserializationError::Ok) { sendJson(400, errJson("Bad request.")); return; }
  drawJoined();
  prov::Config old; prov::load(old);
  const bool had = prov::configured();
  prov::Config c;
  c.ssid = d["ssid"] | "";  c.wifiPass = d["wifiPass"] | "";
  c.host = d["host"] | "";  c.port = d["port"] | 0;
  c.useCert = std::string(d["auth"] | "pw") == "cert";
  c.user = d["user"] | "";  c.pass = d["pass"] | "";
  c.ca = d["ca"] | "";  c.crt = d["crt"] | "";  c.key = d["key"] | "";
  std::string pin = d["pin"] | "";

  // What the result will be once empty fields keep the stored values.
  const std::string ssidAfter = c.ssid.empty() ? old.ssid : c.ssid;
  const bool keepWifi = had && c.wifiPass.empty() && ssidAfter == old.ssid;
  if (ssidAfter.empty() || ssidAfter.size() > 32) { sendJson(200, errJson("The Wi-Fi name must be 1 to 32 characters.")); return; }
  if (!c.wifiPass.empty() && (c.wifiPass.size() < 8 || c.wifiPass.size() > 63)) { sendJson(200, errJson("The Wi-Fi password must be 8 to 63 characters.")); return; }
  if (c.wifiPass.empty() && !keepWifi) { /* an open network: allowed, stored as empty */ }
  if (c.host.empty() || c.host.size() > 253) { sendJson(200, errJson("Enter the broker address.")); return; }
  if (c.port == 0 || c.port > 65535) { sendJson(200, errJson("The port must be between 1 and 65535.")); return; }

  const std::string ca = c.ca.empty() ? old.ca : c.ca;
  if (ca.empty()) { sendJson(200, errJson("The broker CA certificate is missing.")); return; }
  if (c.ca.size() > 8192 || c.crt.size() > 8192 || c.key.size() > 8192) { sendJson(200, errJson("A certificate or key file is too large.")); return; }

  std::string err;
  mbedtls_x509_crt caCrt; mbedtls_x509_crt_init(&caCrt);
  bool good = parseCrt(ca, caCrt, err);
  if (good && !c.useCert) {
    if (c.user.empty() && old.user.empty()) { err = "Enter the broker username."; good = false; }
    else if (c.pass.empty() && (old.pass.empty() || old.useCert)) { err = "Enter the broker password."; good = false; }
  }
  if (good && c.useCert) {
    const std::string crtPem = c.crt.empty() ? old.crt : c.crt, keyPem = c.key.empty() ? old.key : c.key;
    if (crtPem.empty() || keyPem.empty()) { err = "The device certificate and key are both needed."; good = false; }
    else {
      mbedtls_x509_crt crt; mbedtls_x509_crt_init(&crt);
      mbedtls_pk_context pk; mbedtls_pk_init(&pk);
      good = parseCrt(crtPem, crt, err) && parseKey(keyPem, pk, err);
      if (good && mbedtls_pk_check_pair(&crt.pk, &pk, rngFn, nullptr) != 0) { err = "The device key does not belong to the device certificate."; good = false; }
      if (good) {
        uint32_t flags = 0;                             // the clock may not be set yet, so dates are not judged here
        mbedtls_x509_crt_verify(&crt, &caCrt, nullptr, nullptr, &flags, nullptr, nullptr);
        flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
        if (flags & (MBEDTLS_X509_BADCERT_NOT_TRUSTED | MBEDTLS_X509_BADCERT_CN_MISMATCH)) {
          if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) { err = "The device certificate was not issued by that broker CA."; good = false; }
        }
      }
      mbedtls_x509_crt_free(&crt); mbedtls_pk_free(&pk);
    }
  }
  mbedtls_x509_crt_free(&caCrt);
  if (!good) { sendJson(200, errJson(err.c_str())); return; }

  const bool needPin = !had || !prov::hasPin();
  if (pin.empty() && needPin) { sendJson(200, errJson("Set a 6-digit PIN.")); return; }
  if (!pin.empty() && pin.size() != 6) { sendJson(200, errJson("The PIN must be 6 digits.")); return; }

  if (!prov::save(c)) { sendJson(200, errJson("The Tab5 could not store the settings.")); return; }
  if (!pin.empty() && !prov::setPin(pin.c_str())) { sendJson(200, errJson("The PIN must be 6 digits.")); return; }
  prov::setFresh(true);
  prov::setReprovPending(false);

  prov::Config now; prov::load(now);
  savedSsid = now.ssid; savedHost = now.host + ":" + std::to_string(now.port); savedCert = now.useCert;
  sendJson(200, "{\"ok\":true}");
  savedAt = millis();
  drawDone();
}

// Captive-portal probes from the phone (hotspot-detect.html, generate_204, connecttest.txt, ...) and anything else unknown:
// redirect to the setup page. On an iPhone the sign-in sheet then opens by itself after 35 to 60 seconds (measured over
// four joins, by QR code and by hand): the page is served about 3 seconds after joining, so the wait is inside iOS.
// Several variants were tried and made no difference: a DHCP option 114 with a JSON answer, and a tiny probe reply.
static void handleAny() {
  SLOG("probe %s%s -> redirect", web.hostHeader().c_str(), web.uri().c_str());
  web.sendHeader("Location", "http://192.168.4.1/", true);
  web.send(302, "text/plain", "");
}

static void onWifiEvent(arduino_event_id_t e, arduino_event_info_t) {
  if (e == ARDUINO_EVENT_WIFI_AP_STACONNECTED) { phones = phones + 1; SLOG("phone joined the Wi-Fi network"); }
  else if (e == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) { if (phones > 0) phones = phones - 1; SLOG("phone left"); }
  else if (e == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED) SLOG("phone was given an address");
}

// ---------- main loop ----------
static void randomPassword(char out[10]) {
  static const char alpha[] = "abcdefghjkmnpqrstuvwxyz23456789";
  for (int i = 0; i < 9; i++) out[i] = i == 4 ? '-' : alpha[esp_random() % (sizeof alpha - 1)];
  out[9] = 0;
}

void setupModeRun(bool rerun, uint32_t timeoutMs, const char *notice) {
  colours();
  rerun_ = rerun; timeoutMs_ = timeoutMs;
  for (;;) {                                             // loops only for a first run whose time ran out
    phones = 0; savedAt = 0; scanning = false; scanJson = "[]"; scanStarted = 0;
    startedAt = millis(); logT0 = startedAt;
    SLOG("setup mode starts (chip has been up %lu ms)", (unsigned long)startedAt);
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[24]; snprintf(ssid, sizeof ssid, "Tab5-Setup-%02X%02X", mac[4], mac[5]);
    char pw[10]; randomPassword(pw);
    apSsid = ssid; apPass = pw;

    WiFi.onEvent(onWifiEvent);
    WiFi.mode(WIFI_AP_STA);                              // STA side only so the Tab5 can scan for networks
    bool ok = WiFi.softAP(apSsid.c_str(), apPass.c_str(), 6, 0, 2);
    Serial.printf("setup: access point %s %s, address %s\n", apSsid.c_str(), ok ? "up" : "FAILED", WiFi.softAPIP().toString().c_str());
    SLOG("access point %s", ok ? "up" : "FAILED");
    dns.start(53, "*", WiFi.softAPIP());
    web.on("/", handleRoot);
    web.on("/api/state", handleState);
    web.on("/api/scan", handleScan);
    web.on("/api/check", HTTP_POST, handleCheck);
    web.on("/api/save", HTTP_POST, handleSave);
    web.onNotFound(handleAny);
    web.begin();
    startScan();
    uint32_t d0 = millis(); drawScan(notice); SLOG("first screen drawn (%lu ms)", (unsigned long)(millis() - d0));

    int shown = -1;                                       // 0 scan screen, 1 joined screen
    uint32_t lastSecond = 0;
    for (;;) {
      dns.processNextRequest();
      uint32_t h0 = millis();
      web.handleClient();
      if (millis() - h0 > 100) SLOG("web.handleClient() blocked the loop for %lu ms", (unsigned long)(millis() - h0));
      harvestScan();
      uint32_t now = millis();

      if (savedAt) {                                      // saved: show it, then restart onto the new settings
        if (now - savedAt > 4500) { Serial.println("setup: saved, restarting"); delay(200); ESP.restart(); }
        delay(5);
        continue;
      }
      int want = phones > 0 ? 1 : 0;
      if (want != shown) { shown = want; uint32_t d1 = millis(); if (want) { drawJoined(); } else { drawScan(notice); } lastSecond = now; SLOG("screen redrawn (%lu ms)", (unsigned long)(millis() - d1)); }
      if (now - lastSecond >= 1000) {
        lastSecond = now;
        if (!shown) timeLeftLine("Waiting for your phone. Closes in");
        else timeLeftLine("Setup closes in");
      }
      if (now - startedAt >= timeoutMs_) break;
      delay(2);
    }

    Serial.println("setup: time is up");
    web.stop(); dns.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF);
    if (rerun_) { delay(300); ESP.restart(); }            // run with what is stored
    drawClosed();
    for (;;) { M5.update(); if (M5.Touch.getDetail().wasPressed()) break; delay(30); }
  }
}
