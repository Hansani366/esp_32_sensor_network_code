# FireWatch — ESP32 sensor nodes

![FireWatch: ESP32 sensor nodes, described as multi-sensor fire detection firmware. An ESP32-WROOM-32 board stands at the centre, lit against a forest at dusk. Four panels on the left show the modules wired to it, each with its own trend line: an MQ-2 for smoke and combustible gas, an MQ-7 for carbon monoxide, an IR flame sensor, and a DHT22 for temperature and humidity. Coloured wires run from each panel to the board, which reports over Wi-Fi to a laptop on the right. The FireWatch dashboard there shows one tile per channel and an overall fire risk panel below them.](docs/esp_32_sensor_cover_image.png)

## 1. Overview

This repository holds the firmware for the sensor units in `diagram.png`. Each node
is an **ESP32-WROOM-32** carrying four parts: an **MQ-2** for combustible gas and
smoke, an **MQ-7** for carbon monoxide, an **IR flame module**, and a **DHT22** for
temperature and humidity. A node reports its readings over Wi-Fi to the FireWatch
dashboard.

**It ships in mock mode.** The readings are generated in software, so a bare board
with nothing wired to it already drives the dashboard from end to end. You wire the
real modules later and change one setting.

```
ESP32 node ──HTTP POST──▶ sensor-service :8022 ──▶ nginx ──HTTPS──▶ Dashboard
  (readings every 3s)        (on your computer)             (Sensor Network card)
```

**What makes it different:**

- **Four channels that cover each other.** Each channel is strong where the others
  are weak, as Table 1 sets out. One sensor alone would either raise false alarms or
  report a fire too late.
- **A demo that needs no hardware.** The sketch cycles through normal, smouldering
  and fire on its own, so the dashboard shows every state while nobody touches the
  board.
- **Readings that behave like sensors.** The values drift towards a target instead
  of being redrawn at random, so the dashboard is tested against something a real
  sensor could produce.
- **A node that reports itself.** Nothing is configured on the server. It creates a
  node the first time one reports.
- **Silence is visible.** The server marks a node stale after 15 seconds without a
  sample, so a node that has stopped never reads as a quiet room.

**Two hardware limits.** The ESP32-WROOM-32 uses **2.4 GHz Wi-Fi only**, so a router
that gives one name to both bands may stop the board connecting. The node also posts
in plain HTTP on port 8022, not on port 443, because nginx there uses a self-signed
certificate that a small board should not have to handle.

Table 1 shows why four channels are used together.

**Table 1.** The four sensor channels, what each reacts to, and its weakness.

| Channel | Reacts | Weakness |
|---|---|---|
| MQ-2, gas and smoke | Early, before a flame is visible | Also reacts to cooking, sprays, solvents and exhaust |
| MQ-7, carbon monoxide | To incomplete burning | Slower, and the sensor must be warm |
| DHT22, temperature | Reliably | Late, because the fire must already be large |
| IR flame | Immediately, in line of sight | Needs a clear view, and sunlight can fool it |

## 2. Main features

- **Mock mode and real mode.** `MOCK_MODE 1` generates readings in software.
  `MOCK_MODE 0` reads the real modules through `readRealSensors()`.
- **A scenario engine.** The sketch moves through normal, smouldering and fire, and
  changes state every 30 seconds on its own. You can also drive it by hand from the
  Serial Monitor, as Table 2 sets out.
- **Targets set around the grading limits.** The scenario values sit on either side
  of the limits the bridge grades against: MQ-2 warn 400 and danger 800 ppm, MQ-7
  warn 35 and danger 100 ppm, temperature warn 45 and danger 60 °C. So each state
  lands on a colour you can predict before the test runs.
- **Readings that move like sensors.** The values follow a mean-reverting random
  walk, which is noise plus a slow pull towards the target. Fresh random numbers
  each tick would jump across the whole range and would look nothing like a sensor.
- **Node identity.** `DEVICE_ID` names the board and `ZONE_ID` says which room it is
  in. One node belongs to one zone, and every extra board needs its own pair.
