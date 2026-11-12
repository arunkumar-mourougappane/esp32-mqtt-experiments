// Full-screen PIN keypad, shown at boot before "set up again" opens. Blocks until the PIN is right, the user cancels,
// or nothing is touched for a minute. The PIN itself is checked by prov::checkPin (salted hash, 5 tries, then a wait).
#pragma once
#include <Arduino.h>

// `notice` (optional) is a short reason shown under the title, for example why setup is being offered again.
// Returns true only when the right PIN was entered.
bool pinScreenRun(const char *notice);
