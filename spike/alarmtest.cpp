// Can the Tab5 wake itself from an RTC alarm? Each boot prints what it finds, then after 10 s arms the RX8130 alarm for two minutes
// ahead and powers the board off with M5.Power.timerSleep(time). If the alarm powers the board on again, the next boot prints the
// next line of the log; the boot counter lives in NVS, so it survives the power cycle. After WAKES boots it stops and stays on.
// The alarm has one-minute resolution (seconds are ignored). Run it while watching the USB serial port from a computer: if the USB
// link drops and comes back each time, the board really was powered off. If the alarm does not work, press the power button.
// Build and flash: pio run -e alarmtest -t upload
#include <M5Unified.h>
#include <Preferences.h>
#include <esp_system.h>

static const int WAKES = 6;

static void say(const char *fmt, ...) {
  char b[200]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
  Serial.println(b);
  static int line = 0;
  M5.Display.setCursor(10, 10 + 26 * (line++ % 45)); M5.Display.print(b);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0); M5.Display.setBrightness(120); M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(2); M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  Serial.begin(115200);
  delay(1500);

  Preferences pr; pr.begin("alarm", false);
  uint32_t n = pr.getUInt("n", 0) + 1; pr.putUInt("n", n);
  const esp_reset_reason_t rr = esp_reset_reason();
  m5::rtc_date_t d; m5::rtc_time_t t;
  const bool rtcOk = M5.Rtc.isEnabled() && M5.Rtc.getDateTime(&d, &t);
  say("[alarm] boot #%u, reset reason %d (1=power-on 3=software 4=panic)", (unsigned)n, (int)rr);
  say("[alarm] rtc enabled %d, time %04d-%02d-%02d %02d:%02d:%02d", (int)M5.Rtc.isEnabled(), rtcOk ? d.year : 0, rtcOk ? d.month : 0, rtcOk ? d.date : 0, rtcOk ? t.hours : 0, rtcOk ? t.minutes : 0, rtcOk ? t.seconds : 0);
  say("[alarm] battery %d %%, charging state %d, vbus %d mV", (int)M5.Power.getBatteryLevel(), (int)M5.Power.isCharging(), (int)M5.Power.getVBUSVoltage());

  if (rtcOk && d.year < 2024) {                       // clock never set: use the build time, only relative time matters here
    struct tm tmv = {}; char mon[4]; int dd, yy, hh, mm, ss;
    sscanf(__DATE__, "%3s %d %d", mon, &dd, &yy); sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    const char *ms = "JanFebMarAprMayJunJulAugSepOctNovDec"; tmv.tm_mon = (strstr(ms, mon) - ms) / 3;
    tmv.tm_mday = dd; tmv.tm_year = yy - 1900; tmv.tm_hour = hh; tmv.tm_min = mm; tmv.tm_sec = ss;
    M5.Rtc.setDateTime(&tmv); say("[alarm] rtc was not set: set to the build time");
  }
  if (n > WAKES) { say("[alarm] %d wakes done, staying on", WAKES); return; }
  say("[alarm] 10 s, then arm the alarm and power off");
}

void loop() {
  static uint32_t t0 = millis();
  static bool armed = false;
  if (!armed && millis() - t0 > 10000) {
    armed = true;
    Preferences pr; pr.begin("alarm", true);
    if (pr.getUInt("n", 0) > WAKES) return;
    m5::rtc_date_t d; m5::rtc_time_t t;
    M5.Rtc.getDateTime(&d, &t);
    m5::rtc_time_t at = t; at.minutes = (t.minutes + 2) % 60; at.hours = (t.minutes + 2 >= 60) ? (t.hours + 1) % 24 : t.hours; at.seconds = 0;
    say("[alarm] now %02d:%02d:%02d, alarm for %02d:%02d, powering off", t.hours, t.minutes, t.seconds, at.hours, at.minutes);
    delay(300);
    const bool ok = M5.Power.timerSleep(at);          // does not return if it powers off
    say("[alarm] timerSleep RETURNED %d: still powered (USB keeps it on?). watching for the alarm flag", (int)ok);
  }
  if (armed && millis() - t0 > 12000) {
    static uint32_t last = 0;
    if (millis() - last > 15000) {
      last = millis();
      m5::rtc_time_t t; M5.Rtc.getTime(&t);
      say("[alarm] still on, rtc %02d:%02d:%02d, alarm flag %d", t.hours, t.minutes, t.seconds, (int)M5.Rtc.getIRQstatus());
    }
  }
  delay(50);
}