- **A key on every post.** When `DEVICE_KEY` is set, the node sends it as the
  `X-Device-Key` header, so an open port on a shared network still refuses unknown
  boards.

**Driving a demo by hand** uses the keys in Table 2.

**Table 2.** Serial Monitor keys and what each one does.

| Key | Effect |
|---|---|
| `n` | Normal — everything green (mock mode) |
| `s` | Smouldering — MQ-2 and MQ-7 move into the amber band (mock mode) |
| `f` | Fire — all red, flame detected (mock mode) |
| `a` | Return to automatic cycling (mock mode) |
| `c` | Calibrate the MQ R0 values in clean air (real mode) |
| `q` | Silence the buzzer until the readings return to normal |
| `?` | Print the alarm level, Wi-Fi state, IP address, post counts and, in real mode, the MQ node voltages |

## 3. Technologies

Table 3 lists the technologies used in the firmware.

**Table 3.** Technologies used in the implementation.

| Area | Technology |
|---|---|
| Build and upload | Arduino core for ESP32 |
| Network | `WiFi.h`, in station mode |
| Posting | `HTTPClient`, sending JSON |
| JSON | Built as text with `snprintf`, so no JSON library is needed |
| Real DHT22 | Adafruit **DHT sensor library** and **Adafruit Unified Sensor**, used when `MOCK_MODE` is `0` |
| Board | ESP32-WROOM-32, Serial Monitor at 115200 baud |

No libraries are needed in mock mode. In real mode install the two Adafruit libraries
above from the Library Manager. The repository is laid out as follows:

```
sensor_node_mock/
├── sensor_node_mock.ino   # the sketch: mock readings, Wi-Fi, POST loop
├── config.example.h       # the template, committed
└── config.h               # your settings; GIT IGNORES IT, holds the Wi-Fi password
tools/
└── mock_sender.py         # simulate nodes from your computer, with no board
```

## 4. Setup and usage

**Requirements:**

- The Arduino IDE, with the **esp32** boards package by Espressif Systems.
- An ESP32-WROOM-32 board. No sensors are needed to start.
- The web app from the FireWatch project, running on the same network.
- Python 3, to run `tools/mock_sender.py`.
- For real mode: the MQ-2, MQ-7, IR flame and DHT22 modules, three LEDs with 220 Ω
  resistors, a small 3 V active buzzer and a push button, wired as in the
  "Sensor Node Breadboard Layout, version 3" sheet.

**Configure first.** Git ignores `config.h`, because it holds your Wi-Fi password, so
a new clone does not have it. The sketch still compiles, because it falls back to
`config.example.h`, but it cannot join your network until you supply real values:

```bash
cp sensor_node_mock/config.example.h sensor_node_mock/config.h
# then edit config.h — the sketch prefers it over the template
```

Table 4 lists every setting.

**Table 4.** Configuration settings and their defaults. All are set in `config.h`.

