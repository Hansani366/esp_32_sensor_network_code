# ESP32 Sensor Network — FireWatch nodes

Firmware for the sensor units in `diagram.png`: an **ESP32-WROOM-32** carrying an
**MQ-2** (gas/smoke), **MQ-7** (carbon monoxide), **IR flame module** and **DHT22**
(temperature/humidity), reporting over Wi-Fi to the FireWatch dashboard.

Ships in **mock mode** — the readings are synthesised, so a bare board with nothing
wired to it still drives the dashboard end to end. Wire the real modules later and
flip one flag.

```
ESP32 node ──HTTP POST──▶ sensor-service :8022 ──▶ nginx ──HTTPS──▶ Dashboard
  (mock readings, every 3s)      (on your laptop)            (Sensor Network card)
```

---

## Layout

```
sensor_node_mock/
├── sensor_node_mock.ino   # the sketch — mock generators, Wi-Fi, POST loop
├── config.example.h       # template, committed
└── config.h               # ← your settings; GITIGNORED, holds the Wi-Fi password
tools/
└── mock_sender.py         # simulate nodes from your laptop, no board needed
```

`config.h` is gitignored because it holds your Wi-Fi password, so it is **not** in
a fresh clone. The sketch falls back to `config.example.h` when it is missing, so a
clone still compiles and runs — it just cannot join Wi-Fi until you supply real
credentials:

```bash
cp sensor_node_mock/config.example.h sensor_node_mock/config.h
# then edit config.h — it takes precedence over the template
```

---

## Quick start

### 1. Bring up the web app

```bash
cd ../fire_detection_and_classification_web_app
docker compose up --build -d sensor-service
```

### 2. Prove the pipeline before touching hardware

```bash
python3 tools/mock_sender.py
```

Open the dashboard — the **Sensor Network** card should start filling in. Doing this
first means that if the board later fails, you already know the server side works.

```bash
python3 tools/mock_sender.py --nodes 4 --scenario fire   # a whole network, alarming
python3 tools/mock_sender.py --help                      # all options
```

### 3. Flash the board

1. Arduino IDE → **Boards Manager** → install **esp32** (Espressif Systems).
2. Select **ESP32 Dev Module**, and the port your board enumerated on.
3. `cp sensor_node_mock/config.example.h sensor_node_mock/config.h`, then edit it:
   - `WIFI_SSID` / `WIFI_PASSWORD` — **2.4 GHz**, see below
   - `SERVER_URL` — your laptop's LAN address, port 8022
   - `DEVICE_ID` — unique per node
4. Upload, then open the Serial Monitor at **115200**.

No libraries are needed: the JSON is built with `snprintf` and the mock values need
no sensor driver. (Wiring the real DHT22 later does add one.)

Expected serial output:

```
═══ FireWatch sensor node ═══
  device : node-01  (zone fabric-store)
  mode   : MOCK — no sensors required
  target : http://192.168.1.118:8022/api/sensors/ingest every 3000ms
  keys   : n normal · s smouldering · f fire · a auto · ? status

[wifi] connecting to "your-ssid" .....
[wifi] connected — board IP 192.168.1.203, RSSI -54 dBm
[post] #0 NORMAL       MQ2 118ppm  MQ7 4ppm  flame no  27.8°C  62%RH
```

---

## Driving a demo

The sketch auto-cycles NORMAL → SMOULDERING → FIRE every 30s so the dashboard shows
every state unattended. To drive it by hand, type into the Serial Monitor:

| Key | Effect |
|-----|--------|
| `n` | Normal — everything green |
| `s` | Smouldering — MQ-2 and MQ-7 into the amber band |
| `f` | Fire — all red, flame detected |
| `a` | Resume auto-cycling |
| `?` | Print Wi-Fi state, IP, and post counts |

The targets sit either side of the thresholds the bridge grades against (MQ-2 warn
400 / danger 800 ppm, MQ-7 warn 35 / danger 100 ppm, temp warn 45 / danger 60 °C),
so each scenario lands a predictable colour on the dashboard.

Values follow a **mean-reverting random walk** — noise plus a pull toward the
scenario's target — rather than being redrawn at random each tick, which would
flicker across the whole range and look nothing like a sensor.

---

## Wiring the real sensors

Set `MOCK_MODE 0` in `config.h` and fill in `readRealSensors()` in the sketch.

| Module | Pin | Notes |
|--------|-----|-------|
| MQ-2 AO | **GPIO 34** | ADC1, input-only |
| MQ-7 AO | **GPIO 35** | ADC1, input-only |
| IR flame DO | GPIO 27 | Digital; active-**LOW** on most 4-pin modules |
| IR flame AO | GPIO 32 | Optional — the DO alone is enough |
| DHT22 DATA | GPIO 4 | Digital 1-wire; 10 kΩ pull-up to 3.3 V |

> ⚠️ **ADC2 does not work while Wi-Fi is on.** That rules out GPIO 0/2/4/12–15/25–27
> for *any* `analogRead` — they return garbage the moment the radio starts. Both MQ
> analog outputs must sit on **ADC1 (GPIO 32–39)**. This is the single most common
> way an ESP32 sensor project goes quietly wrong. Digital pins are unaffected, which
> is why the flame DO and DHT22 are fine where they are.

> ⚠️ **The MQ modules are 5 V parts** and their AO swings above the ESP32's 3.3 V
> limit. Put a divider (e.g. 10 kΩ / 20 kΩ) on each AO, or you will damage the pin.

> The MQ sensors need a burn-in (24–48 h on first use) and a calibrated Rs/R₀ curve
> for real ppm figures. The linear scaling in `readRealSensors()` is a placeholder,
> not a calibration.

The DHT22 needs the Adafruit **DHT sensor library**; the sketch has the two lines
commented in place.

---

## Troubleshooting

| Symptom | Cause |
|---------|-------|
| `[wifi] FAILED` | The WROOM-32 is **2.4 GHz only**. If your router serves both bands under one name, split the SSIDs or point the board at the 2.4 GHz one. |
| `[post] FAILED code=-1` | Nothing answered. Check the laptop's IP is still `SERVER_URL`, `docker compose ps sensor-service`, and the macOS firewall on 8022. |
| `[post] FAILED code=401` | `DEVICE_KEY` does not match `SENSOR_INGEST_KEY` in the web app's `.env`. |
| `[post] FAILED code=422` | Malformed payload — normally a `DEVICE_ID` with characters outside `A-Za-z0-9_.:-`. |
| Posts succeed, card empty | You are looking at a dashboard served by a *different* host than the one the board posts to. |
| Card shows "Stale" | Posts stopped. The bridge greys a node after 15s without a sample. |

**The laptop's address is a DHCP lease.** Reserve it on your router, or `SERVER_URL`
stops being right the day it changes.

---

## Adding more nodes

Give each board its own `DEVICE_ID` and a `ZONE_ID` from the web app's zone catalog
(`alert-service/zones_seed.py`: `fabric-store`, `cutting-floor`, `dyeing`, `sewing-a`,
`warehouse`, `boiler`, `finishing`). Nothing needs configuring on the server — it
creates a node the first time one reports, up to `SENSOR_MAX_NODES`.
