/* ═══════════════════════════════════════════════════════════
   FireWatch — ESP32 sensor node (mock telemetry over Wi-Fi)

   Simulates the four modules in diagram.png and POSTs them as JSON
   to the web app's sensor-service:

     MQ-2    gas / smoke        → ppm
     MQ-7    carbon monoxide    → ppm
     IR flame (4-pin)           → detected / not
     DHT22   temperature+humidity

   DEPENDENCIES: none beyond the ESP32 board package. The JSON is
   built with snprintf and the mock values need no sensor library,
   so this compiles on a bare Arduino IDE install. (Wiring real
   sensors later does add a DHT library — see readRealSensors().)

   WHY PUSH, NOT SERVE. It would be less code to answer GET /sensors
   and let the server poll. But this board is on DHCP so its address
   moves, the diagram calls for a *network* of these, and a board
   running a server can be wedged by one half-open connection. Push
   fixes all three: only this file needs to know an address, nodes
   cost the server nothing to add, and there is no socket to wedge.

   WHY PLAIN HTTP. The dashboard is HTTPS behind a self-signed cert.
   Rather than teach the ESP32 to accept it, sensor-service is
   published on the host as plain HTTP for exactly this — the same
   arrangement alert-service already uses for the phone app.

   SERIAL (115200) — drive the demo by hand:
     n  normal     s  smouldering     f  fire     a  auto-cycle
     ?  print status
═══════════════════════════════════════════════════════════ */

#include <WiFi.h>
#include <HTTPClient.h>
#include "config.h"

/* ── Scenarios ───────────────────────────────────────────────
   Each is a set of targets the readings wander around, chosen to
   land either side of the bridge's thresholds (MQ-2 warn 400 /
   danger 800, MQ-7 warn 35 / danger 100, temp warn 45 / danger 60)
   so every dashboard colour is reachable from the serial monitor. */
enum Scenario { SCN_NORMAL = 0, SCN_SMOULDER = 1, SCN_FIRE = 2 };

struct Targets {
  const char* name;
  float mq2, mq7, temp, hum;
  bool  flame;
};

static const Targets SCENARIOS[] = {
  /* name           mq2    mq7   temp   hum   flame */
  { "NORMAL",      120.0f,  4.0f, 28.0f, 62.0f, false },
  { "SMOULDERING", 550.0f, 45.0f, 43.0f, 44.0f, false },
  { "FIRE",        950.0f, 140.0f, 68.0f, 27.0f, true  },
};
static const int SCENARIO_COUNT = sizeof(SCENARIOS) / sizeof(SCENARIOS[0]);

/* ── Live state ──────────────────────────────────────────────
   Readings are held as floats and nudged toward the scenario's
   targets each tick, so the dashboard shows a believable drift
   instead of values teleporting between extremes. */
static float mq2Ppm = 120.0f, mq7Ppm = 4.0f, tempC = 28.0f, humPct = 62.0f;
static bool  flameOn = false;

static Scenario scenario     = SCN_NORMAL;
static bool     autoScenario = AUTO_SCENARIO;
static uint32_t scenarioAt   = 0;
static uint32_t lastPostAt   = 0;
static uint32_t seq          = 0;
static uint32_t okCount = 0, failCount = 0;


/* ── Mock generation ─────────────────────────────────────────
   A mean-reverting random walk: each step is noise plus a pull
   toward the target. Pure random() would flicker across the whole
   range every tick and look nothing like a real sensor; a plain
   walk would drift off and never come back. */
static float drift(float current, float target, float noise, float lo, float hi) {
  float jitter = ((float)random(-1000, 1001) / 1000.0f) * noise;
  float pull   = (target - current) * 0.18f;
  float next   = current + pull + jitter;
  if (next < lo) next = lo;
  if (next > hi) next = hi;
  return next;
}