| Setting | Default | Meaning |
|---|---|---|
| `WIFI_SSID` | none | Your network name. It must be the **2.4 GHz** one. |
| `WIFI_PASSWORD` | none | The password for that network. |
| `SERVER_URL` | `http://192.168.1.118:8022/api/sensors/ingest` | The address of the computer running `docker compose`. |
| `DEVICE_KEY` | blank | Must match `SENSOR_INGEST_KEY` in the web app's `.env`. Blank on both sides accepts any board that can reach the port. |
| `DEVICE_ID` | `node-01` | A unique name for this board. |
| `ZONE_ID` | `kitchen` | Which room the board is in. It must belong to the active site. |
| `POST_INTERVAL_MS` | `3000` | Milliseconds between posts. Keep it well under the 15 second stale limit. |
| `HTTP_TIMEOUT_MS` | `5000` | How long to wait for the server to answer. |
| `MOCK_MODE` | `0` | `1` generates readings, `0` reads the real modules. |
| `AUTO_SCENARIO` | `1` | `1` cycles the states, `0` waits for the keys in Table 2. Mock mode only. |
| `SCENARIO_HOLD_MS` | `30000` | How long each state lasts while cycling. Mock mode only. |
| `MQ_DIVIDER_GAIN` | `1.667` | Undoes the 10 kΩ / 15 kΩ divider on each MQ output. |
| `MQ2_RL_KOHM`, `MQ7_RL_KOHM` | `5`, `10` | Load resistor fitted on each MQ breakout board. |
| `MQ2_R0_KOHM`, `MQ7_R0_KOHM` | `0` | Clean-air resistance from the `c` key. `0` means assume clean air at first read. |
| `MQ_WARMUP_MS` | `60000` | Gas ppm is reported as `0` and the amber LED blinks until this has passed. |
| `FLAME_DO_ACTIVE_LOW` | `1` | `1` if the flame module pulls DO **low** on a flame, `0` if it drives it **high**. Vendors ship both. Wrong here and every flame reads backwards. |
| `BUZZER_SILENT_DURING_WARMUP` | `1` | Holds the buzzer off until the heaters settle. The LEDs and the dashboard still alarm. |
| `THRESH_*` | as the bridge | Levels that drive the LEDs and buzzer locally. |
| `BUZZER_BEEP_MS`, `BUZZER_PERIOD_MS` | `120`, `1000` | Beep pattern in the danger state. |

**Finding the value for `SERVER_URL`.** It is the address of the computer that runs
`docker compose`, on port 8022. Do not use `localhost`, because that would mean the
board itself:

```bash
# macOS
ipconfig getifaddr en0

# Windows: read the IPv4 Address of your Wi-Fi adapter
ipconfig
```

This address comes from DHCP, so **reserve it on your router**. If you do not, the
setting stops being correct on the day the address changes.

**Matching `ZONE_ID` to the site.** The web app runs either as a house or as an
industrial unit, and `SITE_KEY` in its `.env` chooses which. Each site has its own
zones, listed in Table 5, and `ZONE_ID` must be one of them.

**Table 5.** Valid zone ids for each site.

| `SITE_KEY` | Valid zone ids |
|---|---|
| `home` | `kitchen`, `dining`, `living`, `verander`, `bedroom-west`, `bedroom-southwest`, `bedroom-northeast`, `bedroom-southeast` |
| `industrial` | `fabric-store`, `cutting-floor`, `dyeing`, `sewing-a`, `warehouse`, `boiler`, `finishing` |

Check this setting carefully. The server accepts any zone name, so a wrong one gives
**no error message**. The node appears on the dashboard and looks healthy, but it
never matches a room on the floor plan. The template ships with `kitchen`, which
belongs to the default `home` site, so change it when you run the demo hall.

**Proving the pipeline before you touch hardware:**

```bash
cd ../fire_detection_and_classification_web_app
docker compose up --build -d sensor-service
cd ../esp_32_sensor_network_code
python3 tools/mock_sender.py
```

Open the dashboard, and the **Sensor Network** card should start to fill in. Do this
first. If the board fails later, you already know the server side works, so only the
board is left to check.

```bash
python3 tools/mock_sender.py --nodes 4 --scenario fire   # a whole network, alarming
python3 tools/mock_sender.py --help                      # all options
```

**Flashing the board:**

1. Arduino IDE → **Boards Manager** → install **esp32** (Espressif Systems).
2. Choose **ESP32 Dev Module** and the port of your board.
3. Upload, then open the Serial Monitor at **115200**.

The expected output is:

```
═══ FireWatch sensor node ═══
  device : node-01  (zone kitchen)
  mode   : REAL sensors (breadboard layout v3)
  target : http://192.168.1.118:8022/api/sensors/ingest every 3000ms
  keys   : c calibrate R0 · q silence · ? status

[mq] heaters warming up for 60 s, gas reported as 0 until then
[wifi] connecting to "your-ssid" .....
[wifi] connected — board IP 192.168.1.203, RSSI -54 dBm
[post] #0 normal       MQ2 0ppm  MQ7 0ppm  flame no  27.8°C  62%RH
```

