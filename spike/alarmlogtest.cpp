// RTC-alarm wake test that logs to the SD card, for a run on battery with nothing attached. Every boot appends one line to
// /alarmrun2.csv on the card: which boot it is, the RTC time (UTC), the reset reason, how the wake compares with the alarm that was
// armed (an alarm wake lands a few seconds after the armed minute; an earlier wake is something else, such as the power button),
// battery voltage, percent and current, the charge state, and the time since the alarm was armed.
//   boot 1          joins the Wi-Fi stored by setup, gets the time from NTP and sets the RTC to it (UTC)
//   boots 1..WAKES  stay awake AWAKE_MS, arm the alarm INTERVAL_MIN minutes ahead (one-minute resolution) and power off
//   boot WAKES+1    joins Wi-Fi again, logs how far the RTC has drifted from NTP, then stays on and prints the file to the serial
//                   port every 30 s (or when you send "d"), so the log can be read after USB is plugged back in
// Between boots the screen is off. Build and flash: pio run -e alarmlog -t upload, then unplug USB after boot 1 has powered off.
#include <M5Unified.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <time.h>
#include "prov.h"

static const int WAKES = 12, INTERVAL_MIN = 5;
static const uint32_t AWAKE_MS = 6000;
static const char *LOG = "/alarmrun2.csv";
static bool sdUp;
static uint32_t n;
static Preferences pr;

static void logln(const char *s) {
  Serial.println(s);
  if (!sdUp) return;
  File f = SD_MMC.open(LOG, FILE_APPEND);
  if (f) { f.println(s); f.close(); }
}
static void say(const char *fmt, ...) {
  char b[220]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
  Serial.println(b);
  static int line = 0;
  M5.Display.setCursor(10, 10 + 26 * (line++ % 45)); M5.Display.print(b);
}
static uint32_t rtcEpoch() {                          // the RTC holds UTC; the C library's local zone is UTC on this firmware
  m5::rtc_date_t d; m5::rtc_time_t t;
  if (!M5.Rtc.getDateTime(&d, &t)) return 0;
  struct tm tmv = {}; tmv.tm_year = d.year - 1900; tmv.tm_mon = d.month - 1; tmv.tm_mday = d.date; tmv.tm_hour = t.hours; tmv.tm_min = t.minutes; tmv.tm_sec = t.seconds;
  return (uint32_t)mktime(&tmv);
}
static void iso(uint32_t e, char *o, size_t n_) { time_t tt = e; struct tm tmv; gmtime_r(&tt, &tmv); strftime(o, n_, "%Y-%m-%dT%H:%M:%S", &tmv); }

static void dump() {
  if (!sdUp) { Serial.println("[alarm] no card"); return; }
  File f = SD_MMC.open(LOG, FILE_READ);
  Serial.println("[alarm] ---- /alarmrun2.csv ----");
  while (f && f.available()) Serial.write(f.read());
  if (f) f.close();
  Serial.println("[alarm] ---- end ----");
}

