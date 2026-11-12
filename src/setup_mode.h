// First-run setup mode: the Tab5 opens its own Wi-Fi network, shows a QR code to join it, and serves a setup page
// (Wi-Fi, broker, certificates, PIN). Everything is stored by prov:: and the Tab5 restarts.
#pragma once
#include <Arduino.h>

// Runs until the settings are saved (then restarts the chip, so it does not return), or for `timeoutMs`.
//   rerun = false: first run. Nothing is stored, so there is nothing to go back to: when the time is up the screen says
//                  so and a touch starts it again. Never returns.
//   rerun = true:  setting up again. When the time is up the chip restarts and runs with what is already stored.
// `notice` is shown under the title (for example why setup opened again).
void setupModeRun(bool rerun, uint32_t timeoutMs, const char *notice = nullptr);