With `MOCK_MODE 1` the mode line reads `MOCK — no sensors required` and the scenario
keys `n s f a` are listed instead.

**Verifying.** The node should appear on the dashboard's **Sensor Network** card
within a few seconds. Nothing needs to be set up on the server, because it creates a
node the first time one reports, up to `SENSOR_MAX_NODES`. If the card says
**Stale**, the posts have stopped.

**Wiring the real sensors.** The firmware matches the "Sensor Node Breadboard Layout,
version 3" sheet. Set `MOCK_MODE 0` in `config.h` (the template default) and install
the two Adafruit libraries from Table 3. Table 6 gives the pin map.

**Table 6.** Pin map, as wired on the breadboard.

| Part | Pin | Notes |
|---|---|---|
| MQ-2 AO | **GPIO 34** | ADC1, input only. Through a 10 kΩ / 15 kΩ divider, node E24 |
| MQ-7 AO | **GPIO 35** | ADC1, input only. Through a 10 kΩ / 15 kΩ divider, node E34 |
| IR flame AO | GPIO 32 | ADC1. Module runs from 3.3 V, so no divider |
| IR flame DO | GPIO 27 | Digital. **LOW** when a flame is seen |
| DHT22 DATA | GPIO 4 | Digital, one wire. Module runs from 3.3 V |
| Green LED | GPIO 25 | Normal. 220 Ω in series |
| Amber LED | GPIO 26 | Warning, and blinks during MQ warm-up. 220 Ω in series |
| Red LED | GPIO 33 | Danger. 220 Ω in series |
| Buzzer | GPIO 13 | Small 3 V active buzzer, driven directly. Short beeps only |
| Button | GPIO 14 | `INPUT_PULLUP`, pressed = LOW. Silences the buzzer |

**Local alarm.** The sketch applies the same thresholds as the bridge (MQ-2 400 / 800
ppm, MQ-7 35 / 100 ppm, temperature 45 / 60 °C, any flame = danger). Green means
normal, amber warning, red danger. In danger the buzzer beeps once a second until the
button is pressed or `q` is typed; it re-arms once readings return to normal.

**Calibrating the MQ sensors.** New MQ-2 and MQ-7 parts need 24 to 48 hours of burn-in.
After that, with the room well ventilated, type `c` in the Serial Monitor. The sketch
prints `MQ2_R0_KOHM` and `MQ7_R0_KOHM` values to paste into `config.h`. Until you do,
it assumes the first warmed-up reading is clean air, which is good enough for a demo
but not for real ppm figures. Check `MQ2_RL_KOHM` and `MQ7_RL_KOHM` against the load
resistor on your breakout boards.

> ⚠️ **ADC2 does not work while Wi-Fi is on.** This rules out GPIO 0, 2, 4, 12–15
> and 25–27 for *any* `analogRead`, because they return meaningless values as soon as
> the radio starts. All three analogue inputs therefore sit on **ADC1, GPIO 32–35**.
> Digital pins are not affected, which is why the flame DO, the DHT22, the LEDs, the
> buzzer and the button are fine where they are.

> ⚠️ **The MQ modules are 5 V parts** and their analogue output rises above the ESP32's
> 3.3 V limit. The board uses a 10 kΩ / 15 kΩ divider on each, and the firmware
> multiplies the measured voltage by 1.667 (`MQ_DIVIDER_GAIN`) before the Rs formula.
> Before connecting the ESP32, measure E24 and E34 with the sensors powered: they must
> read **3.0 V or less**. The `?` key prints the live node voltages as a second check.

> The buzzer is driven straight from GPIO 13 because no transistor was fitted. This is
> fine for a small 3 V active buzzer but close to the pin's limit. If the board resets
> when it beeps, add a 2N2222 and a 1 kΩ base resistor.

**Adding more nodes.** Give every board its own `DEVICE_ID` and its own `ZONE_ID`
from Table 5. Nothing changes on the server.

**Troubleshooting.** Table 7 lists the common failures and their causes.

**Table 7.** Common problems and what causes them.