// Joins the stored Wi-Fi, asks NTP, returns the epoch or 0.
static uint32_t ntpNow(char *why, size_t whyN) {
  prov::Config cfg; prov::begin();
  if (!prov::load(cfg) || cfg.ssid.empty()) { snprintf(why, whyN, "no stored settings"); return 0; }
  WiFi.mode(WIFI_STA); WiFi.begin(cfg.ssid.c_str(), cfg.wifiPass.empty() ? nullptr : cfg.wifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 25000) delay(200);
  if (WiFi.status() != WL_CONNECTED) { snprintf(why, whyN, "wifi join failed"); return 0; }
  // M5Unified sets the system clock from the RTC at boot, so time() already looks valid: clear it, to wait for a real NTP answer.
  struct timeval zero = {0, 0}; settimeofday(&zero, nullptr);
  configTime(0, 0, cfg.ntp.empty() ? "pool.ntp.org" : cfg.ntp.c_str(), "time.google.com");
  t0 = millis();
  while (time(nullptr) < 1700000000 && millis() - t0 < 25000) delay(200);
  if (time(nullptr) < 1700000000) { snprintf(why, whyN, "ntp failed"); return 0; }
  snprintf(why, whyN, "ok");
  return (uint32_t)time(nullptr);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  pr.begin("alarmlog2", false);
  n = pr.getUInt("n", 0) + 1; pr.putUInt("n", n);
  const uint32_t tgt = pr.getUInt("tgt", 0), armedAt = pr.getUInt("arm", 0);
  const bool showScreen = (n == 1 || n > (uint32_t)WAKES);
  M5.Display.setRotation(0); M5.Display.setTextSize(2); M5.Display.setTextColor(TFT_WHITE, TFT_BLACK); M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setBrightness(showScreen ? 120 : 0);
  delay(800);

  SD_MMC.setPowerChannel(4);
  sdUp = SD_MMC.begin("/sdcard", false, false, 20000) || SD_MMC.begin("/sdcard", true, false, 20000);
  if (sdUp && !SD_MMC.exists(LOG)) logln("boot,rtc_utc,reset_reason,wake_cause,source,delta_to_alarm_s,since_armed_s,batt_mV,batt_pct,batt_mA,charge_state,heap,note");

  uint32_t now = rtcEpoch();
  char why[40] = "";
  char note[96] = "";
  if (now < 1704067200) {                                // RTC never set (before 2024-01-01), or unreadable
    snprintf(note, sizeof note, "rtc unset (%lu);", (unsigned long)now);
  }
  uint32_t ntp = 0; long drift = 0; bool synced = false;
  if (n == 1 || n == (uint32_t)WAKES + 1) {
    ntp = ntpNow(why, sizeof why);
    if (ntp) {
      drift = (long)((int64_t)rtcEpoch() - (int64_t)ntp);   // RTC minus NTP, both read just now
      if (n == 1) { time_t tt = ntp; struct tm g; gmtime_r(&tt, &g); M5.Rtc.setDateTime(&g); now = rtcEpoch(); synced = true; }
      snprintf(note + strlen(note), sizeof note - strlen(note), n == 1 ? "rtc set from ntp (was %+lds);" : "rtc minus ntp %+lds;", drift);
    } else snprintf(note + strlen(note), sizeof note - strlen(note), "ntp %s;", why);
    WiFi.disconnect(true, false);
  }

  // How this wake compares with the alarm that was armed: the RX8130 alarm fires at the start of the armed minute and the board
  // takes a few seconds to come up, so a wake within 0..90 s after it is the alarm. Anything earlier is another source.
  const char *src; long delta = 0;
  if (n == 1) src = "first-boot";
  else if (!tgt) src = "unknown(no alarm armed)";
  else { delta = (long)((int64_t)now - (int64_t)tgt); src = delta < -2 ? "EARLY-not-alarm" : delta <= 90 ? "rtc-alarm" : "LATE"; }

  char ts[24]; iso(now, ts, sizeof ts);
  const int mv = M5.Power.getBatteryVoltage(), pct = (int)M5.Power.getBatteryLevel(), ma = (int)M5.Power.getBatteryCurrent();
  char line[300];
  snprintf(line, sizeof line, "%u,%s,%d,%d,%s,%ld,%ld,%d,%d,%d,%d,%u,%s", (unsigned)n, ts, (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause(), src, delta,
           armedAt && now > armedAt ? (long)(now - armedAt) : 0L, mv, pct, ma, (int)M5.Power.isCharging(), (unsigned)ESP.getFreeHeap(), note);
  logln(line);
  say("%s", line);
  if (!sdUp) say("[alarm] NO SD CARD: nothing is being logged");
}

void loop() {
  static uint32_t t0 = millis(), lastDump = 0;
  if (n > (uint32_t)WAKES) {                              // finished: stay on and serve the log on the serial port
    if (Serial.available() && Serial.read() == 'd') dump();
    if (millis() - lastDump > 30000) { lastDump = millis(); dump(); }
    delay(50); return;
  }
  if (millis() - t0 < AWAKE_MS) { delay(50); return; }
  uint32_t now = rtcEpoch();
  const uint32_t tgt = ((now / 60) + INTERVAL_MIN) * 60;   // the start of a minute INTERVAL_MIN or so ahead
  time_t tt = tgt; struct tm g; gmtime_r(&tt, &g);
  m5::rtc_time_t at; at.hours = g.tm_hour; at.minutes = g.tm_min; at.seconds = 0;
  pr.putUInt("tgt", tgt); pr.putUInt("arm", now);
  char ts[24], tg[24]; iso(now, ts, sizeof ts); iso(tgt, tg, sizeof tg);
  char b[160]; snprintf(b, sizeof b, "# boot %u armed at %s for %s, powering off", (unsigned)n, ts, tg);
  logln(b);
  delay(300);
  const bool ok = M5.Power.timerSleep(at);                // does not return if it powers off
  snprintf(b, sizeof b, "# boot %u timerSleep RETURNED %d: the board stayed on (USB?)", (unsigned)n, (int)ok);
  logln(b);
  t0 = millis() + 600000;                                 // do not try again
}
