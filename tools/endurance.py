#!/usr/bin/env python3
"""Endurance test for the Tab5 telemetry stream.

Connects to the broker the way the Tab5 does (TLS + login, from the credentials in include/) and, for as long
as you ask, watches everything that has gone wrong so far:

  * stream stalls and gaps        IMU (tab5/imu) and power (tab5/power) must keep arriving
  * rates                         ~50 IMU msg/s and ~10 power msg/s, on the broker path AND the browser (WebSocket) path
  * device reboots                the device clock going backwards
  * memory                        free heap floor and leak slope (the Wi-Fi RSSI bug leaked ~1 kB per failure)
  * payloads                      every field present, finite and in a sane range, power = V x I
  * device reconnects             counted from the broker log
  * commands                      brightness round trips, screen off/on, mic on/off (audio rate, level, clean stop)
  * device log (optional)         tx stalls, esp-hosted RPC errors, MQTT/TLS failures, panics
                                  (--serial restarts the Tab5 when it opens the port; the script waits for it)

Run it with the project venv:

    tools/.venv/bin/python tools/endurance.py --minutes 30
    tools/.venv/bin/python tools/endurance.py --hours 8 --serial /dev/cu.usbmodem2101

Close the browser viewer first: anything else publishing to tab5/cmd (its brightness slider, Listen button)
fights with the test. The screen is always left on, the mic off and the brightness restored when the run ends.
Exit status: 0 = every check passed, 1 = at least one failed, 2 = could not start.
"""
from __future__ import annotations

import argparse
import atexit
import datetime as dt
import json
import math
import re
import shutil
import ssl
import statistics
import subprocess
import sys
import threading
import time
from collections import defaultdict
from pathlib import Path

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is missing. Use the project venv: tools/.venv/bin/python tools/endurance.py")

ROOT = Path(__file__).resolve().parent.parent
T_IMU, T_PWR, T_AUDIO, T_CMD = "tab5/imu", "tab5/power", "tab5/audio", "tab5/cmd"
IMU_KEYS = ["t", "ax", "ay", "az", "gx", "gy", "gz", "roll", "pitch", "yaw"]
PWR_KEYS = ["t", "v", "i", "p", "soc", "chg", "t_imu", "t_chip", "rssi", "heap", "mic",
            "avg", "wh", "eta", "scr", "bri"]


# ---------------------------------------------------------------- config
def define(path: Path, name: str):
    m = re.search(rf'#define\s+{name}\s+("([^"]*)"|\S+)', path.read_text())
    if not m:
        raise KeyError(f"{name} not found in {path}")
    return m.group(2) if m.group(2) is not None else m.group(1)


def load_config():
    sec, cred = ROOT / "include/secrets.h", ROOT / "include/mqtt_creds.h"
    ca = ROOT / "broker/certs/ca.crt"
    for p in (sec, cred, ca):
        if not p.exists():
            sys.exit(f"missing {p}. Run broker/setup-tls.sh first.")
    return dict(host=define(sec, "MQTT_HOST"), port=int(define(sec, "MQTT_PORT")),
                user=define(cred, "MQTT_USER"), pw=define(cred, "MQTT_PASS"), ca=str(ca))


# ---------------------------------------------------------------- bookkeeping
class Stream:
    """Arrival bookkeeping for one topic on one connection."""

    def __init__(self, name):
        self.name, self.last, self.count = name, None, 0
        self.per_sec = defaultdict(int)
        self.in_stall, self.stall_start = False, 0.0
        self.stalls = []                       # (start_epoch, duration_s)
        self.max_gap = 0.0

    def rx(self, now, on_stall_end):
        if self.last is not None:
            self.max_gap = max(self.max_gap, now - self.last)
        if self.in_stall:
            dur = now - self.stall_start
            self.stalls.append((self.stall_start, dur))
            self.in_stall = False
            on_stall_end(self, dur)
        self.last = now
        self.count += 1
        self.per_sec[int(now)] += 1

    def watch(self, now, limit, on_stall_start):
        if self.last is not None and not self.in_stall and now - self.last > limit:
            self.in_stall, self.stall_start = True, self.last
            on_stall_start(self)

    def close(self, now):
        if self.in_stall:
            self.stalls.append((self.stall_start, now - self.stall_start))