| Symptom | Cause |
|---|---|
| `[wifi] FAILED` | The WROOM-32 is **2.4 GHz only**. Split the network names, or point the board at the 2.4 GHz one. |
| `[post] FAILED code=-1` | Nothing answered. Check that `SERVER_URL` still matches the computer's address, run `docker compose ps sensor-service`, and check the firewall on port 8022. |
| `[post] FAILED code=401` | `DEVICE_KEY` does not match `SENSOR_INGEST_KEY` in the web app's `.env`. |
| `[post] FAILED code=422` | The payload is malformed, usually a `DEVICE_ID` with characters outside `A-Za-z0-9_.:-`. |
| Posts succeed, but the card is empty | The dashboard you are watching is served by a different computer from the one the board posts to. |
| The card shows **Stale** | The posts stopped. The bridge greys a node after 15 seconds without a sample. |
| The node shows a room that does not exist | `ZONE_ID` belongs to the other site. See Table 5. |
| Flame reads **backwards** — `flame YES` in a cold room, `no` at a lighter | Your module drives DO the opposite way. Set `FLAME_DO_ACTIVE_LOW 0`. Press `?` with and without a flame to see the raw pin. |
| Temperature stuck at exactly **28.0 °C** and humidity at **62%** | Those are the start-up placeholders, so the DHT22 has never answered. Press `?`: it says `NEVER READ`. Check 3.3 V, GND, DATA on GPIO 4 and the 4.7–10 kΩ pull-up. |
| Buzzer sounds with no fire | A floating flame pin, or the polarity above. The firmware pulls the pin to the idle level, so suspect the module's 3.3 V supply first. |
| It worked yesterday but not today | The computer's address comes from DHCP. Reserve it on the router. |

## 5. Scope and design decisions

Each point is a decision taken for a stated reason, with what it means for the
result.

- **Mock mode is the default, not an extra.** Gas sensors need a long burn-in and a
  calibration curve before their numbers mean anything, and a fire cannot be lit
  safely to test them. Generated readings let the whole path be built and tested
  first: the board, the network, the grading and the dashboard. When the modules
  arrive, only one layer is new, so a fault has only one place to hide.
- **The readings move like sensors, not like random numbers.** A mean-reverting
  random walk produces values that drift and settle, while fresh random numbers
  would jump across the full range and would test the dashboard against something no
  sensor ever does.
- **Four channels are used together on purpose.** Each covers the weakness of the
  others, as Table 1 sets out. Gas reacts early but also reacts to cooking, the flame
  sensor needs a clear view, and temperature is reliable but late.
- **The scenario targets are set around the grading limits.** Each state lands on a
  colour that can be predicted before the test runs, which makes a demonstration
  repeatable and makes a wrong threshold easy to see.
- **`ZONE_ID` is fixed when the board is flashed.** A node is mounted in one room and
  stays there, so the zone belongs to the installation rather than to something
  discovered at start-up. This keeps the firmware small and removes a whole class of
  start-up failure. Moving a board therefore means flashing it again, and Table 5
  states the valid values.
- **The server accepts any zone name.** The sensor bridge stays separate from the
  zone catalogue, so the two can be developed independently. This is why Table 5
  gives the valid names and warns that a wrong one is silent. Checking the name at
  the server is the next step, and it is a small, contained change.
- **The ingest port is plain HTTP.** TLS on a small board costs memory, and it would
  also make the board trust a self-signed certificate. Port 8022 is therefore
  published only on the local network, and `DEVICE_KEY` is available when that
  network is shared.
- **The credentials are in a file that Git ignores.** `config.example.h` is committed
  and documents every setting, while `config.h` holds the real values. The sketch
  falls back to the template, so a new clone still compiles, and the repository stays
  safe to publish.
- **A node sends and then forgets.** There is no queue for readings that fail to
  send. A fire alarm needs the current state of a room, not a record of the past
  minute, and a queue would deliver old readings that describe a room as it no longer
  is. The server marks a silent node stale after 15 seconds, so a connection problem
  appears on the dashboard instead of being hidden.
