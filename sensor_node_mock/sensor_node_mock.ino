/* ═══════════════════════════════════════════════════════════
   FireWatch — ESP32 sensor node (mock telemetry over Wi-Fi)

   Reads (or, in MOCK_MODE, simulates) the four modules wired as in
   the "Sensor Node Breadboard Layout, version 3" sheet and POSTs them as
   JSON to the web app's sensor-service:

     MQ-2    gas / smoke        → ppm   (GPIO 34, via 10k/15k divider)
     MQ-7    carbon monoxide    → ppm   (GPIO 35, via 10k/15k divider)
     IR flame (4-pin)           → detected / not  (DO GPIO 27, AO GPIO 32)
     DHT22   temperature+humidity       (GPIO 4)

   The node also has a local alarm: green / amber / red LEDs on
   GPIO 25 / 26 / 33, a buzzer on GPIO 13 and a silence button on
   GPIO 14. These follow the same thresholds the dashboard uses.

   DEPENDENCIES: in MOCK_MODE none beyond the ESP32 board package.
   With real sensors (MOCK_MODE 0) install the Adafruit "DHT sensor
   library" and its "Adafruit Unified Sensor" dependency from the
   Library Manager.

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

   SERIAL (115200):
     n  normal     s  smouldering     f  fire     a  auto-cycle  (mock only)
     c  calibrate R0 in clean air (real mode)     q  silence buzzer
     ?  print status
═══════════════════════════════════════════════════════════ */

#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>

/* config.h holds the Wi-Fi password, so it is gitignored and absent from a
   fresh clone. Falling back to the committed template means the sketch still
   compiles and runs there — it simply cannot join a network until someone
   fills in real credentials, which the serial log says in as many words. That
   is a far better first experience than "config.h: No such file or directory"
   before the reader has done anything wrong. */
#if __has_include("config.h")
  #include "config.h"
#else
  #include "config.example.h"
  #warning "No config.h — using config.example.h. Copy it and fill in your Wi-Fi details."
#endif

#if !MOCK_MODE
  #include <DHT.h>
  static DHT dht(PIN_DHT22, DHT22);
#endif

/* config.h is gitignored, so a config.h written before these settings
   existed is still a valid one. Default them here rather than fail to
   compile on someone else's copy. */
#ifndef FLAME_DO_ACTIVE_LOW
  #define FLAME_DO_ACTIVE_LOW 1
#endif
#ifndef BUZZER_SILENT_DURING_WARMUP
  #define BUZZER_SILENT_DURING_WARMUP 1
#endif

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
/* Those two are start-up placeholders. A DHT22 that never answers leaves
   them in place, and 28.0 C / 62 %RH reads like a real room on the
   dashboard — the failure that looks most like success. */
static bool dhtEverRead = false;
static bool  flameOn = false;

static Scenario scenario     = SCN_NORMAL;
static bool     autoScenario = AUTO_SCENARIO;
static uint32_t scenarioAt   = 0;
static uint32_t lastPostAt   = 0;
static uint32_t seq          = 0;
static uint32_t okCount = 0, failCount = 0;

/* Raw ADC counts from the last real read, so the payload reports what
   the pins actually saw rather than a value derived from the ppm. */
static int flameRaw = 3900, mq2Raw = 0, mq7Raw = 0;

/* Alarm level derived from the latest readings. */
enum Level { LVL_NORMAL = 0, LVL_WARN = 1, LVL_DANGER = 2 };
static Level    level        = LVL_NORMAL;
static bool     silenced     = false;   // button pressed while alarming
static uint32_t bootAt       = 0;

/* Clean-air resistance of each MQ sensor, in kΩ. Filled at boot from
   config.h or from the datasheet clean-air ratio on the first read. */
static float mq2R0 = MQ2_R0_KOHM, mq7R0 = MQ7_R0_KOHM;


#if MOCK_MODE
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
#endif  // MOCK_MODE


/* ── Real sensors (MOCK_MODE 0) ──────────────────────────────
   Pins and constants come from config.h. Every analogue input is on
   ADC1 so it keeps working with Wi-Fi up.

   The MQ analogue outputs reach the ESP32 through a 10 kΩ / 15 kΩ
   divider, so the measured voltage is multiplied by MQ_DIVIDER_GAIN
   (1.667) before the Rs formula:

       Rs  = RL × (Vc − Vout) / Vout
       ppm = A × (Rs / R0) ^ B

   analogReadMilliVolts() uses the chip's factory ADC calibration,
   which is noticeably better than a straight 4095 → 3.3 V scale. */
#if !MOCK_MODE

/* Average a few samples; without the 100 nF decoupling parts the MQ
   outputs carry a little heater noise and this removes it. */
