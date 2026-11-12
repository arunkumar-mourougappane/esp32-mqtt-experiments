// Provisioning storage: Wi-Fi, broker, certificates and PIN, kept in NVS.
//
// Secrets (Wi-Fi password, broker password, device private key) are stored encrypted with AES-256-GCM. The key is
// derived (HKDF-SHA256) from the chip's unique ID, so it is never stored and never leaves the chip: it is not shown on
// the screen, printed on the serial port or sent over the network. No eFuses are burned, so this keeps the values
// away from the ordinary interfaces but not from someone who dumps the flash AND runs this firmware on the same chip.
// Names and public values (Wi-Fi name, broker address, CA and device certificates) are stored as they are.
// The PIN is stored only as a salted PBKDF2-HMAC-SHA256 hash, with a persistent failure count and a lockout.
#pragma once
#include <Arduino.h>
#include <string>

namespace prov {

struct Config {
  std::string ssid, wifiPass;          // Wi-Fi
  std::string host;  uint16_t port = 8883;
  bool        useCert = false;         // false: username + password; true: device certificate (mutual TLS)
  std::string user, pass;              // broker login (password path)
  std::string ca, crt, key;            // PEM text: broker CA, device certificate, device private key
};

bool begin();                          // open storage; call once at boot
bool configured();                     // everything needed to run is stored (Wi-Fi, broker, CA, auth material, PIN)
bool load(Config &c);                  // read and decrypt everything
bool save(const Config &c);            // store; empty fields leave what is stored unchanged; useCert/port always stored
void wipe();                           // erase everything (factory reset of the settings)

bool hasPin();
bool setPin(const char *pin);          // 6 digits
enum PinResult { PIN_OK, PIN_BAD, PIN_LOCKED };
PinResult checkPin(const char *pin, uint32_t *waitMs);   // on PIN_LOCKED, waitMs = time until the next try
uint32_t lockWaitMs();                 // 0 = not locked; else how long until the next try is allowed (does not count as a try)

// Flags that survive a restart.
void setFresh(bool on);  bool fresh();                   // settings were just saved and have not connected yet
void setReprovPending(bool on);  bool reprovPending();   // the user asked to set up again; the PIN is asked at boot
void setNotice(const char *text);  std::string notice();  // a short reason shown when setup opens at boot (for example a failed connection)

// What is stored, without any secret, as JSON for the setup page.
std::string summaryJson();

// Exposed for the self-test.
bool encryptField(const char *name, const std::string &plain, std::string &blob);
bool decryptField(const char *name, const std::string &blob, std::string &plain);

}  // namespace prov
