# Tab5 IMU and power telemetry

Firmware for the M5Stack Tab5 (ESP32-P4, with an ESP32-C6 coprocessor that provides Wi-Fi over esp-hosted) that:

- reads the on-board BMI270 IMU at 100 Hz and draws it live on the 720x1280 screen;
- measures battery power with the INA226 and estimates stored energy and time left;
- streams everything to an MQTT broker over Wi-Fi with TLS and a login;
- takes commands back from the broker: screen on/off, brightness, microphone, sleep;
- recovers on its own when the Wi-Fi/MQTT link wedges.

A browser dashboard (`viewer/`) shows the stream, plots it, controls the Tab5 and plays the microphone.
Scripts under `broker/` set up a local Mosquitto broker, and scripts under `tools/` test the whole chain.

## Contents

| Path | What it is |
|---|---|
| `src/main.cpp` | The firmware |
| `include/` | Credentials the firmware is built with (`secrets.h` you write; the rest is generated, see Setup) |
| `broker/` | Mosquitto config, access lists, and the scripts that create certificates and start the broker |
| `viewer/index.html` | The dashboard. Opens straight from disk; no server needed |
| `tools/endurance.py` | Long-running health test of the whole chain |
| `tools/powerbench.py` | Measures what each part of sleep mode saves, from the battery sensor |
| `spike/` | A throwaway test firmware used to check that first-run setup is feasible (see "Roadmap") |

## Hardware

- M5Stack Tab5. The board definition (`m5stack-tab5-p4`) targets the "ES pre rev.300" P4 silicon.
- Battery: the firmware assumes the 2S NP-F550 pack (2000 mAh, 7.4 V, 14.8 Wh) for stored energy and time left. Change `BAT_WH` in `src/main.cpp` if you fit another battery.
- The INA226 sits on the battery path. On USB it shows charge current; on battery it shows what the whole tablet draws.
- Wi-Fi is 2.4 GHz only (the C6).

## Setup

You need PlatformIO, Podman (for the broker) and Python 3.

