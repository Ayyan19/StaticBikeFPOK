// =============================================================================
//  StaticBikeMonitor.ino
//  Monitor sepeda statis berbasis ESP32:
//    - RPM/cadence      : rotary encoder quadrature A/B (dekat pedal, ke gear)
//    - Level & torsi     : VL53L0X (jarak magnet beban di pulley depan)
//    - Heart rate        : sensor bawaan Bodymax (reverse engineering)
//    - Tampilan          : TFT LCD (TFT_eSPI)
//    - Upload wireless    : WiFi internal -> web server + WebSocket (dashboard live)
//
//  Metrik yang dihitung: cadence (rpm engkol), speed virtual (km/j), torsi (Nm),
//  level beban (1..N), power (Watt), energi (kJ) & kalori, heart rate (BPM).
//
//  Library yang dibutuhkan (Library Manager / PlatformIO):
//    - ESP32Encoder            (madhephaestus)
//    - Adafruit_VL53L0X
//    - TFT_eSPI                (bodmer) -- atur User_Setup.h sesuai layar Anda
//    - ESPAsyncWebServer + AsyncTCP  (untuk web server & WebSocket async)
//
//  Board: "ESP32 Dev Module" (atau varian Anda).
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <ESP32Encoder.h>
#include <Adafruit_VL53L0X.h>
#include <TFT_eSPI.h>
#include <ESPAsyncWebServer.h>

#include "config.h"
#include "index_html.h"   // dashboard HTML disimpan sebagai PROGMEM string

// ------------------------- Objek global -------------------------------------
ESP32Encoder       encoder;
Adafruit_VL53L0X   lox;
TFT_eSPI           tft = TFT_eSPI();
AsyncWebServer     server(80);
AsyncWebSocket     ws("/ws");

// ------------------------- State pengukuran ---------------------------------
volatile bool  vl53_ok = false;

// Metrik hasil kalkulasi (dibaca loop utama, dikirim ke TFT & WS)
struct Metrics {
  float cadenceRpm   = 0;   // rpm engkol pedal
  float speedKmh     = 0;   // kecepatan virtual
  float torqueNm     = 0;   // torsi kayuhan
  int   level        = 1;   // level beban 1..LEVEL_COUNT
  float distanceMm   = 0;   // jarak mentah VL53L0X
  float powerW       = 0;   // daya mekanik
  float energyKJ     = 0;   // akumulasi energi
  float calories     = 0;   // estimasi kalori
  int   heartRate    = 0;   // BPM
  uint32_t elapsedS  = 0;   // durasi sesi (detik)
} m;

// ------------------------- Heart rate ISR state -----------------------------
volatile uint32_t hrLastBeatMs   = 0;
volatile uint32_t hrIntervalMs   = 0;   // interval antar detak terakhir
volatile bool     hrNewBeat      = false;

#if HR_MODE == HR_MODE_PULSE
void IRAM_ATTR hrPulseISR() {
  uint32_t now = millis();
  uint32_t dt  = now - hrLastBeatMs;
  // Debounce sederhana: interval minimum sesuai HR_MAX_BPM
  if (dt > (60000UL / HR_MAX_BPM)) {
    hrIntervalMs = dt;
    hrLastBeatMs = now;
    hrNewBeat    = true;
  }
}
#endif

// ============================================================================
//  SETUP
// ============================================================================
uint32_t sessionStartMs = 0;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[BikeMonitor] Boot...");

  // ---- Encoder (quadrature x4) ----
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
#if ENCODER_X4
  encoder.attachFullQuad(ENCODER_PIN_A, ENCODER_PIN_B);   // resolusi x4
#else
  encoder.attachHalfQuad(ENCODER_PIN_A, ENCODER_PIN_B);   // resolusi x2
#endif
  encoder.clearCount();

  // ---- I2C + VL53L0X ----
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
#if VL53_XSHUT_PIN >= 0
  pinMode(VL53_XSHUT_PIN, OUTPUT);
  digitalWrite(VL53_XSHUT_PIN, HIGH);
  delay(10);
#endif
  vl53_ok = lox.begin();
  if (vl53_ok) {
    lox.startRangeContinuous();   // mode kontinu -> pembacaan cepat non-blocking
    Serial.println("[VL53L0X] OK (continuous)");
  } else {
    Serial.println("[VL53L0X] TIDAK terdeteksi -- cek wiring I2C");
  }

  // ---- Heart rate ----
#if HR_MODE == HR_MODE_PULSE
  pinMode(HR_PULSE_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HR_PULSE_PIN), hrPulseISR, FALLING);
#else
  pinMode(HR_ANALOG_PIN, INPUT);
  analogReadResolution(12);   // 0..4095
#endif

  // ---- TFT ----
  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(TFT_BLACK);
  drawStaticLayout();

  // ---- WiFi ----
  setupWiFi();

  // ---- Web server + WebSocket ----
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });
  // Endpoint JSON polling (alternatif bila klien tak pakai WebSocket)
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", buildJson());
  });
  server.begin();
  Serial.println("[HTTP] Server aktif di port 80");

  sessionStartMs = millis();
}