static void stepMockSensors() {
  const Targets& t = SCENARIOS[scenario];

  mq2Ppm = drift(mq2Ppm, t.mq2,  18.0f,   0.0f, 2000.0f);
  mq7Ppm = drift(mq7Ppm, t.mq7,   3.0f,   0.0f,  500.0f);
  tempC  = drift(tempC,  t.temp,  0.6f, -10.0f,  125.0f);
  humPct = drift(humPct, t.hum,   1.2f,   0.0f,  100.0f);

  /* The IR module is a comparator, not a curve — it latches on once
     the flame is in view. Flicker it slightly so the dashboard's
     binary tile is exercised rather than sitting on one value. */
  flameOn = t.flame ? (random(0, 100) > 8) : (random(0, 1000) > 997);
}

/* Raw ADC is derived from the ppm rather than generated separately,
   so the two fields stay consistent — a raw value that disagreed
   with its ppm would be a confusing thing to debug later. */
static int mockRaw(float ppm, float fullScale) {
  float frac = ppm / fullScale;
  if (frac > 1.0f) frac = 1.0f;
  return (int)(frac * 4095.0f);
}


/* ── Real sensors (MOCK_MODE 0) ──────────────────────────────
   Left as the seam for when the modules are actually wired. The
   pin map and its ADC1 / voltage-divider warnings are in config.h.

   You will need a DHT library for the DHT22 (Adafruit "DHT sensor
   library"), and real MQ readings need a calibrated Rs/R0 curve —
   the linear scaling below is a placeholder, not a calibration. */
static void readRealSensors() {
#if !MOCK_MODE
  int mq2Raw = analogRead(PIN_MQ2_AO);
  int mq7Raw = analogRead(PIN_MQ7_AO);
  mq2Ppm = (mq2Raw / 4095.0f) * 2000.0f;
  mq7Ppm = (mq7Raw / 4095.0f) * 500.0f;

  // 4-pin IR flame modules pull DO LOW when they see a flame.
  flameOn = (digitalRead(PIN_FLAME_DO) == LOW);

  // #include <DHT.h>; DHT dht(PIN_DHT22, DHT22); dht.begin() in setup();
  // tempC = dht.readTemperature(); humPct = dht.readHumidity();
#endif
}


/* ── Wi-Fi ───────────────────────────────────────────────────── */

static void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("[wifi] connecting to \"%s\" ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Bounded so a wrong password or a 5 GHz-only SSID reports itself
  // instead of hanging the sketch forever with no output.
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    delay(400);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] connected — board IP %s, RSSI %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    Serial.printf("[wifi] posting to %s\n", SERVER_URL);
  } else {
    Serial.println("[wifi] FAILED — check the SSID/password, and that it is 2.4 GHz.");
  }
}


/* ── Posting ─────────────────────────────────────────────────── */