1. **Create the broker certificates and credentials** (one time; re-run if the Mac's LAN address changes, because the certificate is bound to it):

   ```
   broker/setup-tls.sh            # or: broker/setup-tls.sh <broker-ip>
   ```

   This creates a private CA, a server certificate, the MQTT user `tab5` with a random password, and the generated headers `include/ca_cert.h` and `include/mqtt_creds.h`. None of these are committed.

2. **Write `include/secrets.h`** (not committed):

   ```cpp
   #pragma once
   #define WIFI_SSID        "your-network"
   #define WIFI_PASS        "your-password"
   #define MQTT_HOST        "192.168.1.x"     // this Mac's LAN address, as printed by broker/run.sh
   #define MQTT_PORT        8883
   #define MQTT_TOPIC       "tab5/imu"
   #define MQTT_TOPIC_POWER "tab5/power"
   #define MQTT_TOPIC_CMD   "tab5/cmd"
   #define MQTT_TOPIC_AUDIO "tab5/audio"
   ```

3. **Start the broker**:

   ```
   broker/run.sh
   ```

   Port 8883 is MQTT over TLS with a login (the Tab5). Port 9001 is MQTT over WebSockets, bound to `127.0.0.1` only, read-only plus the command topic, no login (the viewer).

4. **Build and flash**:

   ```
   pio run -t upload
   ```

   Use the `pio` that is on your PATH. The one inside `~/.platformio/penv` is too old for this platform and will uninstall it.

5. **Open the viewer**: open `viewer/index.html` in a browser. It connects to `ws://localhost:9001`.

6. **Optional, for the tools**:

   ```
   python3 -m venv tools/.venv
   tools/.venv/bin/pip install -r tools/requirements.txt
   ```

Opening the Tab5's serial port resets the board (this is how the USB serial on the P4 behaves). Expect a restart whenever a serial monitor or `--serial` opens it.

## On the Tab5

The dashboard shows a g-force scope, gyro rate dials, a linear-acceleration history, and a 30 s battery power chart. The header has two small status symbols and the message rate:

- **Wi-Fi symbol**: arcs light up with signal strength (three green at -60 dBm or better, two green to -70, one amber to -80, one red below). While joining, the arcs sweep in amber; after 30 s down they sweep in red with a slash.
- **Broker symbol**: a two-unit server. Green when connected, with the lower dot flickering as messages go out. While connecting it is amber with the dots blinking in turn; after 30 s it is red with a slash.
- **`N msg/s >>> host:port`**: the arrows sweep while the link is up and stay red and still when it is down.

### Quick settings

Swipe down from the very top edge. The dashboard dims and a panel slides down:

- a brightness slider (5-100%);
- **power symbol**: turn the screen off;
- **Wi-Fi** and **server** symbols: link status;
- **moon**: sleep mode.

Close it with a swipe up or by tapping the dimmed area. With the screen off, touching anywhere wakes it; that touch is not passed on.

### Sleep mode

Chosen from the moon tile or with the `sleep` command; it never starts by itself. It turns the screen off, stops the microphone, drops the CPU from 360 MHz to 40 MHz, samples the IMU at 10 Hz instead of 100 Hz, and sends 2 IMU readings and 1 power reading per second instead of 50 and 10. Touch the screen or send `wake` to end it. On wake the full clock is restored before the screen lights; the screen is lit about 30 ms after the touch is seen.

Measured on battery (USB unplugged), the battery draw goes from about 1.8 W awake to about 0.72 W in sleep mode: roughly 8 hours versus 20 hours on a full pack. Of the 1.08 W saved, the backlight is about 0.76 W, slower rates about 0.25 W, and the 40 MHz clock about 0.1 W. Wi-Fi deep power save added nothing measurable.

The display panel is deliberately left awake. The Tab5's ST7123 is a combined display and touch controller, so putting the panel to sleep also switches off touch and nothing could wake the screen by touch. Panel sleep would save another 0.17 W, but then only the `wake` command could end sleep.

### Recovery

The esp-hosted link to the C6 can wedge, after which TLS cannot get enough internal RAM to reconnect. The firmware:

- resets Wi-Fi after 3 failed MQTT connects, at most once a minute;
- restarts the chip after 180 s without an MQTT link, or if the transmit task stops responding for 60 s, and gives up restarting after 3 restarts in a row without a connection (so a dead broker cannot cause a reboot loop);
- draws the display in 80 px strips, which leaves about 235 kB of internal RAM free for TLS.

## MQTT interface

| Topic | Direction | Content |
|---|---|---|
| `tab5/imu` | Tab5 to broker | JSON: `t` (ms), `ax ay az` (g), `gx gy gz` (deg/s), `roll pitch yaw` (deg). 50/s awake, 2/s asleep |
| `tab5/power` | Tab5 to broker | JSON, 10/s awake, 1/s asleep: `t v i p soc chg t_imu t_chip rssi heap mic avg wh eta scr bri slp`. `i` and `p` are positive when charging; `chg` is 1 charging, 0 idle, -1 discharging; `eta` is minutes to empty or full, -1 if not meaningful |
| `tab5/audio` | Tab5 to broker | Raw 16-bit mono PCM at 16 kHz in 1024-byte blocks, only while the microphone is on |
| `tab5/cmd` | broker to Tab5 | Text commands, below |

Commands:

| Command | Effect |
|---|---|
| `screen on` / `screen off` | Turn the screen on or off (on also ends sleep) |
| `brightness 0-255` | Set the brightness |
| `mic on` / `mic off` | Start or stop microphone streaming (ignored in sleep) |
| `sleep` / `wake` | Enter or leave sleep mode |
| `rates slow` / `rates normal`, `ps on` / `ps off`, `panel on` / `panel off`, `cpu <MHz>` | The individual parts of sleep mode, for measuring. The CPU clock only changes while the screen is off. `panel on` puts the display panel to sleep, which also disables touch, so only `wake` can end sleep then |

The device's access list lets it use only `tab5/#`. The viewer's access list lets it read `tab5/#` and write only `tab5/cmd`.

## Testing

Close the browser viewer first for both tools; anything else publishing to `tab5/cmd` interferes.

```
# 30 minutes of health checks: stalls, rates, memory, reboots, commands, device log
tools/.venv/bin/python tools/endurance.py --minutes 30 --serial /dev/cu.usbmodem2101

# what each part of sleep mode saves. Unplug USB first: on USB the sensor shows charge current
tools/.venv/bin/python tools/powerbench.py
tools/.venv/bin/python tools/powerbench.py --cpus 180,90,40     # real sleep mode at each CPU clock
```

`endurance.py` prints a PASS/FAIL table; the exit status is 0 only if every check passes. While it runs the screen goes off for a couple of seconds every few minutes and the microphone turns on briefly; it restores the screen, microphone and brightness at the end.

## Notes and limits

- A lower CPU clock starves the display (noise, 1 fps) and can leave it in a bad state until a reset, so the firmware only honours it while the screen is off and restores 360 MHz before the screen lights; if the transmit task has not done so within 0.4 s the display task does it. While the clock is at 40 MHz the display driver logs "can't fetch data from external memory fast enough" on the serial port. The screen is dark then and the messages stop once the clock is back.
- The Wi-Fi signal reading and the access-point status are calls into the C6 that can block when the C6 does not answer. The firmware polls the signal every 10 s and stops for good after the first timeout; the symbol then shows plain "connected".
- `broker/setup-tls.sh` binds the server certificate to the Mac's LAN address. If DHCP gives the Mac a new address, re-run it, update `MQTT_HOST`, and reflash. Reserving the address on the router avoids this.
- The broker container restarts with Podman but not after a reboot of the Podman VM; run `broker/run.sh` again.

## Roadmap

Designed but not built yet:

- **First-run setup**: with nothing saved, the Tab5 opens its own Wi-Fi network and shows a QR code; a phone joins and fills in a setup page (Wi-Fi, broker, certificates, a PIN). Secrets are stored encrypted, the setup network closes after 10 minutes, and a PIN is needed to run setup again.
- **Broker sign-in by certificate** (mutual TLS) as an alternative to username and password, on a second broker listener.

`spike/` is the throwaway firmware that confirmed the access point, captive page, form posts and touch all work on this hardware. Build it with `pio run -e spike -t upload`; a plain `pio run` still builds the real firmware.
