#include "prov.h"
#include <Preferences.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

namespace prov {

static Preferences nvs;
static bool opened;
static uint32_t lockedUntil;               // millis(); only meaningful while the persistent failure count is >= MAX_FAILS
static constexpr int      MAX_FAILS = 5;
static constexpr uint32_t LOCK_MS = 60000;
static constexpr uint32_t PBKDF2_ITERS = 8000;

// ---------- key ----------
// 32-byte AES key from the chip's unique ID (optional unique ID if it is programmed, plus the factory MAC).
static bool deriveKey(uint8_t key[32]) {
  uint8_t ikm[16 + 8] = {};
  esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, ikm, 128);        // may be all zeros on some chips
  uint64_t mac = ESP.getEfuseMac();
  memcpy(ikm + 16, &mac, 6);
  static const char salt[] = "tab5-prov-v1-salt";
  static const char info[] = "tab5 aes-256-gcm field key";
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  int rc = mbedtls_hkdf(md, (const uint8_t *)salt, sizeof salt - 1, ikm, sizeof ikm, (const uint8_t *)info, sizeof info - 1, key, 32);
  mbedtls_platform_zeroize(ikm, sizeof ikm);
  return rc == 0;
}

// Blob layout: version(1) | nonce(12) | tag(16) | ciphertext. The field name is bound in as additional data,
// so a blob copied from one field to another does not decrypt.
bool encryptField(const char *name, const std::string &plain, std::string &blob) {
  uint8_t key[32]; if (!deriveKey(key)) return false;
  uint8_t nonce[12]; esp_fill_random(nonce, sizeof nonce);
  uint8_t tag[16];
  std::string ct(plain.size(), '\0');
  mbedtls_gcm_context ctx; mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0)
    rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, plain.size(), nonce, sizeof nonce, (const uint8_t *)name, strlen(name),
                                   (const uint8_t *)plain.data(), (uint8_t *)&ct[0], sizeof tag, tag);
  mbedtls_gcm_free(&ctx);
  mbedtls_platform_zeroize(key, sizeof key);
  if (rc != 0) return false;
  blob.assign(1, '\x01');
  blob.append((const char *)nonce, sizeof nonce);
  blob.append((const char *)tag, sizeof tag);
  blob.append(ct);
  return true;
}

bool decryptField(const char *name, const std::string &blob, std::string &plain) {
  if (blob.size() < 1 + 12 + 16 || blob[0] != '\x01') return false;
  uint8_t key[32]; if (!deriveKey(key)) return false;
  const uint8_t *nonce = (const uint8_t *)blob.data() + 1, *tag = nonce + 12, *ct = tag + 16;
  size_t n = blob.size() - 1 - 12 - 16;
  plain.assign(n, '\0');
  mbedtls_gcm_context ctx; mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0)
    rc = mbedtls_gcm_auth_decrypt(&ctx, n, nonce, 12, (const uint8_t *)name, strlen(name), tag, 16, ct, (uint8_t *)&plain[0]);
  mbedtls_gcm_free(&ctx);
  mbedtls_platform_zeroize(key, sizeof key);
  if (rc != 0) { plain.clear(); return false; }
  return true;
}

// ---------- storage helpers ----------
static std::string getBlob(const char *k) {
  size_t n = nvs.getBytesLength(k);
  std::string s(n, '\0');
  if (n) nvs.getBytes(k, &s[0], n);
  return s;
}
static void putBlob(const char *k, const std::string &s) { nvs.putBytes(k, s.data(), s.size()); }
static std::string getText(const char *k) { String v = nvs.getString(k, ""); return std::string(v.c_str()); }

static bool putSecret(const char *k, const std::string &plain) {
  std::string blob;
  if (!encryptField(k, plain, blob)) return false;
  putBlob(k, blob);
  return true;
}
static bool getSecret(const char *k, std::string &plain) {
  std::string blob = getBlob(k);
  if (blob.empty()) { plain.clear(); return true; }            // nothing stored is not an error
  return decryptField(k, blob, plain);
}

