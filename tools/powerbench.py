#!/usr/bin/env python3
"""Measure what each part of sleep mode saves, from the battery sensor.

Unplug USB first: on USB the sensor shows charge current, not what the Tab5 uses. The script refuses to run while
the Tab5 reports charging unless --force is given. It listens on the viewer's WebSocket listener (localhost:9001)
and sends commands on tab5/cmd, so close the browser viewer while it runs.

Each step sends its commands, waits --settle seconds, then averages battery power for --measure seconds.
Steps add one thing at a time so the table shows what each is worth:

    awake, screen on  ->  screen off  ->  slow sampling and sending  ->  Wi-Fi power save  ->  display panel sleep  ->  optional lower CPU clock
    ->  the real sleep mode

Power is battery power: negative = discharging. Run:  tools/.venv/bin/python tools/powerbench.py
"""
import argparse, json, statistics, sys, threading, time

import paho.mqtt.client as mqtt

T_IMU, T_PWR, T_CMD = "tab5/imu", "tab5/power", "tab5/cmd"
BAT_WH = 14.8

STEPS = [
    ("awake, screen on",      ["wake", "rates normal", "ps off", "panel off", "cpu 0", "screen on", "brightness 180"]),
    ("screen off",            ["screen off"]),
    ("+ slow rates",          ["rates slow"]),
    ("+ Wi-Fi power save",    ["ps on"]),
    ("+ display panel sleep", ["panel on"]),
    ("+ CPU {cpu} MHz",       ["cpu {cpu}"]),
    ("sleep mode (as built)", ["cpu 0", "wake", "sleep"]),
]
SLEEP_ONLY = [STEPS[0], STEPS[-1]]          # --cpus: awake reference, then sleep mode, then one step per clock


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--settle", type=float, default=20, help="seconds to wait after a change (default 20)")
    ap.add_argument("--measure", type=float, default=90, help="seconds to average over (default 90)")
    ap.add_argument("--cpu", type=int, default=0, help="MHz for an optional CPU step (default 0 = skip; only 180, 90 and 40 are accepted)")
    ap.add_argument("--cpus", default="", help="comma list of MHz, e.g. 180,90,40: skip the build-up and measure real sleep mode "
                    "with each CPU clock (the clock only drops while the screen is off; 360 is restored before the screen lights)")
    ap.add_argument("--force", action="store_true", help="run even if the Tab5 reports charging")
    a = ap.parse_args()

    lock = threading.Lock()
    state = {"pw": None, "p": [], "imu": 0, "t0": None, "uptime": None, "resets": 0}

    def on_msg(c, u, m):
        try:
            d = json.loads(m.payload)
        except Exception:
            return
        with lock:
            if m.topic == T_PWR:
                if state["uptime"] is not None and d["t"] < state["uptime"]:
                    state["resets"] += 1
                state["uptime"] = d["t"]
                state["pw"] = d
                state["p"].append((time.time(), d["p"], d["chg"], d["v"], d["i"], d["slp"], d["scr"]))
            elif m.topic == T_IMU:
                state["imu"] += 1

    try:
        cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="powerbench", transport="websockets")
    except AttributeError:
        cli = mqtt.Client(client_id="powerbench", transport="websockets")
    cli.on_message = on_msg
    cli.connect("localhost", 9001, 30)
    cli.subscribe([(T_IMU, 0), (T_PWR, 0)])
    cli.loop_start()

    t_end = time.time() + 15
    while state["pw"] is None and time.time() < t_end:
        time.sleep(0.2)
    if state["pw"] is None:
        sys.exit("no tab5/power messages: is the Tab5 on and the broker running?")
    time.sleep(4)                               # judge by several readings, not the first one (it can read "idle" for a moment)
    with lock:
        recent = [x[2] for x in state["p"][-30:]]
    if recent and sum(1 for c in recent if c < 0) < len(recent) * 0.7 and not a.force:
        sys.exit("the Tab5 reports charging or idle (USB power). Unplug USB so the battery sensor shows the real draw, "
                 "or use --force.")

    def cmd(c):
        cli.publish(T_CMD, c)
        time.sleep(0.3)

    print(f"Settle {a.settle:g} s, measure {a.measure:g} s per step. Battery {state['pw']['soc']:.0f}%, "
          f"{state['pw']['v']:.2f} V.\n")
    rows = []
    steps = list(STEPS)
    if a.cpus:
        steps = list(SLEEP_ONLY) + [(f"sleep + CPU {m} MHz", [f"cpu {m}"]) for m in a.cpus.split(",")]
    for name, cmds in steps:
        if "{cpu}" in name and not a.cpu:
            continue
        name = name.format(cpu=a.cpu)
        for c in cmds:
            cmd(c.format(cpu=a.cpu))
        time.sleep(a.settle)
        with lock:
            state["p"].clear(); state["imu"] = 0; resets0 = state["resets"]
        t0 = time.time()
        time.sleep(a.measure)
        el = time.time() - t0
        with lock:
            samples = list(state["p"]); imu = state["imu"]; resets = state["resets"] - resets0
        if not samples:
            rows.append((name, None)); print(f"{name}: no data"); continue
        ps = [s[1] for s in samples]
        charging = sum(1 for s in samples if s[2] >= 0)
        mean = statistics.fmean(ps)
        row = dict(mean=mean, lo=min(ps), hi=max(ps), n=len(ps), pwr=len(ps) / el, imu=imu / el,
                   v=statistics.fmean(s[3] for s in samples), charging=charging, resets=resets,
                   slp=samples[-1][5], scr=samples[-1][6])
        rows.append((name, row))
        flag = "  (RESET)" if resets else ""
        print(f"{name:26} {mean:7.2f} W   {row['pwr']:4.1f} power/s  {row['imu']:5.1f} imu/s{flag}", flush=True)

    for c in ("wake", "rates normal", "ps off", "panel off", "cpu 0", "screen on", "brightness 180"):
        cmd(c)
    cli.loop_stop()

    print("\n" + "=" * 78)
    print(f"{'step':26} {'battery W':>10} {'saved':>8} {'hours/full pack':>16} {'power/s':>8} {'imu/s':>7}")
    base = None
    for name, r in rows:
        if r is None:
            print(f"{name:26} {'no data':>10}"); continue
        draw = -r["mean"]                       # positive = drawn from the battery
        if base is None:
            base = draw
        hrs = BAT_WH / draw if draw > 0.05 else float("inf")
        note = "  CHARGING?" if r["charging"] else ""
        note += "  RESET" if r["resets"] else ""
        print(f"{name:26} {draw:10.2f} {base - draw:8.2f} {hrs:16.1f} {r['pwr']:8.1f} {r['imu']:7.1f}{note}")
    print("battery W = power drawn from the battery (positive); saved = against the first row")


if __name__ == "__main__":
    main()