class Test:
    def __init__(self, a, cfg):
        self.a, self.cfg = a, cfg
        self.lock = threading.RLock()
        self.t0 = time.time()
        self.stop = threading.Event()
        self.events, self.checks = [], []
        self.logf = None
        # streams: broker path (TLS) and browser path (WebSocket)
        self.s = {k: Stream(f"{k}/tls") for k in ("imu", "power", "audio")}
        self.w = {k: Stream(f"{k}/ws") for k in ("imu", "power")}
        self.disconnects = {"tls": 0, "ws": 0}
        # payload / device state
        self.pw = None                         # latest power message
        self.pw_rx = 0.0
        self.problems = []                     # payload problems
        self.parse_errors = 0
        self.bad_audio = 0
        self.last_t = {"imu": None, "power": None}
        self.reboots = []                      # (epoch, old_t, new_t)
        self.heap_quiet = []                   # (epoch, uptime_segment, heap)   mic off, no test in progress
        self.segment = 0
        self.min_heap = None
        self.rssi = []
        self.rssi_zero = 0
        self.foreign = []                      # unexpected brightness / screen changes
        self.cmd_window = 0.0                  # until when changes in bri/scr are ours
        self.last_bri = self.last_scr = None
        self.in_mic_test = False
        # active tests
        self.cmd_lat, self.cmd_lost = [], []
        self.screen_ok = self.screen_fail = 0
        self.mic_runs = []                     # dicts
        self.serial_hits = defaultdict(list)
        self.orig = {}

    # ---- logging
    def ev(self, kind, msg):
        line = f"[{self.el():>8}] {kind:<5} {msg}"
        print(line, flush=True)
        self.events.append(line)
        if self.logf:
            self.logf.write(line + "\n")
            self.logf.flush()

    def el(self):
        s = int(time.time() - self.t0)
        return f"{s // 3600:02d}:{s % 3600 // 60:02d}:{s % 60:02d}"

    # ---- payload checks
    def problem(self, topic, what):
        with self.lock:
            if len(self.problems) < 200:
                self.problems.append((topic, what))
        if len(self.problems) <= 12:
            self.ev("BAD", f"{topic}: {what}")

    @staticmethod
    def finite(x):
        return isinstance(x, (int, float)) and math.isfinite(x)

    def check_imu(self, m):
        miss = [k for k in IMU_KEYS if k not in m]
        if miss:
            return self.problem("imu", f"missing {miss}")
        for k in IMU_KEYS:
            if not self.finite(m[k]):
                return self.problem("imu", f"{k} not finite: {m[k]!r}")
        if max(abs(m["ax"]), abs(m["ay"]), abs(m["az"])) > 20:
            self.problem("imu", f"accel out of range {m['ax']},{m['ay']},{m['az']}")
        if max(abs(m["gx"]), abs(m["gy"]), abs(m["gz"])) > 3000:
            self.problem("imu", f"gyro out of range {m['gx']},{m['gy']},{m['gz']}")

    def check_power(self, m):
        miss = [k for k in PWR_KEYS if k not in m]
        if miss:
            return self.problem("power", f"missing {miss}")
        for k in PWR_KEYS:
            if not self.finite(m[k]):
                return self.problem("power", f"{k} not finite: {m[k]!r}")
        rng = {"v": (5.5, 9.0), "soc": (0, 100), "t_imu": (5, 95), "t_chip": (5, 105), "wh": (0, 16),
               "bri": (0, 255), "heap": (1000, 600000)}
        for k, (lo, hi) in rng.items():
            if not lo <= m[k] <= hi:
                self.problem("power", f"{k}={m[k]} outside {lo}..{hi}")
        if m["chg"] not in (-1, 0, 1):
            self.problem("power", f"chg={m['chg']}")
        if m["scr"] not in (0, 1):
            self.problem("power", f"scr={m['scr']}")
        if m["eta"] != -1 and m["eta"] < 0:
            self.problem("power", f"eta={m['eta']}")
        if abs(m["p"] - m["v"] * m["i"]) > 0.02 + 0.03 * abs(m["p"]):
            self.problem("power", f"p={m['p']} != v*i={m['v'] * m['i']:.3f}")

    def check_clock(self, key, m, now):
        t = m.get("t")
        if not self.finite(t):
            return
        prev = self.last_t[key]
        if prev is not None and t < prev - 2000:
            with self.lock:
                self.reboots.append((now, prev, t))
                self.segment += 1
            self.ev("REBOOT", f"{key}: device clock went {prev} -> {t} ms (device restarted)")
        self.last_t[key] = t

    # ---- message handlers (paho network threads)
    def on_stall_start(self, st):
        self.ev("STALL", f"{st.name}: nothing for >{self.a.stall:.1f}s")

    def on_stall_end(self, st, dur):
        self.ev("STALL", f"{st.name}: resumed after {dur:.1f}s")

    def on_msg(self, topic, payload, ws=False):
        now = time.time()
        grp = self.w if ws else self.s
        if topic == T_AUDIO:
            if not ws:
                self._audio(now, payload)
            return
        key = "imu" if topic == T_IMU else "power" if topic == T_PWR else None
        if key is None:
            return
        with self.lock:
            grp[key].rx(now, self.on_stall_end)
        if ws:
            return                                        # the WebSocket client only measures delivery
        try:
            m = json.loads(payload)
        except ValueError:
            with self.lock:
                self.parse_errors += 1
            self.problem(key, "invalid JSON")
            return
        if key == "power":                                # one clock is enough to see a restart
            self.check_clock(key, m, now)
        if key == "imu":
            self.check_imu(m)
            return
        self.check_power(m)
        with self.lock:
            self.pw, self.pw_rx = m, now
            if self.finite(m.get("heap")):
                h = m["heap"]
                self.min_heap = h if self.min_heap is None else min(self.min_heap, h)
                if h < self.a.min_heap * 1024 and now - getattr(self, "_heap_warn", 0) > 5:
                    self._heap_warn = now
                    self.ev("HEAP", f"free heap {h / 1024:.1f} kB (mic {'on' if m.get('mic', -99) > -90 else 'off'}, "
                                    f"rssi {m.get('rssi')}, mic test {'running' if self.in_mic_test else 'idle'})")
                if m.get("mic", 0) <= -90 and not self.in_mic_test:
                    self.heap_quiet.append((now, self.segment, h))
            if self.finite(m.get("rssi")):
                self.rssi.append(m["rssi"])
                self.rssi_zero += m["rssi"] == 0
            bri, scr = m.get("bri"), m.get("scr")
            if self.last_bri is not None and (bri != self.last_bri or scr != self.last_scr) and now > self.cmd_window \
                    and not self.stop.is_set():
                self.foreign.append((now, self.last_bri, bri, self.last_scr, scr))
                self.ev("WARN", f"brightness/screen changed by someone else: {self.last_bri}->{bri}, "
                                f"screen {self.last_scr}->{scr}. Is the viewer open?")
            self.last_bri, self.last_scr = bri, scr

    def _audio(self, now, payload):
        with self.lock:
            self.s["audio"].rx(now, lambda *_: None)
            if len(payload) != 1024:
                self.bad_audio += 1

    # ---- helpers for active tests
    def send(self, cmd):
        self.cmd_window = time.time() + 4
        self.cli.publish(T_CMD, cmd, qos=0)

    def wait_for(self, pred, timeout):
        end = time.time() + timeout
        while time.time() < end and not self.stop.is_set():
            with self.lock:
                if pred():
                    return time.time()
            time.sleep(0.02)
        return None

    def fresh(self, since):
        return self.pw is not None and self.pw_rx >= since

    # ---- active tests
    def test_brightness(self):
        with self.lock:
            orig = self.pw["bri"] if self.pw else 180
        for lvl in (30, 120, 220, orig):
            t_send = time.time()
            self.send(f"brightness {lvl}")
            t_ack = self.wait_for(lambda: self.fresh(t_send) and self.pw["bri"] == lvl, self.a.cmd_timeout)
            if t_ack is None:
                self.cmd_lost.append(("brightness", lvl))
                self.ev("FAIL", f"brightness {lvl} not applied within {self.a.cmd_timeout:.0f}s")
            else:
                self.cmd_lat.append(t_ack - t_send)
                self.ev("TEST", f"brightness {lvl}: applied in {(t_ack - t_send) * 1000:.0f} ms")

    def test_screen(self):
        t = time.time()
        self.send("screen off")
        off = self.wait_for(lambda: self.fresh(t) and self.pw["scr"] == 0, self.a.cmd_timeout)
        time.sleep(1.0)
        t = time.time()
        self.send("screen on")
        on = self.wait_for(lambda: self.fresh(t) and self.pw["scr"] == 1, self.a.cmd_timeout)
        if off and on:
            self.screen_ok += 1
            self.ev("TEST", "screen off/on: ok")
        else:
            self.screen_fail += 1
            self.ev("FAIL", f"screen cycle: off={'ok' if off else 'LOST'} on={'ok' if on else 'LOST'}")
            self.send("screen on")                       # never leave the screen dark

    def test_mic(self):
        run = dict(ok=True, why=[])
        with self.lock:
            self.in_mic_test = True
            h0 = self.pw["heap"] if self.pw else None
        try:
            n0 = self.s["audio"].count
            t = time.time()
            self.send("mic on")
            if self.wait_for(lambda: self.s["audio"].count - n0 >= 10, 6) is None:
                run["ok"] = False
                run["why"].append("no audio after mic on")
            else:
                n1, t1 = self.s["audio"].count, time.time()
                time.sleep(3.0)
                rate = (self.s["audio"].count - n1) / (time.time() - t1)
                run["rate"] = rate
                with self.lock:
                    lvl = self.pw["mic"] if self.pw else -99
                run["level"] = lvl
                if not 25 <= rate <= 36:
                    run["ok"] = False
                    run["why"].append(f"audio rate {rate:.1f}/s (expect ~31)")
                if lvl <= -90:
                    run["ok"] = False
                    run["why"].append("mic level reads silence (-99)")
        finally:
            t = time.time()
            self.send("mic off")
            quiet = self.wait_for(lambda: self.fresh(t) and self.pw["mic"] <= -90, 5)
            time.sleep(1.2)
            n = self.s["audio"].count
            time.sleep(1.0)
            if quiet is None or self.s["audio"].count - n > 1:
                run["ok"] = False
                run["why"].append("audio kept flowing / mic level not reset after mic off")
            time.sleep(1.0)
            with self.lock:
                h1 = self.pw["heap"] if self.pw else None
                self.in_mic_test = False
            run["heap_delta"] = (h1 - h0) if (h0 is not None and h1 is not None) else None
        self.mic_runs.append(run)
        if run["ok"]:
            self.ev("TEST", f"mic on/off: ok, audio {run['rate']:.0f}/s, level {run['level']:.0f} dBFS, "
                            f"heap change {((run.get('heap_delta') or 0) / 1024):+.1f} kB")
        if not run["ok"]:
            self.ev("FAIL", "mic cycle: " + "; ".join(run["why"]))

    def actions(self):
        a, nxt = self.a, {}
        for name, every in (("cmd", a.cmd_every), ("screen", a.screen_every), ("mic", a.mic_every)):
            if every > 0:
                nxt[name] = time.time() + every / 2          # first run after half an interval
        fns = {"cmd": self.test_brightness, "screen": self.test_screen, "mic": self.test_mic}
        every = {"cmd": a.cmd_every, "screen": a.screen_every, "mic": a.mic_every}
        while not self.stop.is_set():
            now = time.time()
            for name in list(nxt):
                if now >= nxt[name] and now < self.end - 15:
                    try:
                        fns[name]()
                    except Exception as e:                   # a test bug must not end the run silently
                        self.ev("ERR", f"{name} test crashed: {e!r}")
                    nxt[name] = time.time() + every[name]
            self.stop.wait(0.5)

    # ---- optional device log
    PATTERNS = {
        "tx stall": r"tx stall",
        "rpc error": r"rpc_core|RPC_WRAP",
        "mqtt connect failed": r"mqtt connect failed",
        "tls error": r"start_ssl_client|NetworkClientSecure",
        "panic": r"Guru Meditation|abort\(\)|Backtrace|panic",
        "rssi gave up": r"rssi unavailable",
        "alloc failure": r"alloc failed|out of memory|OOM",
    }

    def serial_reader(self, port):
        try:
            import serial
        except ImportError:
            self.ev("WARN", "pyserial missing, skipping --serial")
            return
        try:
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = port, 115200, 0.5
            s.dtr = s.rts = False                           # do not poke the reset lines
            s.open()
        except Exception as e:
            self.ev("WARN", f"cannot open {port}: {e}")
            return
        self.ev("INFO", f"reading device log from {port}")
        while not self.stop.is_set():
            line = s.readline().decode(errors="replace").strip()
            for name, pat in self.PATTERNS.items():
                if line and re.search(pat, line):
                    self.serial_hits[name].append(line)
                    self.ev("DEV", line[:160])
                    break
        s.close()

    # ---- broker log
    def broker_reconnects(self):
        if not shutil.which("podman"):
            return None
        try:
            since = f"{int(time.time() - self.t0) + 2}s"
            out = subprocess.run(["podman", "logs", "--since", since, self.a.container],
                                 capture_output=True, text=True, timeout=30)
            text = out.stdout + out.stderr
        except Exception:
            return None
        conn = re.findall(r"New client connected from \S+ as (tab5-\w+)", text)
        drop = re.findall(r"Client (tab5-\w+) \[[^\]]*\] disconnected(?:: ([^\n]*))?", text)
        return len(conn), drop

    # ---- run
    def run(self):
        a, cfg = self.a, self.cfg

        def make(cid, transport="tcp"):
            try:
                return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=cid, transport=transport)
            except AttributeError:
                return mqtt.Client(client_id=cid, transport=transport)

        def mk_conn(name):
            def on_connect(c, u, flags, rc, *x):
                if getattr(rc, "is_failure", rc != 0):
                    self.ev("ERR", f"{name} connect refused: {rc}")
                    return
                c.subscribe([(T_IMU, 0), (T_PWR, 0)] + ([(T_AUDIO, 0)] if name == "tls" else []))

            def on_disc(c, u, *x):
                if not self.stop.is_set():
                    self.disconnects[name] += 1
                    self.ev("DISC", f"test client ({name}) lost its broker connection")
            return on_connect, on_disc

        tag = f"endurance-{int(time.time()) % 100000}"
        self.cli = make(tag)
        self.cli.tls_set(ca_certs=cfg["ca"], cert_reqs=ssl.CERT_REQUIRED)
        self.cli.username_pw_set(cfg["user"], cfg["pw"])
        self.cli.on_connect, self.cli.on_disconnect = mk_conn("tls")
        self.cli.on_message = lambda c, u, m: self.on_msg(m.topic, m.payload)
        self.cli.reconnect_delay_set(1, 5)
        self.cli.connect_async(cfg["host"], cfg["port"], keepalive=30)
        self.cli.loop_start()

        self.wsc = None
        if a.ws:
            self.wsc = make(tag + "-ws", "websockets")
            self.wsc.on_connect, self.wsc.on_disconnect = mk_conn("ws")
            self.wsc.on_message = lambda c, u, m: self.on_msg(m.topic, m.payload, ws=True)
            self.wsc.reconnect_delay_set(1, 5)
            self.wsc.connect_async("localhost", 9001, keepalive=30)
            self.wsc.loop_start()

        Path(a.logdir).mkdir(parents=True, exist_ok=True)
        logpath = Path(a.logdir) / f"endurance-{dt.datetime.now():%Y%m%d-%H%M%S}.log"
        self.logf = open(logpath, "w")
        print(f"log: {logpath}")

        if a.serial:
            # Opening the port resets the Tab5 on this board (even with DTR/RTS held low), so do it first and let
            # the device come back before the clock starts. The reset is not a fault in the firmware.
            threading.Thread(target=self.serial_reader, args=(a.serial,), daemon=True).start()
            print("opened the serial port; the Tab5 restarts when it is opened, waiting for it to come back...", flush=True)
            time.sleep(4)
        print(f"waiting for the Tab5 (up to {a.wait:.0f}s)...", flush=True)
        if self.wait_for(lambda: self.s["imu"].count > 20 and self.s["power"].count > 3, a.wait) is None:
            self.ev("FAIL", "no IMU/power data from the Tab5. Is it on, on Wi-Fi, and is the broker running?")
            self.checks.append(("device is streaming at start", False, "no data"))
            return self.finish(logpath, started=False)

        if a.serial:                                         # let the restarted device settle (NTP, TLS, first publishes)
            self.wait_for(lambda: self.pw is not None and self.pw["t"] > 20000, 60)
            time.sleep(2)
        self.t0 = time.time()
        self.end = self.t0 + a.duration
        self.reboots.clear()
        self.segment = 0
        self.heap_quiet.clear()
        self.min_heap = None
        self.rssi.clear()                                    # boot-time samples (before the first reading) do not count
        self.rssi_zero = 0
        self.problems.clear()
        self.foreign.clear()
        self.parse_errors = self.bad_audio = 0
        with self.lock:
            self.orig = dict(bri=self.pw["bri"], scr=self.pw["scr"])
        self.last_bri, self.last_scr = self.orig["bri"], self.orig["scr"]
        atexit.register(self.restore)
        self.ev("INFO", f"started: {a.duration / 60:.0f} min, brightness {self.orig['bri']}, "
                        f"heap {self.pw['heap'] / 1024:.0f} kB, rssi {self.pw['rssi']}, device uptime {self.pw['t'] / 1000:.0f} s")
        for s in list(self.s.values()) + list(self.w.values()):
            s.stalls.clear()
            s.max_gap = 0.0
            s.per_sec.clear()
            s.count = 0
        threading.Thread(target=self.actions, daemon=True).start()
        for v in self.serial_hits.values():                  # lines from the restart itself do not count
            v.clear()

        next_status = time.time() + a.status
        try:
            while time.time() < self.end:
                time.sleep(0.25)
                now = time.time()
                with self.lock:
                    for st in list(self.s.values())[:2] + list(self.w.values()):
                        st.watch(now, a.stall, self.on_stall_start)
                if now >= next_status:
                    next_status = now + a.status
                    self.status_line(now)
        except KeyboardInterrupt:
            self.ev("INFO", "interrupted, finishing")
        self.stop.set()
        self.finish(logpath, started=True)

    def status_line(self, now):
        sec = int(now) - 1
        pw = self.pw or {}
        cmds = f"{len(self.cmd_lat)}/{len(self.cmd_lat) + len(self.cmd_lost)}"
        mics = f"{sum(r['ok'] for r in self.mic_runs)}/{len(self.mic_runs)}"
        ws = f" ws {self.w['imu'].per_sec[sec]}/{self.w['power'].per_sec[sec]}" if self.a.ws else ""
        print(f"[{self.el():>8}] imu {self.s['imu'].per_sec[sec]}/s power {self.s['power'].per_sec[sec]}/s{ws} "
              f"| heap {pw.get('heap', 0) / 1024:.0f}k (min {(self.min_heap or 0) / 1024:.0f}k) rssi {pw.get('rssi', '?')} "
              f"| stalls {sum(len(s.stalls) for s in self.s.values())} reboots {len(self.reboots)} "
              f"disc {sum(self.disconnects.values())} | cmds {cmds} screen {self.screen_ok}/{self.screen_ok + self.screen_fail} "
              f"mic {mics} bad {len(self.problems)}", flush=True)

    def restore(self):
        if getattr(self, "restored", False) or not self.orig:
            return
        self.restored = True
        try:
            self.cli.publish(T_CMD, "mic off")
            self.cli.publish(T_CMD, "screen on")
            self.cli.publish(T_CMD, f"brightness {self.orig['bri']}")
            if not self.orig["scr"]:
                time.sleep(0.5)
                self.cli.publish(T_CMD, "screen off")
            time.sleep(0.6)
        except Exception:
            pass

    # ---- verdict
    def add(self, name, ok, detail):
        self.checks.append((name, ok, detail))

    def finish(self, logpath, started):
        now = time.time()
        if started:
            self.restore()
            for st in list(self.s.values()) + list(self.w.values()):
                st.close(now)
            a = self.a
            run = max(now - self.t0, 1)
            warm = int(self.t0) + 5

            def rates(st):
                v = [c for sec, c in st.per_sec.items() if sec >= warm and sec < int(now)]
                return (statistics.median(v) if v else 0, min(v) if v else 0)

            for st, want in ((self.s["imu"], a.min_imu), (self.s["power"], a.min_power),
                             (self.w["imu"], a.min_imu), (self.w["power"], a.min_power)):
                if st.name.endswith("/ws") and not a.ws:
                    continue
                med, lo = rates(st)
                self.add(f"{st.name} rate", med >= want,
                         f"median {med:.0f}/s (need >= {want:g}), slowest second {lo}/s, "
                         f"longest gap {st.max_gap:.2f}s")
            stalls = [(s.name, st, d) for s in list(self.s.values())[:2] + list(self.w.values())
                      for st, d in s.stalls]
            self.add("no stalls", not stalls,
                     "none" if not stalls else f"{len(stalls)} stall(s), worst {max(d for *_, d in stalls):.1f}s: "
                     + ", ".join(f"{n} {d:.1f}s@{dt.datetime.fromtimestamp(st):%H:%M:%S}" for n, st, d in stalls[:6]))
            self.add("no device reboots", not self.reboots,
                     "none" if not self.reboots else f"{len(self.reboots)} reboot(s)")
            self.add("test client stayed connected", sum(self.disconnects.values()) == 0,
                     f"tls {self.disconnects['tls']}, ws {self.disconnects['ws']} disconnect(s)")

            # memory
            if self.min_heap is not None:
                self.add("free heap floor", self.min_heap >= a.min_heap * 1024,
                         f"lowest {self.min_heap / 1024:.0f} kB (need >= {a.min_heap} kB)")
            seg = self.segment
            pts = [(t, h) for t, s, h in self.heap_quiet if s == seg]
            if len(pts) >= 30 and pts[-1][0] - pts[0][0] >= a.leak_min * 60:
                x = [(t - pts[0][0]) / 60 for t, _ in pts]
                y = [h / 1024 for _, h in pts]
                mx, my = statistics.fmean(x), statistics.fmean(y)
                slope = sum((xi - mx) * (yi - my) for xi, yi in zip(x, y)) / max(sum((xi - mx) ** 2 for xi in x), 1e-9)
                self.add("no memory leak", slope > -a.leak_slope,
                         f"{slope:+.2f} kB/min over {x[-1]:.0f} min (fail below -{a.leak_slope:g}), "
                         f"{y[0]:.0f} -> {y[-1]:.0f} kB")
            else:
                self.add("no memory leak", True, f"skipped: need >= {a.leak_min:g} min without a reboot")

            # payloads
            self.add("payloads valid", not self.problems and self.bad_audio == 0,
                     "all fields present, finite, in range, p = V x I" if not self.problems and not self.bad_audio
                     else f"{len(self.problems)} problem(s), e.g. {self.problems[0]}; {self.bad_audio} odd audio block(s)")
            if self.rssi:
                nz = [x for x in self.rssi if x]
                self.add("Wi-Fi signal", self.rssi_zero == 0 and bool(nz) and min(nz) > -80,
                         f"weakest {min(nz) if nz else 'n/a'} dBm (fail below -80), "
                         f"{self.rssi_zero} sample(s) unavailable")

            # commands
            if self.cmd_lat or self.cmd_lost:
                med = statistics.median(self.cmd_lat) if self.cmd_lat else 99
                self.add("brightness commands", not self.cmd_lost and med <= a.cmd_median,
                         f"{len(self.cmd_lat)} ok, {len(self.cmd_lost)} lost, median {med * 1000:.0f} ms, "
                         f"worst {max(self.cmd_lat, default=0) * 1000:.0f} ms")
            if self.screen_ok or self.screen_fail:
                self.add("screen off/on", self.screen_fail == 0, f"{self.screen_ok} ok, {self.screen_fail} failed")
            if self.mic_runs:
                bad = [r for r in self.mic_runs if not r["ok"]]
                dh = [r["heap_delta"] for r in self.mic_runs if r.get("heap_delta") is not None]
                leak = statistics.fmean(dh) / 1024 if dh else 0
                rates_ = [r["rate"] for r in self.mic_runs if "rate" in r]
                lv = [r["level"] for r in self.mic_runs if "level" in r]
                self.add("mic on/off", not bad and leak > -a.mic_leak,
                         f"{len(self.mic_runs) - len(bad)}/{len(self.mic_runs)} ok, audio "
                         f"{statistics.fmean(rates_):.0f}/s, level {statistics.fmean(lv):.0f} dBFS, "
                         f"heap change per cycle {leak:+.1f} kB" if rates_ else f"{len(bad)} failed")

            # broker log + serial
            br = self.broker_reconnects()
            if br is not None:
                n, drops = br
                self.add("device never reconnected", n == 0,
                         "no reconnects in the broker log" if n == 0 else
                         f"{n} reconnect(s). Reasons: " + "; ".join(sorted({(r or 'clean') for _, r in drops})))
            else:
                self.add("device never reconnected", True, "skipped: podman/broker log not available")
            if a.serial:
                bad = {k: v for k, v in self.serial_hits.items()}
                self.add("device log clean", not bad,
                         "no stalls, RPC/TLS errors or panics" if not bad else
                         ", ".join(f"{k} x{len(v)}" for k, v in bad.items()))
            self.add("no one else controlling it", not self.foreign,
                     "no unexpected brightness/screen changes" if not self.foreign else
                     f"{len(self.foreign)} unexpected change(s): close the viewer during the test")

        # report
        width = max((len(n) for n, *_ in self.checks), default=10)
        lines = ["", "=" * 78, f"ENDURANCE RESULT  ({(now - self.t0) / 60:.1f} min)", "=" * 78]
        for n, ok, d in self.checks:
            lines.append(f"  {'PASS' if ok else 'FAIL'}  {n:<{width}}  {d}")
        failed = [n for n, ok, _ in self.checks if not ok]
        lines.append("-" * 78)
        lines.append("ALL CHECKS PASSED" if not failed and started else f"{len(failed)} CHECK(S) FAILED: " + ", ".join(failed))
        text = "\n".join(lines)
        print(text)
        if self.logf:
            self.logf.write(text + "\n")
            self.logf.close()
        self.cli.loop_stop()
        if self.wsc:
            self.wsc.loop_stop()
        return 0 if (started and not failed) else 1


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    d = p.add_mutually_exclusive_group()
    d.add_argument("--minutes", type=float, default=10)
    d.add_argument("--hours", type=float)
    p.add_argument("--serial", metavar="PORT", help="also read the device log, e.g. /dev/cu.usbmodem2101")
    p.add_argument("--no-ws", dest="ws", action="store_false", help="skip the browser (WebSocket) path")
    p.add_argument("--cmd-every", type=float, default=60, help="seconds between brightness tests (0 = never)")
    p.add_argument("--screen-every", type=float, default=300, help="seconds between screen off/on tests (0 = never)")
    p.add_argument("--mic-every", type=float, default=300, help="seconds between mic on/off tests (0 = never)")
    p.add_argument("--status", type=float, default=30, help="seconds between status lines")
    p.add_argument("--stall", type=float, default=1.5, help="a gap longer than this (s) is a stall")
    p.add_argument("--min-imu", type=float, default=45)
    p.add_argument("--min-power", type=float, default=8)
    p.add_argument("--min-heap", type=float, default=60, help="kB, lowest acceptable free heap")
    p.add_argument("--leak-slope", type=float, default=1.0, help="kB/min of heap loss that fails")
    p.add_argument("--leak-min", type=float, default=3, help="minutes of data needed for the leak check")
    p.add_argument("--mic-leak", type=float, default=8, help="kB lost per mic cycle that fails")
    p.add_argument("--cmd-timeout", type=float, default=3)
    p.add_argument("--cmd-median", type=float, default=1.0)
    p.add_argument("--wait", type=float, default=60, help="seconds to wait for the device at start")
    p.add_argument("--container", default="tab5-mqtt")
    p.add_argument("--logdir", default=str(ROOT / "tools/logs"))
    a = p.parse_args()
    a.duration = (a.hours * 3600) if a.hours else a.minutes * 60
    try:
        cfg = load_config()
    except KeyError as e:
        sys.exit(f"config problem: {e}")
    sys.exit(Test(a, cfg).run())


if __name__ == "__main__":
    main()