bool begin() {
  if (opened) return true;
  opened = nvs.begin("prov", false);
  if (opened && nvs.getUInt("fails", 0) >= (uint32_t)MAX_FAILS) lockedUntil = millis() + LOCK_MS;   // a restart does not skip the wait
  return opened;
}

// ---------- configuration ----------
bool save(const Config &c) {
  if (!opened) return false;
  bool ok = true;
  if (!c.ssid.empty()) nvs.putString("ssid", c.ssid.c_str());
  if (!c.wifiPass.empty()) ok &= putSecret("wpw", c.wifiPass);
  if (!c.host.empty()) nvs.putString("host", c.host.c_str());
  nvs.putUShort("port", c.port);
  nvs.putBool("cert", c.useCert);
  if (!c.user.empty()) nvs.putString("user", c.user.c_str());
  if (!c.pass.empty()) ok &= putSecret("mpw", c.pass);
  if (!c.ca.empty())  putBlob("ca", c.ca);
  if (!c.crt.empty()) putBlob("crt", c.crt);
  if (!c.key.empty()) ok &= putSecret("key", c.key);
  return ok;
}

bool load(Config &c) {
  if (!opened) return false;
  c = Config();
  c.ssid = getText("ssid");  c.host = getText("host");  c.user = getText("user");
  c.port = nvs.getUShort("port", 8883);
  c.useCert = nvs.getBool("cert", false);
  c.ca = getBlob("ca");  c.crt = getBlob("crt");
  bool ok = getSecret("wpw", c.wifiPass);
  ok &= getSecret("mpw", c.pass);
  ok &= getSecret("key", c.key);
  return ok;
}

bool configured() {
  if (!opened) return false;
  if (nvs.getString("ssid", "").length() == 0 || nvs.getString("host", "").length() == 0) return false;
  if (nvs.getBytesLength("ca") == 0 || !hasPin()) return false;
  if (nvs.getBool("cert", false)) return nvs.getBytesLength("crt") > 0 && nvs.getBytesLength("key") > 0;
  return nvs.getString("user", "").length() > 0 && nvs.getBytesLength("mpw") > 0;
}

void wipe() {
  if (!opened) return;
  nvs.clear();
  lockedUntil = 0;
}

// ---------- PIN ----------
static void pbkdf2(const uint8_t *pw, size_t pwl, const uint8_t *salt, size_t sl, uint32_t iters, uint8_t out[32]) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  uint8_t s[64], u[32], t[32];
  memcpy(s, salt, sl); s[sl] = 0; s[sl + 1] = 0; s[sl + 2] = 0; s[sl + 3] = 1;      // salt || INT(1)
  mbedtls_md_hmac(md, pw, pwl, s, sl + 4, u);
  memcpy(t, u, 32);
  for (uint32_t i = 1; i < iters; i++) {
    mbedtls_md_hmac(md, pw, pwl, u, 32, u);
    for (int k = 0; k < 32; k++) t[k] ^= u[k];
  }
  memcpy(out, t, 32);
  mbedtls_platform_zeroize(u, sizeof u); mbedtls_platform_zeroize(t, sizeof t);
}

static bool validPin(const char *pin) {
  if (!pin || strlen(pin) != 6) return false;
  for (int i = 0; i < 6; i++) if (pin[i] < '0' || pin[i] > '9') return false;
  return true;
}

bool hasPin() { return opened && nvs.getBytesLength("pinh") == 32 && nvs.getBytesLength("pins") == 16; }

bool setPin(const char *pin) {
  if (!opened || !validPin(pin)) return false;
  uint8_t salt[16], h[32];
  esp_fill_random(salt, sizeof salt);
  pbkdf2((const uint8_t *)pin, 6, salt, sizeof salt, PBKDF2_ITERS, h);
  nvs.putBytes("pins", salt, sizeof salt);
  nvs.putBytes("pinh", h, sizeof h);
  nvs.putUInt("fails", 0);
  lockedUntil = 0;
  return true;
}

