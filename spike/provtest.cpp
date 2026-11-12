// On-device self-test for src/prov.cpp: encryption, storage, "secrets are not readable in flash", PIN and lockout.
// Build and flash: pio run -e provtest -t upload, then read the serial port. It wipes the stored settings first.
#include <Arduino.h>
#include <esp_partition.h>
#include "../src/prov.h"

static int failures, checks;
static void check(bool ok, const char *what) {
  checks++; if (!ok) failures++;
  Serial.printf("[provtest] %s  %s\n", ok ? "PASS" : "FAIL", what);
}

// Search a marker in the raw NVS partition (what someone dumping the flash would see).
static bool inFlash(const char *marker) {
  const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, nullptr);
  if (!p) return false;
  static uint8_t buf[4096 + 64];
  size_t m = strlen(marker), have = 0;
  for (size_t off = 0; off < p->size; off += 4096) {
    size_t n = (p->size - off) < 4096 ? (size_t)(p->size - off) : (size_t)4096;
    memmove(buf, buf + have - (have > m ? m - 1 : 0), have > m ? m - 1 : 0);     // keep the tail across blocks
    size_t keep = have > m ? m - 1 : 0;
    if (esp_partition_read(p, off, buf + keep, n) != ESP_OK) return false;
    have = keep + n;
    if (memmem(buf, have, marker, m)) return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("[provtest] start");
  check(prov::begin(), "NVS opens");
  prov::wipe();
  check(!prov::configured(), "nothing stored: not configured");

  // ---- encryption ----
  std::string a, b, back;
  check(prov::encryptField("f", "hello world", a) && prov::encryptField("f", "hello world", b), "encrypt works");
  check(a != b, "same text encrypts differently each time (random nonce)");
  check(prov::decryptField("f", a, back) && back == "hello world", "decrypt returns the original");
  check(!prov::decryptField("other", a, back), "blob does not decrypt under another field name");
  std::string t = a; t[t.size() - 1] ^= 1;
  check(!prov::decryptField("f", t, back), "a flipped bit in the ciphertext is rejected");
  t = a; t[5] ^= 1;
  check(!prov::decryptField("f", t, back), "a flipped bit in the nonce is rejected");
  t = a; t[1 + 12 + 3] ^= 1;
  check(!prov::decryptField("f", t, back), "a flipped bit in the tag is rejected");

  // ---- storage ----
  prov::Config c;
  c.ssid = "SSID-PLAIN-MARKER"; c.wifiPass = "WIFIPASS-MARKER-12345";
  c.host = "192.168.1.232"; c.port = 8884; c.useCert = false;
  c.user = "tab5"; c.pass = "BROKERPASS-MARKER-67890";
  c.ca = "-----BEGIN CERTIFICATE-----\nCA-PUBLIC-MARKER\n-----END CERTIFICATE-----\n";
  check(prov::save(c), "save works");
  check(!prov::configured(), "not configured without a PIN");
  check(prov::setPin("482913"), "PIN set");
  check(prov::configured(), "configured once Wi-Fi, broker, CA, login and PIN are stored");

  prov::Config d;
  check(prov::load(d), "load works");
  check(d.ssid == c.ssid && d.wifiPass == c.wifiPass && d.host == c.host && d.port == 8884 && !d.useCert &&
        d.user == c.user && d.pass == c.pass && d.ca == c.ca, "everything loads back unchanged");

  // an update with empty fields keeps what is stored
  prov::Config u; u.ssid = "SSID-NEW"; u.port = 8883; u.useCert = false;
  prov::save(u);
  prov::load(d);
  check(d.ssid == "SSID-NEW" && d.wifiPass == c.wifiPass && d.pass == c.pass && d.host == c.host, "empty fields keep the stored values");
  c.ssid = "SSID-PLAIN-MARKER"; prov::save(c);

  // ---- certificate path ----
  prov::Config m = c; m.useCert = true; m.crt = "-----BEGIN CERTIFICATE-----\nDEVCRT-PUBLIC-MARKER\n-----END CERTIFICATE-----\n";
  m.key = "-----BEGIN PRIVATE KEY-----\nDEVKEY-SECRET-MARKER-ABCDEF\n-----END PRIVATE KEY-----\n";
  check(prov::save(m), "save with certificate and key");
  prov::load(d);
  check(d.useCert && d.crt == m.crt && d.key == m.key, "certificate and key load back");

  // ---- what is visible in flash ----
  check(inFlash("SSID-PLAIN-MARKER"), "sanity: the flash scan can see a plain value");
  check(inFlash("CA-PUBLIC-MARKER") && inFlash("DEVCRT-PUBLIC-MARKER"), "sanity: public certificates are stored as they are");
  check(!inFlash("WIFIPASS-MARKER-12345"), "Wi-Fi password is NOT readable in flash");
  check(!inFlash("BROKERPASS-MARKER-67890"), "broker password is NOT readable in flash");
  check(!inFlash("DEVKEY-SECRET-MARKER"), "device private key is NOT readable in flash");
  std::string sum = prov::summaryJson();
  check(sum.find("MARKER-12345") == std::string::npos && sum.find("MARKER-67890") == std::string::npos &&
        sum.find("hasKey\":true") != std::string::npos, "the setup page summary has no secret and says a key is stored");
  Serial.printf("[provtest] summary: %s\n", sum.c_str());

  // ---- PIN ----
  uint32_t w = 0; uint32_t t0 = millis();
  check(prov::checkPin("482913", &w) == prov::PIN_OK, "right PIN accepted");
  Serial.printf("[provtest] one PIN check took %u ms\n", (unsigned)(millis() - t0));
  check(prov::checkPin("000000", &w) == prov::PIN_BAD, "wrong PIN rejected (1)");
  check(prov::checkPin("12345", &w) == prov::PIN_BAD, "short PIN rejected (2)");
  check(prov::checkPin("abcdef", &w) == prov::PIN_BAD, "non-digit PIN rejected (3)");
  check(prov::checkPin("111111", &w) == prov::PIN_BAD, "wrong PIN rejected (4)");
  check(prov::checkPin("222222", &w) == prov::PIN_LOCKED && w > 50000, "fifth miss locks for about a minute");
  check(prov::checkPin("482913", &w) == prov::PIN_LOCKED, "the right PIN is refused while locked");
  Serial.println("[provtest] waiting 61 s for the lockout to end ...");
  delay(61000);
  check(prov::checkPin("333333", &w) == prov::PIN_LOCKED, "after the wait, one wrong try locks again straight away");
  Serial.println("[provtest] waiting 61 s again ...");
  delay(61000);
  check(prov::checkPin("482913", &w) == prov::PIN_OK, "after the second wait the right PIN works and unlocks");
  check(prov::checkPin("000000", &w) == prov::PIN_BAD, "failure count was reset by the success");

  // ---- flags ----
  prov::setFresh(true); prov::setReprovPending(true);
  check(prov::fresh() && prov::reprovPending(), "flags set");
  prov::setFresh(false); prov::setReprovPending(false);
  check(!prov::fresh() && !prov::reprovPending(), "flags cleared");

  prov::wipe();
  check(!prov::configured() && !inFlash("WIFIPASS-MARKER-12345"), "wipe removes the settings");

  Serial.printf("[provtest] DONE: %d checks, %d failed\n", checks, failures);
}

void loop() { delay(1000); }
