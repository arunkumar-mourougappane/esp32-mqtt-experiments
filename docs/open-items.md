# Open items

A snapshot of what is still open on the low-power mode and subscriptions work, as of 2026-11-15. The design and every
decision behind it are in [low-power-mode.md](low-power-mode.md); its section 8 holds the longer notes on each item.
Nothing in the design is built yet.

## 1. Waiting on a decision

| # | Item | Notes |
|---|---|---|
| 1 | Where uploaded data goes | A call-in publishes at QoS 0, non-retained. With no subscriber connected at that moment the batch is lost. Needs an always-on collector with a persistent session, or the viewer-side PWA store. Blocks the low-power build. A collector could also push the catalog when no viewer is open. The user is still thinking about it |
| 2 | Download fallback | When the viewer cannot write the catalog file (no File System Access API, or the permission is refused): offer the changed catalog as a download, or drop the fallback and make the viewer read-only. Recommendation: drop it if only Chrome is used |

## 2. Not specified yet

| Item | Notes |
|---|---|
| Radio failures | No backoff, and no total awake budget per wake. A broker that is down costs the full 20 s radio cap at every wake |
| Records on the card | Whether delivered records are deleted, marked or kept, and what happens when the card is full (drop the oldest or the newest) |
| Access list | `broker/acl.viewer` must allow writes to `tab5/poke`, `tab5/desired`, `tab5/subscription`, `tab5/catalog`, `tab5/catalog/sha` and `tab5/catalog/push`. Every viewer login would then control the device. Decide whether that is acceptable |
| Acks and fields | The ack topic is not chosen. The sample record, ack and status contents are proposals (design sections 5 and 6) |
| Per-subscription data selection (later) | The `data` field names, and whether the selection applies when a record is sampled or when it is sent |

## 3. To test or measure

| Test | What it settles |
|---|---|
| Backlight fix | Call `M5.Display.setBrightness(0)` before `M5.begin()` in the wake path and watch a few alarm wakes on battery. Also log how long `M5.begin()` takes. The user saw the screen light for about a second at an alarm wake; the cause is the default brightness 127 applied at the end of the panel init |
| Chrome restart | What the viewer's saved file permission does after Chrome is fully restarted |
| Radio-wake cost | A spike that times boot, Wi-Fi, TLS and MQTT at a wake |
| Day-of-month alarm | Must work on the hardware before `long_s` can go above 86340 s |
| Power button during a wake | Does `M5.Power.getKeyState()` see the Tab5's button while the firmware is running? |
| FatFS rename | The "exists" result when renaming over a file came from knowledge of FatFS, not from the source. It decides the three-step swap of `subscription.json` |
| Port A 5 V while off | Multimeter on the Port A 5 V pin with no unit plugged in, the device in LPM between wakes. 0 V means the rail is off |
| Heap for a 16 KB buffer | Free internal RAM with a 16384 byte MQTT buffer, and the working memory to parse a full catalog |
| C6 supply cut and restore | Does esp-hosted recover cleanly after the supply is cut through `WLAN_PWR_EN` and restored? |
| External power detection | **Deferred by the user.** How the firmware recognises USB-C or `HVIN` power, which the low-battery rule and "a plugged-in device leaves LPM" both need. A pack in protection mode may take no charge current while the board runs from USB-C |
| Off-state power | **Deferred by the user.** Needs an external logging meter in the battery path. It decides whether the 200 mV margin of the 6200 mV limit is enough. There is no battery-life target yet |

## 4. Firmware changes the design implies (none made)

- Raise the MQTT buffer from 2048 to 16384 bytes.
- Fix the NTP and RTC gate bug in the main firmware: with a valid RTC the clock can skip NTP, so `ts` stays about a second off.
- Provisioning writes the settings to the card as well as to NVS, and the settings sync from the card to NVS.
- The standby rename in the firmware, viewer and README: the caps token `sleep` becomes `standby`, and the `slp` field becomes `stb`.
- The wake-path rules: the backlight fix, `cfg.output_power = false`, the boot check against the low-battery limit, leaving LPM when external power is present, and the no-card behaviour.

## 5. Outside the design

- The PWA (an nginx container on 127.0.0.1) is not approved.
- A DHCP reservation on the router for the Mac, so the broker's address stops changing.
- A leftover `/log.txt` on the SD card from the first spike.
- Local `main` is nine commits ahead of `origin/main`; nothing has been pushed.

## 6. Decided since the last review

The `tab5/desired` rules and bad-message handling, `lpm_active` and `armed_alarm`, the catalog SHA sync with the push id, the
pull cap (3 s in LPM, 7 s awake) and back-off, create, read, update and deprecate for subscriptions (never delete; 20 active,
40 in all), saving through the File System Access API, the 6200 mV low-battery limit in every mode with the boot check, the
no-card behaviour, the sequence number and session id rules, the standby renames, the peripherals table, and broker persistence.