static void postReadings() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
    if (WiFi.status() != WL_CONNECTED) return;
  }

  char payload[512];
  snprintf(payload, sizeof(payload),
    "{\"deviceId\":\"%s\",\"zoneId\":\"%s\",\"seq\":%lu,\"uptimeMs\":%lu,"
    "\"rssi\":%d,\"mock\":%s,\"readings\":{"
    "\"mq2_ppm\":%.1f,\"mq2_raw\":%d,"
    "\"mq7_ppm\":%.1f,\"mq7_raw\":%d,"
    "\"flame\":%d,\"flame_raw\":%d,"
    "\"temperature_c\":%.1f,\"humidity_pct\":%.1f}}",
    DEVICE_ID, ZONE_ID, (unsigned long)seq, (unsigned long)millis(),
    WiFi.RSSI(), MOCK_MODE ? "true" : "false",
    mq2Ppm, mockRaw(mq2Ppm, 2000.0f),
    mq7Ppm, mockRaw(mq7Ppm, 500.0f),
    flameOn ? 1 : 0, flameOn ? 400 : 3900,
    tempC, humPct);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(SERVER_URL)) {
    Serial.println("[post] bad SERVER_URL");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  if (strlen(DEVICE_KEY) > 0) http.addHeader("X-Device-Key", DEVICE_KEY);

  int code = http.POST((uint8_t*)payload, strlen(payload));

  if (code == 200) {
    okCount++;
    Serial.printf("[post] #%lu %-11s  MQ2 %.0fppm  MQ7 %.0fppm  flame %s  %.1f°C  %.0f%%RH\n",
                  (unsigned long)seq, SCENARIOS[scenario].name,
                  mq2Ppm, mq7Ppm, flameOn ? "YES" : "no", tempC, humPct);
  } else {
    failCount++;
    // A negative code is a client-side failure (no route, refused,
    // timed out); a positive one is the server saying no.
    Serial.printf("[post] #%lu FAILED code=%d %s\n",
                  (unsigned long)seq, code,
                  code < 0 ? HTTPClient::errorToString(code).c_str()
                           : http.getString().c_str());
    if (code < 0) {
      Serial.println("       → is the laptop at SERVER_URL, on this network,");
      Serial.println("         with sensor-service up and the firewall allowing 8022?");
    }
  }
  http.end();
  seq++;
}


/* ── Serial control ──────────────────────────────────────────── */

static void setScenario(Scenario s, bool fromSerial) {
  scenario   = s;
  scenarioAt = millis();
  if (fromSerial) autoScenario = false;
  Serial.printf("[scn] → %s%s\n", SCENARIOS[s].name, autoScenario ? " (auto)" : "");
}

static void handleSerial() {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'n': setScenario(SCN_NORMAL, true);   break;
      case 's': setScenario(SCN_SMOULDER, true); break;
      case 'f': setScenario(SCN_FIRE, true);     break;
      case 'a':
        autoScenario = true;
        scenarioAt   = millis();
        Serial.println("[scn] auto-cycle on");
        break;
      case '?':
        Serial.printf("[status] %s | wifi %s | ip %s | posts ok=%lu fail=%lu\n",
                      SCENARIOS[scenario].name,
                      WiFi.status() == WL_CONNECTED ? "up" : "down",
                      WiFi.localIP().toString().c_str(),
                      (unsigned long)okCount, (unsigned long)failCount);
        break;
      default: break;  // ignore newlines and stray keys
    }
  }
}


/* ── Arduino entry points ────────────────────────────────────── */

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);

  Serial.println();
  Serial.println("═══ FireWatch sensor node ═══");
  Serial.printf("  device : %s  (zone %s)\n", DEVICE_ID, ZONE_ID);
  Serial.printf("  mode   : %s\n", MOCK_MODE ? "MOCK — no sensors required" : "REAL sensors");
  Serial.printf("  target : %s every %dms\n", SERVER_URL, POST_INTERVAL_MS);
  Serial.println("  keys   : n normal · s smouldering · f fire · a auto · ? status");
  Serial.println();

#if !MOCK_MODE
  pinMode(PIN_FLAME_DO, INPUT);
  analogReadResolution(12);
#endif

  // Seed from a floating ADC pin so two boards flashed identically do not
  // produce the same "random" walk.
  randomSeed(analogRead(PIN_FLAME_AO) ^ micros());

  connectWifi();
  scenarioAt = millis();
  lastPostAt = millis() - POST_INTERVAL_MS;   // post immediately on boot
}

void loop() {
  handleSerial();

  if (autoScenario && millis() - scenarioAt >= SCENARIO_HOLD_MS) {
    setScenario((Scenario)((scenario + 1) % SCENARIO_COUNT), false);
  }

  if (millis() - lastPostAt >= POST_INTERVAL_MS) {
    lastPostAt = millis();
#if MOCK_MODE
    stepMockSensors();
#else
    readRealSensors();
#endif
    postReadings();
  }

  delay(10);   // keep the Wi-Fi stack's housekeeping fed
}