PinResult checkPin(const char *pin, uint32_t *waitMs) {
  if (waitMs) *waitMs = 0;
  if (!hasPin()) return PIN_BAD;
  uint32_t fails = nvs.getUInt("fails", 0);
  if (fails >= (uint32_t)MAX_FAILS) {
    uint32_t now = millis();
    if ((int32_t)(lockedUntil - now) > 0) { if (waitMs) *waitMs = lockedUntil - now; return PIN_LOCKED; }
    fails = MAX_FAILS - 1;                          // the wait is over: one more try, and a miss locks again
    nvs.putUInt("fails", fails);
  }
  uint8_t salt[16], want[32], got[32];
  nvs.getBytes("pins", salt, sizeof salt);
  nvs.getBytes("pinh", want, sizeof want);
  bool match = false;
  if (validPin(pin)) {
    pbkdf2((const uint8_t *)pin, 6, salt, sizeof salt, PBKDF2_ITERS, got);
    uint8_t diff = 0; for (int i = 0; i < 32; i++) diff |= got[i] ^ want[i];     // compare without an early exit
    match = diff == 0;
  }
  if (match) { nvs.putUInt("fails", 0); lockedUntil = 0; return PIN_OK; }
  nvs.putUInt("fails", ++fails);
  if (fails >= (uint32_t)MAX_FAILS) { lockedUntil = millis() + LOCK_MS; if (waitMs) *waitMs = LOCK_MS; return PIN_LOCKED; }
  return PIN_BAD;
}

uint32_t lockWaitMs() {
  if (!opened || nvs.getUInt("fails", 0) < (uint32_t)MAX_FAILS) return 0;
  int32_t left = (int32_t)(lockedUntil - millis());
  return left > 0 ? (uint32_t)left : 0;
}

// ---------- flags ----------
void setFresh(bool on) { if (opened) nvs.putBool("fresh", on); }
bool fresh() { return opened && nvs.getBool("fresh", false); }
void setReprovPending(bool on) { if (opened) nvs.putBool("reprov", on); }
bool reprovPending() { return opened && nvs.getBool("reprov", false); }
void setNotice(const char *text) { if (opened) nvs.putString("notice", text ? text : ""); }
std::string notice() { return opened ? getText("notice") : std::string(); }

// ---------- summary for the setup page (no secrets) ----------
static std::string jsonEscape(const std::string &s) {
  std::string o;
  for (char ch : s) { if (ch == '"' || ch == '\\') { o += '\\'; o += ch; } else if ((uint8_t)ch >= 0x20) o += ch; }
  return o;
}

std::string summaryJson() {
  std::string o = "{";
  o += "\"configured\":" + std::string(configured() ? "true" : "false");
  o += ",\"ssid\":\"" + jsonEscape(getText("ssid")) + "\"";
  o += ",\"hasWifiPass\":" + std::string(nvs.getBytesLength("wpw") ? "true" : "false");
  o += ",\"host\":\"" + jsonEscape(getText("host")) + "\"";
  o += ",\"port\":" + std::to_string(nvs.getUShort("port", 8883));
  o += ",\"useCert\":" + std::string(nvs.getBool("cert", false) ? "true" : "false");
  o += ",\"user\":\"" + jsonEscape(getText("user")) + "\"";
  o += ",\"hasBrokerPass\":" + std::string(nvs.getBytesLength("mpw") ? "true" : "false");
  o += ",\"hasCa\":" + std::string(nvs.getBytesLength("ca") ? "true" : "false");
  o += ",\"hasCrt\":" + std::string(nvs.getBytesLength("crt") ? "true" : "false");
  o += ",\"hasKey\":" + std::string(nvs.getBytesLength("key") ? "true" : "false");
  o += "}";
  return o;
}

}  // namespace prov