// ============================================================================
//  LOOP
// ============================================================================
uint32_t lastSampleMs = 0;
uint32_t lastTftMs    = 0;
uint32_t lastWsMs     = 0;
int64_t  lastEncCount = 0;

void loop() {
  uint32_t now = millis();

  // ---- 1) Sampling RPM tiap SAMPLE_WINDOW_MS ----
  if (now - lastSampleMs >= SAMPLE_WINDOW_MS) {
    float dtSec = (now - lastSampleMs) / 1000.0f;
    lastSampleMs = now;
    updateCadence(dtSec);
    updateLoadAndTorque();
    updatePower(dtSec);
    updateHeartRate(now);
    m.elapsedS = (now - sessionStartMs) / 1000;
  }

  // ---- 2) Refresh TFT ----
  if (now - lastTftMs >= TFT_REFRESH_MS) {
    lastTftMs = now;
    drawValues();
  }

  // ---- 3) Push data ke dashboard ----
  if (now - lastWsMs >= WS_PUSH_MS) {
    lastWsMs = now;
    ws.textAll(buildJson());
    ws.cleanupClients();
  }
}

// ============================================================================
//  KALKULASI
// ============================================================================

// --- Cadence (rpm engkol) dari perubahan hitungan encoder ---
void updateCadence(float dtSec) {
  int64_t c    = encoder.getCount();
  int64_t d    = c - lastEncCount;
  lastEncCount = c;

  // Hitungan per revolusi poros-encoder pada mode x4
  const float countsPerRev = (float)ENCODER_PPR * (ENCODER_X4 ? 4.0f : 2.0f);

  // Putaran poros-encoder per detik -> rpm poros
  float revEncPerSec = (fabs((float)d) / countsPerRev) / dtSec;
  float shaftRpm     = revEncPerSec * 60.0f * ENCODER_DIRECTION;

  // rpm engkol pedal (koreksi rasio gear pemasangan encoder)
  m.cadenceRpm = fabs(shaftRpm) / GEAR_RATIO_ENC_PER_CRANK;

  // --- Speed virtual ---
  // Putaran roda per menit = cadence * rasio transmisi virtual
  float wheelRpm   = m.cadenceRpm * DRIVE_RATIO_WHEEL_PER_CRANK;
  float wheelRevPS = wheelRpm / 60.0f;
  float speedMps   = wheelRevPS * WHEEL_CIRCUMFERENCE_M;
  m.speedKmh       = speedMps * 3.6f;
}

// --- Jarak VL53L0X -> level beban -> torsi ---
void updateLoadAndTorque() {
  if (vl53_ok && lox.isRangeComplete()) {
    float mm = lox.readRange();               // mm
    // Filter low-pass ringan untuk menghaluskan pembacaan
    m.distanceMm = (m.distanceMm == 0) ? mm : (0.7f * m.distanceMm + 0.3f * mm);
  }

  // Petakan jarak -> fraksi beban 0..1 (memperhitungkan arah kalibrasi)
  float span = DIST_AT_MIN_LOAD_MM - DIST_AT_MAX_LOAD_MM;   // bisa +/-
  float frac = 0.0f;
  if (fabs(span) > 0.001f) {
    frac = (DIST_AT_MIN_LOAD_MM - m.distanceMm) / span;     // 0=ringan, 1=berat
  }
  frac = constrain(frac, 0.0f, 1.0f);

  // Level 1..LEVEL_COUNT
  m.level = (int)roundf(frac * (LEVEL_COUNT - 1)) + 1;
  m.level = constrain(m.level, 1, LEVEL_COUNT);

  // --- Torsi (Nm) dari level + koreksi cadence (rem magnetik) ---
  float torqueBase = TORQUE_BASE_NM + TORQUE_PER_LEVEL_NM * (m.level - 1);
  float cadCorr    = 1.0f + TORQUE_CADENCE_GAIN * (m.cadenceRpm - CADENCE_REF_RPM);
  cadCorr          = constrain(cadCorr, 0.3f, 3.0f);
  m.torqueNm       = (m.cadenceRpm > 1.0f) ? torqueBase * cadCorr : 0.0f;
}

// --- Power (Watt) = torsi * kecepatan sudut engkol ; + energi & kalori ---
void updatePower(float dtSec) {
  float omega = 2.0f * PI * (m.cadenceRpm / 60.0f);   // rad/s engkol
  m.powerW    = m.torqueNm * omega;                   // W = Nm * rad/s

  m.energyKJ += (m.powerW * dtSec) / 1000.0f;         // integral daya -> kJ
  // Kalori mekanik: 1 kcal = 4.184 kJ. Efisiensi tubuh ~24% -> kalori metabolik.
  const float EFF = 0.24f;
  m.calories = (m.energyKJ / 4.184f) / EFF;
}

