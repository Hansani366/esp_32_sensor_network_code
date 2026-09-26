#!/usr/bin/env python3
"""
FireWatch — sensor node simulator

Speaks the same JSON contract as sensor_node_mock.ino, from your laptop.
Lets you build and debug the web-app side before any board is flashed, and
afterwards isolates faults: if the dashboard updates from this but not from the
board, the problem is the board or the network, not the service.

Standard library only — no pip install.

    # default: one node, auto-cycling scenarios, against localhost
    python3 tools/mock_sender.py

    # a whole network, fixed in one state
    python3 tools/mock_sender.py --nodes 4 --scenario fire

    # point at the docker host from another machine
    python3 tools/mock_sender.py --url http://192.168.1.118:8022/api/sensors/ingest
"""

import argparse
import json
import random
import sys
import time
import urllib.error
import urllib.request

DEFAULT_URL = "http://localhost:8022/api/sensors/ingest"

# Targets either side of sensor-service's thresholds (MQ-2 warn 400 / danger
# 800, MQ-7 warn 35 / danger 100, temp warn 45 / danger 60) so every dashboard
# colour is reachable. Mirrors SCENARIOS[] in the .ino.
SCENARIOS = {
    "normal":    {"mq2": 120.0, "mq7": 4.0,   "temp": 28.0, "hum": 62.0, "flame": False},
    "smoulder":  {"mq2": 550.0, "mq7": 45.0,  "temp": 43.0, "hum": 44.0, "flame": False},
    "fire":      {"mq2": 950.0, "mq7": 140.0, "temp": 68.0, "hum": 27.0, "flame": True},
}
CYCLE = ["normal", "smoulder", "fire"]

# Zone ids of the default site (SITE_KEY=home), so extra nodes land in real
# zones. Swap these for the unit7 ids when the web app runs the demo hall.
ZONES = ["kitchen", "dining", "living", "verander",
         "bedroom-west", "bedroom-southwest", "bedroom-northeast",
         "bedroom-southeast"]


def drift(current, target, noise, lo, hi):
    """Mean-reverting random walk — noise plus a pull toward the target.

    Same shape as the sketch's drift(). Pure random() would flicker across the
    range every tick and look nothing like a sensor; an unanchored walk would
    wander off and never return.
    """
    nxt = current + (target - current) * 0.18 + random.uniform(-noise, noise)
    return max(lo, min(hi, nxt))


def raw_of(ppm, full_scale):
    """Derive raw ADC from ppm so the two fields never contradict each other."""
    return int(min(1.0, ppm / full_scale) * 4095)


class Node:
    def __init__(self, index, zone, scenario):
        self.device_id = f"sim-{index:02d}"
        self.zone = zone
        self.scenario = scenario
        self.seq = 0
        self.started = time.monotonic()
        s = SCENARIOS[scenario]
        # Start at the scenario's targets rather than at zero, so the first
        # frames on the dashboard are already meaningful.
        self.mq2, self.mq7 = s["mq2"], s["mq7"]
        self.temp, self.hum = s["temp"], s["hum"]
        self.flame = s["flame"]

    def step(self):
        s = SCENARIOS[self.scenario]
        self.mq2 = drift(self.mq2, s["mq2"], 18.0, 0.0, 2000.0)
        self.mq7 = drift(self.mq7, s["mq7"], 3.0, 0.0, 500.0)
        self.temp = drift(self.temp, s["temp"], 0.6, -10.0, 125.0)
        self.hum = drift(self.hum, s["hum"], 1.2, 0.0, 100.0)
        # The IR module is a comparator, not a curve: it latches on, with the
        # odd dropout. Flickering it exercises the dashboard's binary tile.
        self.flame = random.random() > 0.08 if s["flame"] else random.random() > 0.997

    def payload(self):
        return {
            "deviceId": self.device_id,
            "zoneId": self.zone,
            "seq": self.seq,
            "uptimeMs": int((time.monotonic() - self.started) * 1000),
            "rssi": random.randint(-72, -45),
            "mock": True,
            "readings": {
                "mq2_ppm": round(self.mq2, 1),
                "mq2_raw": raw_of(self.mq2, 2000.0),
                "mq7_ppm": round(self.mq7, 1),
                "mq7_raw": raw_of(self.mq7, 500.0),
                "flame": 1 if self.flame else 0,
                "flame_raw": 400 if self.flame else 3900,
                "temperature_c": round(self.temp, 1),
                "humidity_pct": round(self.hum, 1),
            },
        }


def post(url, key, body, timeout):
    data = json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    if key:
        req.add_header("X-Device-Key", key)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.status


def main():
    ap = argparse.ArgumentParser(description="Simulate ESP32 sensor nodes.")
    ap.add_argument("--url", default=DEFAULT_URL, help=f"ingest endpoint (default {DEFAULT_URL})")
    ap.add_argument("--key", default="", help="X-Device-Key, if SENSOR_INGEST_KEY is set")
    ap.add_argument("--nodes", type=int, default=1, help="how many nodes to simulate")
    ap.add_argument("--interval", type=float, default=3.0, help="seconds between posts")
    ap.add_argument("--scenario", choices=list(SCENARIOS) + ["auto"], default="auto",
                    help="fixed scenario, or 'auto' to cycle (default)")
    ap.add_argument("--hold", type=float, default=30.0, help="seconds per scenario when auto")
    ap.add_argument("--timeout", type=float, default=5.0, help="HTTP timeout")
    args = ap.parse_args()

    start_scn = "normal" if args.scenario == "auto" else args.scenario
    nodes = [Node(i + 1, ZONES[i % len(ZONES)], start_scn) for i in range(args.nodes)]

    print(f"→ {args.url}")
    print(f"  {len(nodes)} node(s), every {args.interval}s, scenario={args.scenario}")
    print("  Ctrl-C to stop\n")

    cycle_at = time.monotonic()
    cycle_idx = 0
    ok = fail = 0

    try:
        while True:
            if args.scenario == "auto" and time.monotonic() - cycle_at >= args.hold:
                cycle_idx = (cycle_idx + 1) % len(CYCLE)
                cycle_at = time.monotonic()
                for n in nodes:
                    n.scenario = CYCLE[cycle_idx]
                print(f"\n── scenario → {CYCLE[cycle_idx].upper()} ──")

            for n in nodes:
                n.step()
                try:
                    post(args.url, args.key, n.payload(), args.timeout)
                    ok += 1
                    r = n.payload()["readings"]
                    print(f"  {n.device_id} {n.scenario:<9} "
                          f"MQ2 {r['mq2_ppm']:>6.0f}ppm  MQ7 {r['mq7_ppm']:>5.0f}ppm  "
                          f"flame {'YES' if n.flame else 'no ':<3}  "
                          f"{r['temperature_c']:>5.1f}°C  {r['humidity_pct']:>3.0f}%RH")
                except urllib.error.HTTPError as e:
                    fail += 1
                    print(f"  {n.device_id} HTTP {e.code}: {e.read().decode()[:160]}")
                except urllib.error.URLError as e:
                    fail += 1
                    print(f"  {n.device_id} unreachable: {e.reason}")
                    print("    → is sensor-service up?  docker compose ps sensor-service")
                n.seq += 1

            time.sleep(args.interval)
    except KeyboardInterrupt:
        print(f"\nstopped — {ok} accepted, {fail} failed")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