static uint32_t readMilliVoltsAvg(int pin, int n = 16) {
  uint32_t sum = 0;
  for (int i = 0; i < n; i++) sum += analogReadMilliVolts(pin);
  return sum / n;
}

static int readRawAvg(int pin, int n = 16) {
  uint32_t sum = 0;
  for (int i = 0; i < n; i++) sum += analogRead(pin);
  return (int)(sum / n);
}

/* Sensor-side output voltage, with the divider undone. */
static float mqVoltage(int pin) {
  float vNode = readMilliVoltsAvg(pin) / 1000.0f;
  float v = vNode * MQ_DIVIDER_GAIN;
  if (v < 0.05f) v = 0.05f;                 // avoid divide-by-zero below
  if (v > MQ_SUPPLY_V - 0.05f) v = MQ_SUPPLY_V - 0.05f;
  return v;
}

static float mqRs(float vOut, float rlKohm) {
  return rlKohm * (MQ_SUPPLY_V - vOut) / vOut;
}

static float mqPpm(float rs, float r0, float a, float b) {
  if (r0 <= 0.0f) return 0.0f;
  float ppm = a * powf(rs / r0, b);
  if (ppm < 0.0f) ppm = 0.0f;
  if (ppm > 10000.0f) ppm = 10000.0f;
  return ppm;
}

static bool mqWarmedUp() {
  return millis() - bootAt >= MQ_WARMUP_MS;
}

/* Press 'c' in clean air after burn-in. Prints the R0 values to paste
   into config.h and uses them immediately. */
static void calibrateR0() {
  if (!mqWarmedUp()) {
    Serial.println("[cal] heaters still warming up, try again later");
    return;
  }
  float rs2 = mqRs(mqVoltage(PIN_MQ2_AO), MQ2_RL_KOHM);
  float rs7 = mqRs(mqVoltage(PIN_MQ7_AO), MQ7_RL_KOHM);
  mq2R0 = rs2 / MQ2_CLEAN_AIR_RATIO;
  mq7R0 = rs7 / MQ7_CLEAN_AIR_RATIO;
  Serial.printf("[cal] clean air: MQ2 Rs=%.2fk → R0=%.2fk   MQ7 Rs=%.2fk → R0=%.2fk\n",
                rs2, mq2R0, rs7, mq7R0);
  Serial.printf("[cal] put in config.h:  #define MQ2_R0_KOHM %.2ff   #define MQ7_R0_KOHM %.2ff\n",
                mq2R0, mq7R0);
}

static void readRealSensors() {
  /* MQ-2 and MQ-7 */
  float v2 = mqVoltage(PIN_MQ2_AO);
  float v7 = mqVoltage(PIN_MQ7_AO);
  mq2Raw = readRawAvg(PIN_MQ2_AO, 4);
  mq7Raw = readRawAvg(PIN_MQ7_AO, 4);

  float rs2 = mqRs(v2, MQ2_RL_KOHM);
  float rs7 = mqRs(v7, MQ7_RL_KOHM);

  if (mqWarmedUp()) {
    /* No R0 in config.h: assume the first warmed-up read is clean air. */
    if (mq2R0 <= 0.0f) { mq2R0 = rs2 / MQ2_CLEAN_AIR_RATIO; Serial.printf("[cal] MQ2 R0 assumed %.2fk\n", mq2R0); }
    if (mq7R0 <= 0.0f) { mq7R0 = rs7 / MQ7_CLEAN_AIR_RATIO; Serial.printf("[cal] MQ7 R0 assumed %.2fk\n", mq7R0); }
    mq2Ppm = mqPpm(rs2, mq2R0, MQ2_CURVE_A, MQ2_CURVE_B);
    mq7Ppm = mqPpm(rs7, mq7R0, MQ7_CURVE_A, MQ7_CURVE_B);
  } else {
    mq2Ppm = 0.0f;
    mq7Ppm = 0.0f;
  }

  /* 4-pin IR flame module. Vendors ship both DO polarities and the
     board gives no clue which you have, so FLAME_DO_ACTIVE_LOW picks.
     A module wired the other way round reads every flame backwards:
     alarm when the room is cold, silence when it is burning.
     AO falls as the flame gets stronger, so it is only reported,
     never judged — the comparator on the module already decided. */
#if FLAME_DO_ACTIVE_LOW
  flameOn  = (digitalRead(PIN_FLAME_DO) == LOW);
#else
  flameOn  = (digitalRead(PIN_FLAME_DO) == HIGH);
#endif
  flameRaw = readRawAvg(PIN_FLAME_AO, 4);

  /* DHT22. A failed read returns NaN; keep the previous value so a
     single missed read does not push a 0 °C to the dashboard. */
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) { tempC = t; dhtEverRead = true; }
  else Serial.println("[dht] read failed, keeping last value");
  if (!isnan(h)) humPct = h;
}
#endif


