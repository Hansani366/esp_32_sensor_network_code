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
   in the network its own. zoneId must belong to the ACTIVE site,
   which SITE_KEY chooses in the web app's .env.

   home  (the default): kitchen, dining, living, verander,
         bedroom-west, bedroom-southwest, bedroom-northeast,
         bedroom-southeast
   industrial (the demo hall): fabric-store, cutting-floor,
         dyeing, sewing-a, warehouse, boiler, finishing

   A zone from the wrong site is accepted without any error, and
   the node then never matches a room on the floor plan. */
#define DEVICE_ID      "node-01"
#define ZONE_ID        "kitchen"

/* ── Timing ───────────────────────────────────────────────────
   The bridge reports a node stale after 15s by default, so keep
   this comfortably under a third of that. */
#define POST_INTERVAL_MS   3000
#define HTTP_TIMEOUT_MS    5000
#define SERIAL_BAUD        115200

/* ── Mock mode ────────────────────────────────────────────────
   1 = synthesise readings (no sensors need to be wired).
   0 = read the real modules wired as in the breadboard layout
       (the "Sensor Node Breadboard Layout, version 3" sheet).

   The template now defaults to the real circuit. Set it back to 1
   for a bare board demo. */
#define MOCK_MODE      0

/* Cycle NORMAL → SMOULDERING → FIRE → back, so a demo shows every
   dashboard state without anyone touching the board. Set to 0 to
   stay in one scenario and drive it from the serial monitor
   (n / s / f / a). Only used while MOCK_MODE is 1. */
#define AUTO_SCENARIO        1
#define SCENARIO_HOLD_MS     30000   // time in each scenario

/* ── Pin map (breadboard layout v3) ───────────────────────────
   ⚠ ADC2 DOES NOT WORK WHILE WI-FI IS ON. That rules out GPIO
   0/2/4/12–15/25–27 for any analogRead — they return garbage the
   moment the radio starts. All analogue inputs below sit on ADC1
   (GPIO 32–39).

   ⚠ The MQ-2 and MQ-7 are 5 V parts. On the board each AO goes
   through a 10 kΩ / 15 kΩ divider (10 k on top, 15 k to ground),
   so the ESP32 sees 0.6 × the module voltage. MQ_DIVIDER_GAIN
   undoes that in firmware. The node must measure 3.0 V or less
   before the ESP32 is connected. */
#define PIN_MQ2_AO      34   // ADC1_CH6, input-only. Divider node E24
#define PIN_MQ7_AO      35   // ADC1_CH7, input-only. Divider node E34
#define PIN_FLAME_AO    32   // ADC1_CH4. Flame module AO, 3.3 V supply
#define PIN_FLAME_DO    27   // digital, LOW = flame seen
#define PIN_DHT22       4    // digital 1-wire. Module powered from 3.3 V

/* Outputs. LEDs through 220 Ω to ground; the buzzer is driven
   straight from the pin (no transistor fitted), so beeps are kept
   short. The button pulls GPIO 14 to ground when pressed and uses
   the internal pull-up. */
#define PIN_LED_GREEN   25   // normal
#define PIN_LED_AMBER   26   // warning
#define PIN_LED_RED     33   // danger
#define PIN_BUZZER      13   // small 3 V active buzzer, direct drive
#define PIN_BUTTON      14   // silence / acknowledge, pressed = LOW

/* ── Analogue scaling ─────────────────────────────────────────
   10 k / 15 k divider: Vnode = Vmodule × 15 / 25 = 0.6 × Vmodule,
   so multiply the measured voltage by 1 / 0.6 = 1.667. */
#define MQ_DIVIDER_GAIN     1.667f
#define MQ_SUPPLY_V         5.0f     // heater and divider supply (VIN rail)

/* Load resistor fitted on the breakout board, in kΩ. Common values:
   MQ-2 modules 5 k (some 1 k), MQ-7 modules 10 k. Check the board or
   read the resistor marked RL. */
#define MQ2_RL_KOHM         5.0f
#define MQ7_RL_KOHM         10.0f

/* R0 is the sensor resistance in clean air. Leave the datasheet
   ratios below for a first run, then after 24 to 48 h of burn-in
   press 'c' in the serial monitor with the room well ventilated.
   It prints the measured R0 values; paste them here. */
#define MQ2_R0_KOHM         0.0f     // 0 = derive from MQ2_CLEAN_AIR_RATIO at boot
#define MQ7_R0_KOHM         0.0f     // 0 = derive from MQ7_CLEAN_AIR_RATIO at boot
#define MQ2_CLEAN_AIR_RATIO 9.83f    // Rs/R0 in clean air, MQ-2 datasheet
#define MQ7_CLEAN_AIR_RATIO 27.5f    // Rs/R0 in clean air, MQ-7 datasheet

/* ppm = A × (Rs/R0)^B, fitted from the datasheet log-log curves.
   MQ-2 uses the LPG / combustible gas curve, MQ-7 the CO curve. */
#define MQ2_CURVE_A         574.25f
#define MQ2_CURVE_B        -2.222f
#define MQ7_CURVE_A         99.042f
#define MQ7_CURVE_B        -1.518f

/* The MQ heaters need time before Rs settles. Gas ppm is reported as
   0 and the amber LED blinks until this many ms have passed. */
#define MQ_WARMUP_MS        60000

/* ── Local alarm levels ───────────────────────────────────────
   These mirror the bridge's thresholds so the LEDs and buzzer agree
   with the dashboard colours. */
#define THRESH_MQ2_WARN     400.0f
#define THRESH_MQ2_DANGER   800.0f
#define THRESH_MQ7_WARN     35.0f
#define THRESH_MQ7_DANGER   100.0f
#define THRESH_TEMP_WARN    45.0f
#define THRESH_TEMP_DANGER  60.0f

/* Buzzer pattern in DANGER: a short beep every period. Keep BEEP_MS
   short while the buzzer is driven directly from the GPIO. */
#define BUZZER_BEEP_MS      120
#define BUZZER_PERIOD_MS    1000
