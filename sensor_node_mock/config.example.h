/* ═══════════════════════════════════════════════════════════
   FireWatch sensor node — configuration TEMPLATE

   Copy this to config.h and fill it in:

       cp sensor_node_mock/config.example.h sensor_node_mock/config.h

   config.h is gitignored because it holds your Wi-Fi password.
   This file is the committed copy — keep it free of secrets.
═══════════════════════════════════════════════════════════ */
#pragma once

/* ── Wi-Fi ────────────────────────────────────────────────────
   The ESP32-WROOM-32 has a 2.4 GHz radio ONLY. If your router
   publishes one name for both bands, the board may still fail to
   associate — split the SSIDs or force the 2.4 GHz one here. */
#define WIFI_SSID      "YOUR_2GHZ_SSID"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

/* ── Where to post ────────────────────────────────────────────
   The laptop running docker compose, on the plain-HTTP port that
   sensor-service publishes. NOT https://…:443 — that is nginx's
   self-signed cert, which this board has no reason to fight.

   Find it with:  ipconfig getifaddr en0
   It is a DHCP lease: reserve it on the router, or this stops
   working the day it changes. */
#define SERVER_URL     "http://192.168.1.118:8022/api/sensors/ingest"

/* Must match SENSOR_INGEST_KEY in the web app's .env.
   Leave empty while that is unset (the default). */
#define DEVICE_KEY     ""

/* ── Identity ─────────────────────────────────────────────────
   deviceId is the dashboard's key for this board — give each node
   in the network its own. zoneId should match an id from
   alert-service/zones_seed.py (fabric-store, cutting-floor,
   dyeing, sewing-a, warehouse, boiler, finishing). */
#define DEVICE_ID      "node-01"
#define ZONE_ID        "fabric-store"

/* ── Timing ───────────────────────────────────────────────────
   The bridge reports a node stale after 15s by default, so keep
   this comfortably under a third of that. */
#define POST_INTERVAL_MS   3000
#define HTTP_TIMEOUT_MS    5000
#define SERIAL_BAUD        115200

/* ── Mock mode ────────────────────────────────────────────────
   1 = synthesise readings (no sensors need to be wired).
   0 = read the real modules — see readRealSensors() in the .ino,
       which is where you add the DHT library and your MQ curves. */
#define MOCK_MODE      1

/* Cycle NORMAL → SMOULDERING → FIRE → back, so a demo shows every
   dashboard state without anyone touching the board. Set to 0 to
   stay in one scenario and drive it from the serial monitor
   (n / s / f / a). */
#define AUTO_SCENARIO        1
#define SCENARIO_HOLD_MS     30000   // time in each scenario

/* ── Pin map ──────────────────────────────────────────────────
   Unused while MOCK_MODE is 1, but wire to these when the real
   modules arrive.

   ⚠ ADC2 DOES NOT WORK WHILE WI-FI IS ON. That rules out GPIO
   0/2/4/12–15/25–27 for any analogRead — they return garbage the
   moment the radio starts. Both MQ analog outputs must therefore
   sit on ADC1: GPIO 32–39.

   ⚠ The MQ-2 and MQ-7 are 5V parts and their AO swings above the
   ESP32's 3.3V limit. Put a divider (e.g. 10k/20k) on each AO, or
   you will cook the pin.

   Digital pins are unaffected by the ADC2 rule, so the flame DO
   and the DHT22 data line are fine where they are. */
#define PIN_MQ2_AO      34   // ADC1_CH6, input-only
#define PIN_MQ7_AO      35   // ADC1_CH7, input-only
#define PIN_FLAME_DO    27   // digital, active-LOW on most 4-pin modules
#define PIN_FLAME_AO    32   // ADC1_CH4 (optional; the DO alone is enough)
#define PIN_DHT22       4    // digital 1-wire
