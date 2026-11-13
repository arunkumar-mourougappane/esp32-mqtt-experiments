#include "pin_screen.h"
#include <M5Unified.h>
#include "prov.h"

namespace {
const int KX = 40, KY = 520, KW = 200, KH = 120, KG = 20;     // keypad: 3 columns x 4 rows
uint16_t cBg, cTxt, cDim, cKey, cBad, cOk;

// 0-9 digits, 10 = Cancel, 11 = Delete; -1 = none
int keyAt(int x, int y) {
  if (x < KX || y < KY) return -1;
  int c = (x - KX) / (KW + KG), r = (y - KY) / (KH + KG);
  if (c > 2 || r > 3) return -1;
  if ((x - KX) % (KW + KG) >= KW || (y - KY) % (KH + KG) >= KH) return -1;      // in a gap
  int i = r * 3 + c;                       // 0..11 in reading order: 1 2 3 / 4 5 6 / 7 8 9 / Cancel 0 Delete
  if (i < 9) return i + 1;
  return i == 9 ? 10 : i == 10 ? 0 : 11;
}

void drawKey(int i, bool pressed) {
  int c, r;
  if (i >= 1 && i <= 9) { c = (i - 1) % 3; r = (i - 1) / 3; }
  else if (i == 10) { c = 0; r = 3; } else if (i == 0) { c = 1; r = 3; } else { c = 2; r = 3; }
  int x = KX + c * (KW + KG), y = KY + r * (KH + KG);
  M5.Display.fillRoundRect(x, y, KW, KH, 22, pressed ? M5.Display.color565(60, 72, 88) : cKey);
  M5.Display.setTextDatum(middle_center);
  if (i <= 9) { M5.Display.setFont(&fonts::DejaVu40); M5.Display.setTextColor(cTxt, pressed ? M5.Display.color565(60, 72, 88) : cKey); char b[2] = {(char)('0' + i), 0}; M5.Display.drawString(b, x + KW / 2, y + KH / 2); }
  else { M5.Display.setFont(&fonts::DejaVu24); M5.Display.setTextColor(cDim, pressed ? M5.Display.color565(60, 72, 88) : cKey); M5.Display.drawString(i == 10 ? "Cancel" : "Delete", x + KW / 2, y + KH / 2); }
}

void drawDots(int n) {
  M5.Display.fillRect(0, 360, 720, 60, cBg);
  for (int i = 0; i < 6; i++) {
    int x = 360 + (i - 3) * 70 + 35;
    if (i < n) M5.Display.fillCircle(x, 390, 15, cTxt);
    else { M5.Display.drawCircle(x, 390, 15, cDim); M5.Display.drawCircle(x, 390, 14, cDim); }
  }
}

void status(const char *msg, uint16_t col) {
  M5.Display.fillRect(0, 440, 720, 50, cBg);
  M5.Display.setTextDatum(top_center);
  M5.Display.setFont(&fonts::DejaVu24);
  M5.Display.setTextColor(col, cBg);
  M5.Display.drawString(msg, 360, 448);
}

void drawAll(const char *notice) {
  M5.Display.fillScreen(cBg);
  M5.Display.setTextDatum(top_center);
  M5.Display.setFont(&fonts::DejaVu40); M5.Display.setTextColor(cTxt, cBg);
  M5.Display.drawString("Enter PIN", 360, 90);
  M5.Display.setFont(&fonts::DejaVu24); M5.Display.setTextColor(cDim, cBg);
  M5.Display.drawString("To set up again", 360, 150);
  if (notice && *notice) {                                 // split a long reason over two lines at a space
    std::string s = notice, a = s, b;
    if (s.size() > 44) { size_t p = s.rfind(' ', 44); if (p == std::string::npos) p = 44; a = s.substr(0, p); b = s.substr(p + 1); }
    M5.Display.setFont(&fonts::DejaVu18); M5.Display.setTextColor(cDim, cBg);
    M5.Display.drawString(a.c_str(), 360, 205);
    if (!b.empty()) M5.Display.drawString(b.c_str(), 360, 235);
  }
  drawDots(0);
  for (int i = 0; i <= 11; i++) drawKey(i, false);
}
}  // namespace

bool pinScreenRun(const char *notice) {
  cBg = M5.Display.color565(14, 17, 22);  cTxt = M5.Display.color565(232, 237, 242); cDim = M5.Display.color565(163, 175, 187);
  cKey = M5.Display.color565(27, 34, 43); cBad = M5.Display.color565(255, 120, 110); cOk = M5.Display.color565(61, 214, 140);
  char pin[7] = {0}; int n = 0;
  drawAll(notice);
  uint32_t lastTouch = millis(), lockedUntil = 0, lastTick = 0;
  int held = -1;
  if (uint32_t w = prov::lockWaitMs()) { lockedUntil = millis() + w; lastTick = 0; }     // still locked from earlier tries
  for (;;) {
    M5.update();
    auto t = M5.Touch.getDetail();
    uint32_t now = millis();
    if (t.isPressed()) lastTouch = now;

    if (lockedUntil) {                                      // locked: show the wait, ignore the keypad except Cancel
      if (now - lastTick >= 500) {
        lastTick = now;
        if ((int32_t)(lockedUntil - now) <= 0) { lockedUntil = 0; lastTouch = now; status("You can try again.", cDim); }   // the idle timer restarts here
        else { char b[48]; uint32_t s = (lockedUntil - now + 999) / 1000; snprintf(b, sizeof b, "Too many tries. Wait %u s.", (unsigned)s); status(b, cBad); }
      }
    }
    int k = t.isPressed() ? keyAt(t.x, t.y) : -1;
    if (k != held) {                                        // press feedback
      if (held >= 0) drawKey(held, false);
      if (k >= 0) drawKey(k, true);
      held = k;
    }
    if (t.wasPressed() && k >= 0) {
      if (k == 10) return false;                            // Cancel
      if (lockedUntil && (int32_t)(lockedUntil - now) > 0) {}   // keypad disabled while locked
      else if (k == 11) { if (n) { pin[--n] = 0; drawDots(n); } status("", cDim); }
      else if (n < 6) {
        pin[n++] = (char)('0' + k); pin[n] = 0; drawDots(n);
        if (n == 6) {
          status("Checking...", cDim);
          uint32_t wait = 0;
          prov::PinResult r = prov::checkPin(pin, &wait);
          if (r == prov::PIN_OK) { status("PIN accepted.", cOk); delay(400); return true; }
          memset(pin, 0, sizeof pin); n = 0; drawDots(0);
          if (r == prov::PIN_LOCKED) { lockedUntil = millis() + wait; lastTick = 0; }
          else status("Wrong PIN.", cBad);
        }
      }
    }
    if (!lockedUntil && now - lastTouch > 60000) return false;   // nobody is there (not counted while the keypad is locked)
    delay(15);
  }
}