/* ── Local alarm: LEDs, buzzer, button ───────────────────────── */

static Level computeLevel() {
  bool danger = flameOn
             || mq2Ppm >= THRESH_MQ2_DANGER
             || mq7Ppm >= THRESH_MQ7_DANGER
             || tempC  >= THRESH_TEMP_DANGER;
  if (danger) return LVL_DANGER;
  bool warn = mq2Ppm >= THRESH_MQ2_WARN
           || mq7Ppm >= THRESH_MQ7_WARN
           || tempC  >= THRESH_TEMP_WARN;
  return warn ? LVL_WARN : LVL_NORMAL;
}

static void updateLevel() {
  Level next = computeLevel();
  if (next != level) {
    Serial.printf("[alarm] %s → %s\n",
                  level == LVL_DANGER ? "DANGER" : level == LVL_WARN ? "WARN" : "normal",
                  next  == LVL_DANGER ? "DANGER" : next  == LVL_WARN ? "WARN" : "normal");
    level = next;
    if (level == LVL_NORMAL) silenced = false;   // re-arm once things are quiet
  }
}

static void silenceAlarm(const char* who) {
  if (level == LVL_NORMAL) return;
  if (!silenced) Serial.printf("[alarm] silenced (%s)\n", who);
  silenced = true;
}

/* Called every loop. Drives the LEDs from the current level and
   beeps the buzzer in DANGER until the button or 'q' silences it. */
static void driveIndicators() {
  uint32_t now = millis();

#if !MOCK_MODE
  bool warming = !mqWarmedUp();
#else
  bool warming = false;
#endif

  bool green = false, amber = false, red = false;
  switch (level) {
    case LVL_NORMAL: green = true; break;
    case LVL_WARN:   amber = true; break;
    case LVL_DANGER: red   = true; break;
  }
  if (warming) amber = ((now / 500) % 2) == 0;   // blink while heaters settle

  digitalWrite(PIN_LED_GREEN, green ? HIGH : LOW);
  digitalWrite(PIN_LED_AMBER, amber ? HIGH : LOW);
  digitalWrite(PIN_LED_RED,   red   ? HIGH : LOW);

  /* The LEDs still show DANGER while the heaters settle; only the
     buzzer is held off. Gas already reports 0 until warm, so an alarm
     in the first minute comes from the flame pin or the DHT22 — and
     those are exactly what a half-wired board gets wrong. */
  bool beep = (level == LVL_DANGER) && !silenced
#if BUZZER_SILENT_DURING_WARMUP
           && !warming
#endif
           && (now % BUZZER_PERIOD_MS) < BUZZER_BEEP_MS;
  digitalWrite(PIN_BUZZER, beep ? HIGH : LOW);
}