// --- Heart rate ---
void updateHeartRate(uint32_t now) {
#if HR_MODE == HR_MODE_PULSE
  if (hrNewBeat) {
    hrNewBeat  = false;
    uint32_t iv = hrIntervalMs;
    if (iv > 0) {
      int bpm = 60000 / iv;
      if (bpm >= HR_MIN_BPM && bpm <= HR_MAX_BPM) m.heartRate = bpm;
    }
  }
  // Timeout: tidak ada detak -> 0
  if (now - hrLastBeatMs > HR_TIMEOUT_MS) m.heartRate = 0;
#else
  // Mode analog: deteksi puncak sederhana dengan threshold + refractory
  static bool above = false;
  int v = analogRead(HR_ANALOG_PIN);
  if (!above && v > HR_ANALOG_THRESHOLD) {
    above = true;
    uint32_t dt = now - hrLastBeatMs;
    if (dt > (60000UL / HR_MAX_BPM)) {
      int bpm = 60000 / dt;
      if (bpm >= HR_MIN_BPM && bpm <= HR_MAX_BPM) m.heartRate = bpm;
      hrLastBeatMs = now;
    }
  } else if (above && v < (HR_ANALOG_THRESHOLD - 200)) {
    above = false;   // histeresis turun
  }
  if (now - hrLastBeatMs > HR_TIMEOUT_MS) m.heartRate = 0;
#endif
}

// ============================================================================
//  JSON untuk dashboard
// ============================================================================
String buildJson() {
  char buf[320];
  snprintf(buf, sizeof(buf),
    "{\"cadence\":%.1f,\"speed\":%.1f,\"torque\":%.2f,\"level\":%d,"
    "\"power\":%.1f,\"hr\":%d,\"energyKJ\":%.2f,\"cal\":%.1f,"
    "\"distMm\":%.0f,\"elapsed\":%lu}",
    m.cadenceRpm, m.speedKmh, m.torqueNm, m.level,
    m.powerW, m.heartRate, m.energyKJ, m.calories,
    m.distanceMm, (unsigned long)m.elapsedS);
  return String(buf);
}

// ============================================================================
//  WIFI
// ============================================================================
void setupWiFi() {
#if WIFI_MODE == WIFI_MODE_AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[WiFi] AP '%s' aktif. Buka http://%s\n", AP_SSID, ip.toString().c_str());
  tftBanner("AP: " + String(AP_SSID), "http://" + ip.toString());
#else
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASSWORD);
  Serial.print("[WiFi] Menyambung");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    Serial.printf("\n[WiFi] Terhubung. Buka http://%s\n", ip.toString().c_str());
    tftBanner("WiFi OK", "http://" + ip.toString());
  } else {
    Serial.println("\n[WiFi] Gagal -- cek SSID/Password");
    tftBanner("WiFi GAGAL", "cek kredensial");
  }
#endif
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("[WS] Klien #%u tersambung\n", client->id());
    client->text(buildJson());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("[WS] Klien #%u putus\n", client->id());
  }
}

// ============================================================================
//  TAMPILAN TFT
// ============================================================================
void tftBanner(const String &l1, const String &l2) {
  tft.fillRect(0, 0, tft.width(), 40, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(1);
  tft.setCursor(6, 6);  tft.print(l1);
  tft.setCursor(6, 22); tft.print(l2);
}

void drawStaticLayout() {
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(6,  50); tft.print("CADENCE (rpm)");
  tft.setCursor(6,  95); tft.print("SPEED (km/j)");
  tft.setCursor(6, 140); tft.print("TORSI (Nm)");
  tft.setCursor(6, 185); tft.print("POWER (W)");
  tft.setCursor(170, 50); tft.print("LEVEL");
  tft.setCursor(170, 95); tft.print("HEART (BPM)");
  tft.setCursor(170,140); tft.print("ENERGI (kJ)");
  tft.setCursor(170,185); tft.print("WAKTU");
}

void drawValues() {
  tft.setTextSize(2);
  auto field = [&](int x, int y, const String &val, uint16_t color) {
    tft.fillRect(x, y, 150, 22, TFT_BLACK);
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(x, y); tft.print(val);
  };
  field(6,   62, String(m.cadenceRpm, 0),  TFT_CYAN);
  field(6,  107, String(m.speedKmh, 1),    TFT_GREEN);
  field(6,  152, String(m.torqueNm, 1),    TFT_YELLOW);
  field(6,  197, String(m.powerW, 0),      TFT_ORANGE);
  field(170, 62, String(m.level),          TFT_MAGENTA);
  field(170,107, m.heartRate ? String(m.heartRate) : "--", TFT_RED);
  field(170,152, String(m.energyKJ, 1),    TFT_WHITE);
  char t[10];
  snprintf(t, sizeof(t), "%02u:%02u", m.elapsedS / 60, m.elapsedS % 60);
  field(170,197, String(t),                TFT_WHITE);
}