/* Pressed = LOW with INPUT_PULLUP. Simple time debounce. */
static void handleButton() {
  static bool     last      = false;
  static uint32_t changedAt = 0;
  bool pressed = (digitalRead(PIN_BUTTON) == LOW);
  if (pressed != last && millis() - changedAt > 40) {
    changedAt = millis();
    last = pressed;
    if (pressed) silenceAlarm("button");
  }
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
#if MOCK_MODE
    mq2Ppm, mockRaw(mq2Ppm, 2000.0f),
    mq7Ppm, mockRaw(mq7Ppm, 500.0f),
    flameOn ? 1 : 0, flameOn ? 400 : 3900,
#else
    mq2Ppm, mq2Raw,
    mq7Ppm, mq7Raw,
    flameOn ? 1 : 0, flameRaw,
#endif
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
                  (unsigned long)seq,
                  MOCK_MODE ? SCENARIOS[scenario].name
                            : (level == LVL_DANGER ? "DANGER" : level == LVL_WARN ? "WARN" : "normal"),
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

#if MOCK_MODE
static void setScenario(Scenario s, bool fromSerial) {
  scenario   = s;
  scenarioAt = millis();
  if (fromSerial) autoScenario = false;
  Serial.printf("[scn] → %s%s\n", SCENARIOS[s].name, autoScenario ? " (auto)" : "");
}
#endif

static void handleSerial() {
  while (Serial.available()) {
    switch (Serial.read()) {
#if MOCK_MODE
      case 'n': setScenario(SCN_NORMAL, true);   break;
      case 's': setScenario(SCN_SMOULDER, true); break;
      case 'f': setScenario(SCN_FIRE, true);     break;
      case 'a':
        autoScenario = true;
        scenarioAt   = millis();
        Serial.println("[scn] auto-cycle on");
        break;
#else
      case 'n': case 's': case 'f': case 'a':
        Serial.println("[scn] scenarios are only available with MOCK_MODE 1");
        break;
      case 'c': calibrateR0(); break;
#endif
      case 'q': silenceAlarm("serial"); break;
      case '?':
        Serial.printf("[status] %s | alarm %s%s | wifi %s | ip %s | posts ok=%lu fail=%lu\n",
                      MOCK_MODE ? SCENARIOS[scenario].name : "REAL",
                      level == LVL_DANGER ? "DANGER" : level == LVL_WARN ? "WARN" : "normal",
                      silenced ? " (silenced)" : "",
                      WiFi.status() == WL_CONNECTED ? "up" : "down",
                      WiFi.localIP().toString().c_str(),
                      (unsigned long)okCount, (unsigned long)failCount);
#if !MOCK_MODE
        Serial.printf("[status] MQ2 node %lumV  MQ7 node %lumV  (must be ≤3000)  R0 mq2=%.2fk mq7=%.2fk  %s\n",
                      (unsigned long)readMilliVoltsAvg(PIN_MQ2_AO, 4),
                      (unsigned long)readMilliVoltsAvg(PIN_MQ7_AO, 4),
                      mq2R0, mq7R0, mqWarmedUp() ? "warmed up" : "WARMING UP");
        /* Raw pin, before the polarity setting is applied. Hold a flame to
           the module and watch which way this moves: whichever level it
           shows WITH a flame is the active one, and FLAME_DO_ACTIVE_LOW
           must agree with it. Reading the pin beats trusting the vendor. */
        Serial.printf("[status] flame DO pin=%s → %s (FLAME_DO_ACTIVE_LOW=%d)  AO raw=%d\n",
                      digitalRead(PIN_FLAME_DO) == HIGH ? "HIGH" : "LOW",
                      flameOn ? "FLAME" : "no flame",
                      FLAME_DO_ACTIVE_LOW,
                      readRawAvg(PIN_FLAME_AO, 4));
        Serial.printf("[status] dht %.1f°C %.0f%%RH %s\n", tempC, humPct,
                      dhtEverRead ? "live" : "NEVER READ — these are the start-up placeholders");
#endif
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
  Serial.printf("  mode   : %s\n", MOCK_MODE ? "MOCK — no sensors required" : "REAL sensors (breadboard layout v3)");
  Serial.printf("  target : %s every %dms\n", SERVER_URL, POST_INTERVAL_MS);
#if MOCK_MODE
  Serial.println("  keys   : n normal · s smouldering · f fire · a auto · q silence · ? status");
#else
  Serial.println("  keys   : c calibrate R0 · q silence · ? status");
#endif
  Serial.println();

  bootAt = millis();

  /* Outputs first, so nothing floats while Wi-Fi comes up. */
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_AMBER, OUTPUT);
  pinMode(PIN_LED_RED,   OUTPUT);
  pinMode(PIN_BUZZER,    OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BUTTON, INPUT_PULLUP);      // pressed = LOW

  /* Quick lamp test so a missing LED is obvious at power-up. */
  const int leds[] = { PIN_LED_GREEN, PIN_LED_AMBER, PIN_LED_RED };
  for (int p : leds) { digitalWrite(p, HIGH); delay(150); digitalWrite(p, LOW); }

#if !MOCK_MODE
  /* Pull the idle level to "no flame" for whichever polarity is set, so
     an unplugged module stays quiet instead of faking a fire. The module
     drives DO hard enough to win against the internal resistor. */
#if FLAME_DO_ACTIVE_LOW
  pinMode(PIN_FLAME_DO, INPUT_PULLUP);
#else
  pinMode(PIN_FLAME_DO, INPUT_PULLDOWN);
#endif
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_MQ2_AO,   ADC_11db);   // full 0 to ~3.1 V range
  analogSetPinAttenuation(PIN_MQ7_AO,   ADC_11db);
  analogSetPinAttenuation(PIN_FLAME_AO, ADC_11db);
  dht.begin();
  Serial.printf("[mq] heaters warming up for %d s, gas reported as 0 until then\n", MQ_WARMUP_MS / 1000);
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
  handleButton();

#if MOCK_MODE
  if (autoScenario && millis() - scenarioAt >= SCENARIO_HOLD_MS) {
    setScenario((Scenario)((scenario + 1) % SCENARIO_COUNT), false);
  }
#endif

  if (millis() - lastPostAt >= POST_INTERVAL_MS) {
    lastPostAt = millis();
#if MOCK_MODE
    stepMockSensors();
#else
    readRealSensors();
#endif
    updateLevel();
    postReadings();
  }

  driveIndicators();

  delay(10);   // keep the Wi-Fi stack's housekeeping fed
}
