#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <ESP32Encoder.h>
#include <ESP32Servo.h>
#include <MAX30100_PulseOximeter.h>
#include <TFT_eSPI.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

using fs::File;

// =============================================================================
// 1. KONFIGURASI
// =============================================================================

// ---- WiFi (ESP32 = Access Point, KONTINU) ----
static const char* AP_SSID     = "FPOKTeraphyBike";
static const char* AP_PASSWORD = "coe123456";       // min 8 karakter
static const int   AP_CHANNEL  = 6;
static const int   AP_MAX_CONN = 4;

// ---- Encoder (cadence) ----
#define ENCODER_PIN_A       25
#define ENCODER_PIN_B       26
#define ENCODER_PPR         600
#define ENCODER_X4          1
#define ENCODER_DIRECTION   1
#define GEAR_RATIO          1.0f

// ---- I2C (MAX30100 numpang bus ini; VLX sudah dihapus) ----
#define I2C_SDA_PIN         21
#define I2C_SCL_PIN         22

// =============================================================================
//  ####  KALIBRASI FISIK BELUM DIUBAH  ####
// =============================================================================
// ---- Umum ----
#define CRANK_LENGTH_M       0.238f

// ---- Flywheel (torsi & power) ----
#define FLYWHEEL_MASS_KG     5.0f    // TODO: massa flywheel (kg)
#define FLYWHEEL_RADIUS_M    0.25f   // TODO: jari-jari flywheel (m)
#define FLYWHEEL_SHAPE       1       // 0 = pejal (0.5 m r^2) ; 1 = pelek (m r^2)

// ---- Speed virtual ----
#define WHEEL_CIRC_M         2.105f
#define DRIVE_RATIO          3.0f

// ---- SERVO LEVELING (beban) ----
#define SERVO_PIN            13
#define LEVEL_COUNT          8       // jumlah level beban (1..8)
#define SERVO_MIN_DEG        10.0f   // sudut level 1 (beban paling ringan)
#define SERVO_MAX_DEG        170.0f  // sudut level 8 (beban paling berat)
#define SERVO_RATE_DEG_S     40.0f   // batas laju gerak servo (derajat/detik)
#define GRADE_PER_LEVEL      2.0f    // mode sim: tiap +2% grade naik 1 level

// ---- Kurva BEBAN (torsi estimasi dari level) ----
#define TORQUE_BASE_NM       3.0f
#define TORQUE_PER_LEVEL_NM  2.2f
#define CADENCE_REF_RPM     60.0f
#define TORQUE_CADENCE_GAIN  0.010f

// ---- TFT ----
#define TFT_ROTATION          1

// ---- Timing ----
#define SAMPLE_MS           250
#define SERVO_UPDATE_MS      20
#define TFT_REFRESH_MS      200
#define WS_PUSH_MS          500

// ---- Session Defaults ----
#define DEFAULT_DURATION_MIN 10
#define MAX_DURATION_MIN     60
#define MIN_DURATION_MIN      1
#define SESSION_FILE     "/sessions.csv"

// =============================================================================
// 2. STATE MACHINE
// =============================================================================
enum SystemState {
  ST_BOOT, ST_SELF_CHECK, ST_READY, ST_RUNNING, ST_PAUSED, ST_SUMMARY, ST_FAULT
};
const char* stateNames[] = {
  "BOOT", "SELF_CHECK", "READY", "RUNNING", "PAUSED", "SUMMARY", "FAULT"
};

// =============================================================================
// 3. DATA STRUCTURES
// =============================================================================
struct Metrics {
  float cadenceRpm = 0;
  float torqueNm   = 0;
  float powerW     = 0;
  float speedKmh   = 0;
  int   level      = 1;    // level beban aktual (dari posisi servo)
  float servoAngle = SERVO_MIN_DEG;
  float heartRate  = 0;    // dari MAX30100 (core-1)
  float rotations  = 0;
};
struct SessionAccum {
  uint32_t sampleCount = 0;
  float sumCadence = 0, maxCadence = 0;
  float sumTorque  = 0, maxTorque  = 0;
  float sumPower   = 0, maxPower   = 0;
  float sumLevel   = 0; int maxLevel = 0;
  float sumHR      = 0, maxHR = 0; uint32_t hrCount = 0;
  uint32_t totalPulses = 0;
  uint32_t stopCount   = 0;
  bool     wasPedaling = false;
};
struct SessionSummary {
  uint32_t sessionId = 0, durationSec = 0;
  float avgCadence = 0, maxCadence = 0;
  float avgTorque = 0, maxTorque = 0;
  float avgPower = 0, maxPower = 0;
  float avgLevel = 0; int maxLevel = 0;
  float avgHR = 0, maxHR = 0;
  float totalRotations = 0;
  uint32_t stopCount = 0;
  uint8_t status = 0;   // 0=normal,1=stop,2=emergency
};

inline float flywheelInertia() {
  float base = FLYWHEEL_MASS_KG * FLYWHEEL_RADIUS_M * FLYWHEEL_RADIUS_M;
  return (FLYWHEEL_SHAPE == 0) ? 0.5f * base : base;
}

// =============================================================================
// 4. GLOBAL OBJECTS
// =============================================================================
ESP32Encoder    encoder;
Servo           servoLoad;
PulseOximeter   pox;
TFT_eSPI        tft = TFT_eSPI();
AsyncWebServer  server(80);
AsyncWebSocket  ws("/ws");

SystemState sysState = ST_BOOT, prevState = ST_BOOT;
bool        stateChanged = true;
bool        fs_ok = false;
bool        hr_ok = false;

Metrics        met;
SessionAccum   acc;
SessionSummary lastSummary;

uint32_t sessionId = 0, sessionStartMs = 0, sessionElapsed = 0;
uint16_t targetDurationMin = DEFAULT_DURATION_MIN;

int64_t lastEncCount = 0;
float   gOmega = 0, gAlpha = 0, lastOmega = 0;
uint32_t lastSampleMs = 0, lastServoMs = 0, lastTftMs = 0, lastWsMs = 0;

// ---- Kendali beban / mode ----
String  g_mode        = "manual";   // "sim" | "manual" | "cognitive"
int     targetLevel   = 1;          // level yang dituju
float   g_grade       = 0;          // dari aplikasi (mode sim)
float   currentAngle  = SERVO_MIN_DEG;

// ---- Heart rate (dibagi dengan task core-1) ----
volatile float g_heartRate = 0;

String faultMsg = "";

// =============================================================================
// 5. DASHBOARD HTML — MINIMALIS
// =============================================================================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>Therapy Bike Monitor</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,'Segoe UI',Arial,sans-serif;background:#f4f4f4;color:#222;padding:16px;line-height:1.4}
.wrap{max-width:640px;margin:0 auto}
h1{font-size:17px;font-weight:600;text-align:center;margin-bottom:2px}
.sub{text-align:center;font-size:11px;color:#888;margin-bottom:12px}
.state{text-align:center;padding:8px;border:1px solid #ccc;background:#fff;border-radius:4px;font-size:13px;font-weight:600;margin-bottom:10px}
.mode{text-align:center;font-size:12px;color:#555;margin-bottom:10px}
.mode button{border:1px solid #999;background:#fff;border-radius:4px;padding:4px 12px;margin:0 3px;cursor:pointer;font-size:12px}
.mode button.on{background:#333;color:#fff}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:8px;margin-bottom:12px}
.card{background:#fff;border:1px solid #ddd;border-radius:4px;padding:10px 12px}
.card .label{font-size:11px;color:#777}
.card .value{font-size:22px;font-weight:600;font-variant-numeric:tabular-nums}
.card .unit{font-size:11px;color:#999}
.info{background:#fff;border:1px solid #ddd;border-radius:4px;padding:10px 12px;margin-bottom:12px;font-size:13px}
.info .row{display:flex;justify-content:space-between;align-items:center;padding:3px 0}
.info .lbl{color:#777}
.dbtn{background:#fff;border:1px solid #bbb;border-radius:3px;width:24px;height:24px;font-size:14px;cursor:pointer}
.dbtn:disabled{opacity:.4;cursor:not-allowed}
.controls{text-align:center;margin-bottom:12px}
.controls button{padding:9px 20px;border:1px solid #999;background:#fff;color:#222;border-radius:4px;font-size:13px;cursor:pointer;margin:3px}
.controls button:disabled{opacity:.4;cursor:not-allowed}
.summary{background:#fff;border:1px solid #ddd;border-radius:4px;padding:12px}
.summary h3{font-size:13px;font-weight:600;margin-bottom:8px;text-align:center}
.summary .row{display:flex;justify-content:space-between;padding:2px 0;font-size:13px}
.summary .lbl{color:#777}
.note{text-align:center;font-size:11px;color:#999;margin-top:14px}
</style></head><body><div class="wrap">
<h1>Therapy Bike Monitor</h1>
<div class="sub">Monitoring latihan &mdash; bukan alat diagnosis</div>
<div class="state" id="stateBar">Menghubungkan...</div>
<div class="mode">Mode:
  <button id="mSim" onclick="setMode('sim')">SIM</button>
  <button id="mMan" onclick="setMode('manual')">MANUAL</button>
</div>
<div class="grid">
  <div class="card"><div class="label">Cadence</div><div class="value" id="cadence">0</div><div class="unit">RPM</div></div>
  <div class="card"><div class="label">Detak Jantung</div><div class="value" id="hr">--</div><div class="unit">BPM</div></div>
  <div class="card"><div class="label">Power</div><div class="value" id="power">0</div><div class="unit">Watt &middot; est</div></div>
  <div class="card"><div class="label">Speed</div><div class="value" id="speed">0</div><div class="unit">km/j &middot; virtual</div></div>
  <div class="card"><div class="label">Torsi</div><div class="value" id="torque">0</div><div class="unit">Nm &middot; est</div></div>
  <div class="card"><div class="label">Level Beban</div><div class="value" id="level">1</div><div class="unit">1-8</div></div>
  <div class="card"><div class="label">Servo</div><div class="value" id="servo">0</div><div class="unit">derajat</div></div>
  <div class="card"><div class="label">Waktu Sesi</div><div class="value" id="elapsed">00:00</div><div class="unit">mm:ss</div></div>
</div>
<div class="info">
  <div class="row"><span class="lbl">Atur Level (mode manual)</span>
    <span><button class="dbtn" id="lvDown" onclick="sendCmd('levelDown')">&minus;</button>
    <span id="target">1</span>
    <button class="dbtn" id="lvUp" onclick="sendCmd('levelUp')">+</button></span></div>
  <div class="row"><span class="lbl">Grade (mode sim, dari app)</span><span id="grade">0 %</span></div>
  <div class="row"><span class="lbl">Durasi Target</span>
    <span><button class="dbtn" onclick="sendCmd('durDown')">&minus;</button>
    <span id="dur">10 menit</span>
    <button class="dbtn" onclick="sendCmd('durUp')">+</button></span></div>
  <div class="row"><span class="lbl">Berhenti</span><span id="stops">0 kali</span></div>
</div>
<div class="controls">
  <button id="btnStart" onclick="sendCmd('start')">START</button>
  <button id="btnPause" onclick="sendCmd('pause')" disabled>PAUSE</button>
  <button id="btnStop" onclick="sendCmd('stop')" disabled>STOP</button>
  <button id="btnReset" onclick="sendCmd('reset')" style="display:none">RESET</button>
</div>
<div class="summary" id="summaryBox" style="display:none"><h3>Ringkasan Sesi</h3><div id="summaryContent"></div></div>
<div class="note">Torsi/power estimasi. HR kelas kebugaran, bukan diagnosis.</div>
</div>
<script>
function updateUI(d){
  document.getElementById('cadence').textContent=d.cadence.toFixed(1);
  document.getElementById('hr').textContent=(d.heartRate>0?d.heartRate.toFixed(0):'--');
  document.getElementById('power').textContent=d.power.toFixed(0);
  document.getElementById('speed').textContent=d.speed.toFixed(1);
  document.getElementById('torque').textContent=d.torque.toFixed(1);
  document.getElementById('level').textContent=d.level;
  document.getElementById('servo').textContent=d.servoAngle.toFixed(0);
  document.getElementById('target').textContent=d.targetLevel;
  document.getElementById('grade').textContent=d.grade.toFixed(0)+' %';
  document.getElementById('dur').textContent=d.targetMin+' menit';
  document.getElementById('stops').textContent=d.stops+' kali';
  let s=d.elapsed,m=Math.floor(s/60),sec=s%60;
  document.getElementById('elapsed').textContent=String(m).padStart(2,'0')+':'+String(sec).padStart(2,'0');
  document.getElementById('stateBar').textContent=d.stateLabel;
  document.getElementById('mSim').className=(d.mode==='sim')?'on':'';
  document.getElementById('mMan').className=(d.mode==='manual')?'on':'';
  let bS=document.getElementById('btnStart'),bP=document.getElementById('btnPause'),bT=document.getElementById('btnStop'),bR=document.getElementById('btnReset');
  let sb=document.getElementById('summaryBox');
  let lu=document.getElementById('lvUp'),ld=document.getElementById('lvDown');
  let man=(d.mode==='manual');lu.disabled=!man;ld.disabled=!man;
  bS.style.display='inline-block';bP.style.display='inline-block';bT.style.display='inline-block';bR.style.display='none';sb.style.display='none';
  if(d.state==='READY'){bS.disabled=false;bP.disabled=true;bT.disabled=true}
  else if(d.state==='RUNNING'){bS.disabled=true;bP.disabled=false;bT.disabled=false}
  else if(d.state==='PAUSED'){bS.disabled=false;bS.textContent='RESUME';bP.disabled=true;bT.disabled=false}
  else if(d.state==='SUMMARY'){bS.style.display='none';bP.style.display='none';bT.style.display='none';bR.style.display='inline-block';sb.style.display='block';
    if(d.summary){let h='';for(let k in d.summary)h+='<div class="row"><span class="lbl">'+k+'</span><span>'+d.summary[k]+'</span></div>';document.getElementById('summaryContent').innerHTML=h}}
  else if(d.state==='FAULT'){bS.disabled=true;bP.disabled=true;bT.style.display='none';bR.style.display='inline-block'}
  else{bS.disabled=true;bP.disabled=true;bT.disabled=true}
  if(d.state!=='PAUSED')bS.textContent='START';
}
function sendCmd(c){fetch('/cmd?action='+c).then(r=>r.text()).catch(()=>{})}
function setMode(m){fetch('/set?mode='+m).then(r=>r.text()).catch(()=>{})}
let ws,lastMsg=0;
function poll(){fetch('/data').then(r=>r.json()).then(updateUI).catch(()=>{})}
setInterval(()=>{ if(Date.now()-lastMsg>1500) poll(); },1000);
function connect(){
  try{ws=new WebSocket('ws://'+location.host+'/ws');
    ws.onclose=()=>setTimeout(connect,2000);
    ws.onerror=()=>{try{ws.close()}catch(e){}};
    ws.onmessage=e=>{lastMsg=Date.now();try{updateUI(JSON.parse(e.data))}catch(err){}};
  }catch(e){}
}
connect();poll();
</script></body></html>
)rawliteral";

// =============================================================================
// 6. FORWARD DECLARATIONS
// =============================================================================
void updateCadence(float dtSec);
void updateLoadAndTorque();
void updateServo(float dtSec);
int  gradeToLevel(float grade);
float angleForLevel(int level);
int  levelForAngle(float angle);
void selfCheck();
void enterState(SystemState s);
void handleStateLogic();
void accumulateSample();
SessionSummary buildSummary(uint8_t status);
void saveSession(const SessionSummary &s);
void resetSession();
String buildJson();
void applyControl(const String &mode, bool hasGrade, float grade, bool hasLevel, int level);
void tftDrawState();
void tftDrawMetrics();
void tftBanner(const String &l1, const String &l2, uint16_t bg);
void setupWiFi();
void setupWebServer();
void hrTask(void *param);
void doStartResume(); void doPauseOrStop(); void doStopFromRun(); void doReset();
void durUp(); void durDown(); void levelUp(); void levelDown();

// =============================================================================
// 7. SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n Sepeda Statis FPOK — Servo leveling + Sim + HR");

  // Encoder
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  #if ENCODER_X4
    encoder.attachFullQuad(ENCODER_PIN_A, ENCODER_PIN_B);
  #else
    encoder.attachHalfQuad(ENCODER_PIN_A, ENCODER_PIN_B);
  #endif
  encoder.clearCount();

  // I2C + MAX30100
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  hr_ok = pox.begin();
  if (hr_ok) { pox.setIRLedCurrent(MAX30100_LED_CURR_7_6MA); Serial.println("[MAX30100] OK"); }
  else       Serial.println("[MAX30100] GAGAL — cek wiring/pull-up I2C");

  // Servo (alokasikan timer LEDC agar tidak bentrok)
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  servoLoad.setPeriodHertz(50);
  servoLoad.attach(SERVO_PIN, 500, 2400);
  currentAngle = SERVO_MIN_DEG;
  servoLoad.write((int)currentAngle);

  // LittleFS
  fs_ok = LittleFS.begin(true);
  if (fs_ok) {
    Serial.println("[LittleFS] OK");
    if (LittleFS.exists(SESSION_FILE)) {
      fs::File f = LittleFS.open(SESSION_FILE, "r");
      bool first = true;
      while (f.available()) {
        String line = f.readStringUntil('\n');
        if (line.length() == 0) continue;
        if (first) { first = false; continue; }
        sessionId++;
      }
      f.close();
      Serial.printf("[Session] %u tersimpan\n", sessionId);
    }
  } else Serial.println("[LittleFS] GAGAL");

  // TFT
  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(TFT_BLACK);

  // WiFi + Web
  setupWiFi();
  setupWebServer();

  // Task heart rate di core-1
  if (hr_ok)
    xTaskCreatePinnedToCore(hrTask, "hrTask", 4096, NULL, 1, NULL, 1);

  enterState(ST_SELF_CHECK);
}

// =============================================================================
// 8. LOOP
// =============================================================================
void loop() {
  uint32_t now = millis();
  met.heartRate = g_heartRate;   // ambil hasil dari task HR

  // Servo update (halus, 20 ms)
  if (now - lastServoMs >= SERVO_UPDATE_MS) {
    float dt = (now - lastServoMs) / 1000.0f;
    lastServoMs = now;
    updateServo(dt);
  }

  // Sampling sensor (250 ms)
  if (sysState != ST_BOOT && sysState != ST_FAULT) {
    if (now - lastSampleMs >= SAMPLE_MS) {
      float dtSec = (now - lastSampleMs) / 1000.0f;
      lastSampleMs = now;
      updateCadence(dtSec);
      updateLoadAndTorque();

      if (sysState == ST_RUNNING) {
        sessionElapsed = (now - sessionStartMs) / 1000;
        accumulateSample();
        if (sessionElapsed >= (uint32_t)targetDurationMin * 60) {
          lastSummary = buildSummary(0);
          saveSession(lastSummary);
          enterState(ST_SUMMARY);
        }
      }
    }
  }

  handleStateLogic();

  if (now - lastTftMs >= TFT_REFRESH_MS) {
    lastTftMs = now;
    if (stateChanged) { tftDrawState(); stateChanged = false; }
    if (sysState == ST_RUNNING || sysState == ST_READY || sysState == ST_PAUSED)
      tftDrawMetrics();
  }

  if (now - lastWsMs >= WS_PUSH_MS) {
    lastWsMs = now;
    ws.textAll(buildJson());
    ws.cleanupClients();
  }
}

// =============================================================================
// 9. STATE MACHINE
// =============================================================================
void enterState(SystemState s) {
  prevState = sysState; sysState = s; stateChanged = true;
  Serial.printf("[State] %s -> %s\n", stateNames[prevState], stateNames[s]);
}
void selfCheck() {
  // Sensor inti (encoder) selalu siap; HR & FS tidak fatal.
  enterState(ST_READY);
}
void handleStateLogic() {
  switch (sysState) {
    case ST_BOOT:       enterState(ST_SELF_CHECK); break;
    case ST_SELF_CHECK: selfCheck();               break;
    default: break;
  }
}

// =============================================================================
// 10. AKSI SESI & KENDALI LEVEL
// =============================================================================
void doStartResume() {
  if (sysState == ST_READY) {
    resetSession(); sessionStartMs = millis(); enterState(ST_RUNNING);
  } else if (sysState == ST_PAUSED) {
    sessionStartMs = millis() - (sessionElapsed * 1000UL); enterState(ST_RUNNING);
  }
}
void doPauseOrStop() {
  if (sysState == ST_RUNNING) enterState(ST_PAUSED);
  else if (sysState == ST_PAUSED) { lastSummary = buildSummary(1); saveSession(lastSummary); enterState(ST_SUMMARY); }
  else if (sysState == ST_SUMMARY || sysState == ST_FAULT) { faultMsg=""; enterState(ST_SELF_CHECK); }
}
void doStopFromRun() {
  if (sysState == ST_RUNNING || sysState == ST_PAUSED) { lastSummary = buildSummary(1); saveSession(lastSummary); enterState(ST_SUMMARY); }
}
void doReset() { if (sysState == ST_SUMMARY || sysState == ST_FAULT) { faultMsg=""; enterState(ST_SELF_CHECK); } }
void durUp()   { if (sysState == ST_READY && targetDurationMin < MAX_DURATION_MIN) { targetDurationMin++; stateChanged = true; } }
void durDown() { if (sysState == ST_READY && targetDurationMin > MIN_DURATION_MIN) { targetDurationMin--; stateChanged = true; } }
void levelUp()   { if (g_mode == "manual" && targetLevel < LEVEL_COUNT) targetLevel++; }
void levelDown() { if (g_mode == "manual" && targetLevel > 1) targetLevel--; }

// Terapkan perintah dari aplikasi / web
void applyControl(const String &mode, bool hasGrade, float grade, bool hasLevel, int level) {
  if (mode == "sim" || mode == "manual" || mode == "cognitive") g_mode = mode;
  if (hasGrade)  g_grade = grade;
  if (hasLevel)  targetLevel = constrain(level, 1, LEVEL_COUNT);
}

// =============================================================================
// 11. SENSOR: CADENCE
// =============================================================================
void updateCadence(float dtSec) {
  int64_t c = encoder.getCount();
  int64_t d = c - lastEncCount;
  lastEncCount = c;

  const float countsPerRev = (float)ENCODER_PPR * (ENCODER_X4 ? 4.0f : 2.0f);
  float revEncPerSec = (fabs((float)d) / countsPerRev) / dtSec;
  float shaftRpm     = revEncPerSec * 60.0f * ENCODER_DIRECTION;
  met.cadenceRpm = fabs(shaftRpm) / GEAR_RATIO;

  float omega = 2.0f * PI * (met.cadenceRpm / 60.0f);
  float alphaRaw = (dtSec > 0) ? (omega - lastOmega) / dtSec : 0.0f;
  gAlpha = 0.6f * gAlpha + 0.4f * alphaRaw;
  lastOmega = omega; gOmega = omega;

  met.speedKmh = (WHEEL_CIRC_M > 0.0f)
    ? (met.cadenceRpm * DRIVE_RATIO / 60.0f) * WHEEL_CIRC_M * 3.6f : 0.0f;

  if (sysState == ST_RUNNING) acc.totalPulses += (uint32_t)fabs((float)d);
  met.rotations = (float)acc.totalPulses / (countsPerRev * GEAR_RATIO);
}

// =============================================================================
// 12. BEBAN & TORSI (dari level servo, bukan sensor jarak)
// =============================================================================
void updateLoadAndTorque() {
  float torqueBase = TORQUE_BASE_NM + TORQUE_PER_LEVEL_NM * (met.level - 1);
  float cadCorr    = 1.0f + TORQUE_CADENCE_GAIN * (met.cadenceRpm - CADENCE_REF_RPM);
  cadCorr          = constrain(cadCorr, 0.3f, 3.0f);
  float loadTorque = (met.cadenceRpm > 1.0f) ? torqueBase * cadCorr : 0.0f;

  float inertiaTorque = flywheelInertia() * gAlpha;
  met.torqueNm = loadTorque + inertiaTorque;
  if (met.torqueNm < 0) met.torqueNm = 0;
  met.powerW = (met.cadenceRpm > 1.0f) ? met.torqueNm * gOmega : 0.0f;
}

// =============================================================================
// 13. SERVO LEVELING
// =============================================================================
float angleForLevel(int level) {
  level = constrain(level, 1, LEVEL_COUNT);
  return SERVO_MIN_DEG + (float)(level - 1) / (LEVEL_COUNT - 1) * (SERVO_MAX_DEG - SERVO_MIN_DEG);
}
int levelForAngle(float angle) {
  float frac = (angle - SERVO_MIN_DEG) / (SERVO_MAX_DEG - SERVO_MIN_DEG);
  int lv = (int)roundf(frac * (LEVEL_COUNT - 1)) + 1;
  return constrain(lv, 1, LEVEL_COUNT);
}
int gradeToLevel(float grade) {
  if (grade < 0) return 1;
  int lv = 1 + (int)roundf(grade / GRADE_PER_LEVEL);
  return constrain(lv, 1, LEVEL_COUNT);
}
void updateServo(float dtSec) {
  // Tentukan target level menurut mode
  if (g_mode == "sim") targetLevel = gradeToLevel(g_grade);
  // mode "manual": targetLevel diatur tombol/app
  // mode "cognitive": (menyusul) — sementara pertahankan targetLevel

  float targetAngle = angleForLevel(targetLevel);
  float step = SERVO_RATE_DEG_S * dtSec;          // batasi laju
  if (currentAngle < targetAngle) currentAngle = min(currentAngle + step, targetAngle);
  else if (currentAngle > targetAngle) currentAngle = max(currentAngle - step, targetAngle);
  currentAngle = constrain(currentAngle, SERVO_MIN_DEG, SERVO_MAX_DEG);

  servoLoad.write((int)roundf(currentAngle));
  met.servoAngle = currentAngle;
  met.level = levelForAngle(currentAngle);
}

// =============================================================================
// 14. SESSION MANAGEMENT
// =============================================================================
void resetSession() {
  sessionElapsed = 0; met.rotations = 0;
  acc = SessionAccum();
  encoder.clearCount(); lastEncCount = 0;
}
void accumulateSample() {
  acc.sampleCount++;
  acc.sumCadence += met.cadenceRpm;
  acc.sumTorque  += met.torqueNm;
  acc.sumPower   += met.powerW;
  acc.sumLevel   += met.level;
  if (met.cadenceRpm > acc.maxCadence) acc.maxCadence = met.cadenceRpm;
  if (met.torqueNm   > acc.maxTorque)  acc.maxTorque  = met.torqueNm;
  if (met.powerW     > acc.maxPower)   acc.maxPower   = met.powerW;
  if (met.level      > acc.maxLevel)   acc.maxLevel   = met.level;
  if (met.heartRate > 0) { acc.sumHR += met.heartRate; acc.hrCount++;
    if (met.heartRate > acc.maxHR) acc.maxHR = met.heartRate; }
  bool pedaling = (met.cadenceRpm > 1.0f);
  if (acc.wasPedaling && !pedaling) acc.stopCount++;
  acc.wasPedaling = pedaling;
}
SessionSummary buildSummary(uint8_t status) {
  SessionSummary s;
  s.sessionId = sessionId + 1; s.durationSec = sessionElapsed; s.status = status;
  s.stopCount = acc.stopCount;
  const float cpr = (float)ENCODER_PPR * (ENCODER_X4 ? 4.0f : 2.0f) * GEAR_RATIO;
  s.totalRotations = (float)acc.totalPulses / cpr;
  if (acc.sampleCount > 0) {
    s.avgCadence = acc.sumCadence / acc.sampleCount; s.maxCadence = acc.maxCadence;
    s.avgTorque  = acc.sumTorque  / acc.sampleCount; s.maxTorque  = acc.maxTorque;
    s.avgPower   = acc.sumPower   / acc.sampleCount; s.maxPower   = acc.maxPower;
    s.avgLevel   = acc.sumLevel   / acc.sampleCount; s.maxLevel   = acc.maxLevel;
  }
  if (acc.hrCount > 0) { s.avgHR = acc.sumHR / acc.hrCount; s.maxHR = acc.maxHR; }
  return s;
}
void saveSession(const SessionSummary &s) {
  sessionId = s.sessionId;
  Serial.printf("\n== SESI %u | %us | cad %.1f | pwr %.0fW | lvl %.1f | HR %.0f | %s ==\n",
    s.sessionId, s.durationSec, s.avgCadence, s.avgPower, s.avgLevel, s.avgHR,
    s.status==0?"Normal":s.status==1?"Stop":"E-Stop");
  if (!fs_ok) return;
  fs::File f = LittleFS.open(SESSION_FILE, "a");
  if (!f) return;
  if (f.size() == 0)
    f.println("id,durasi_s,cadence_avg,cadence_max,torque_avg,power_avg,power_max,level_avg,level_max,hr_avg,hr_max,putaran,stops,status");
  f.printf("%u,%u,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%.0f,%.0f,%.0f,%u,%u\n",
    s.sessionId, s.durationSec, s.avgCadence, s.maxCadence, s.avgTorque,
    s.avgPower, s.maxPower, s.avgLevel, s.maxLevel, s.avgHR, s.maxHR,
    s.totalRotations, s.stopCount, s.status);
  f.close();
}

// =============================================================================
// 15. JSON
// =============================================================================
String buildJson() {
  StaticJsonDocument<1280> doc;
  doc["state"]      = stateNames[sysState];
  doc["mode"]       = g_mode;
  doc["cadence"]    = met.cadenceRpm;
  doc["torque"]     = met.torqueNm;
  doc["power"]      = met.powerW;
  doc["speed"]      = met.speedKmh;
  doc["heartRate"]  = met.heartRate;
  doc["level"]      = met.level;
  doc["targetLevel"]= targetLevel;
  doc["servoAngle"] = met.servoAngle;
  doc["grade"]      = g_grade;
  doc["rotations"]  = met.rotations;
  doc["elapsed"]    = sessionElapsed;
  doc["targetMin"]  = targetDurationMin;
  doc["stops"]      = acc.stopCount;

  switch (sysState) {
    case ST_READY:   doc["stateLabel"] = "SIAP - START"; break;
    case ST_RUNNING: doc["stateLabel"] = "SESI BERJALAN"; break;
    case ST_PAUSED:  doc["stateLabel"] = "JEDA"; break;
    case ST_SUMMARY: doc["stateLabel"] = "HASIL SESI"; break;
    case ST_FAULT:   doc["stateLabel"] = "FAULT: " + faultMsg; break;
    default:         doc["stateLabel"] = stateNames[sysState]; break;
  }
  if (sysState == ST_SUMMARY) {
    JsonObject sum = doc.createNestedObject("summary");
    char dur[16]; snprintf(dur, sizeof(dur), "%u:%02u", lastSummary.durationSec/60, lastSummary.durationSec%60);
    sum["Durasi"]      = dur;
    sum["Cadence Avg"] = String(lastSummary.avgCadence,1) + " RPM";
    sum["Power Avg"]   = String(lastSummary.avgPower,1) + " W";
    sum["Torsi Avg"]   = String(lastSummary.avgTorque,1) + " Nm";
    sum["Level Avg"]   = String(lastSummary.avgLevel,1);
    sum["Level Max"]   = String(lastSummary.maxLevel);
    sum["HR Avg"]      = (lastSummary.avgHR>0)? String(lastSummary.avgHR,0)+" bpm" : "-";
    sum["Putaran"]     = String(lastSummary.totalRotations,0);
    sum["Berhenti"]    = String(lastSummary.stopCount) + " kali";
    const char* st[] = {"Selesai Normal","Dihentikan","Emergency"};
    sum["Status"] = st[lastSummary.status];
  }
  String out; serializeJson(doc, out); return out;
}

// =============================================================================
// 16. TFT
// =============================================================================
void tftBanner(const String &l1, const String &l2, uint16_t bg) {
  tft.fillRect(0, 0, tft.width(), 36, bg);
  tft.setTextColor(TFT_WHITE, bg); tft.setTextSize(1);
  tft.setCursor(6, 6); tft.print(l1);
  if (l2.length() > 0) { tft.setCursor(6, 20); tft.print(l2); }
}
void tftDrawState() {
  tft.fillScreen(TFT_BLACK);
  switch (sysState) {
    case ST_SELF_CHECK: tftBanner("SELF-CHECK...", "", TFT_NAVY); break;
    case ST_READY: {
      tftBanner("SIAP", "Kontrol via web/app", 0x2104);
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK); tft.setTextSize(1);
      tft.setCursor(6,46); tft.print("CADENCE"); tft.setCursor(6,86); tft.print("LEVEL");
      tft.setCursor(6,126); tft.print("SERVO"); tft.setCursor(170,46); tft.print("HR");
      tft.setCursor(170,86); tft.print("MODE");
      tft.setTextSize(2); tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(170,100); tft.print(g_mode);
      break;
    }
    case ST_RUNNING: {
      tftBanner("SESI BERJALAN", "", TFT_NAVY);
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK); tft.setTextSize(1);
      tft.setCursor(6,46); tft.print("CADENCE"); tft.setCursor(6,86); tft.print("POWER (W)");
      tft.setCursor(6,126); tft.print("LEVEL"); tft.setCursor(6,166); tft.print("HR (bpm)");
      tft.setCursor(170,46); tft.print("SERVO"); tft.setCursor(170,86); tft.print("WAKTU");
      break;
    }
    case ST_PAUSED: tftBanner("JEDA", "RESUME / STOP", 0x4208); break;
    case ST_SUMMARY: {
      tftBanner("HASIL SESI", "RESET via web", 0x2104);
      tft.setTextSize(1); int y=44;
      auto row=[&](const char* l,const String &v){ tft.setTextColor(TFT_DARKGREY,TFT_BLACK);tft.setCursor(6,y);tft.print(l);tft.setTextColor(TFT_WHITE,TFT_BLACK);tft.setCursor(150,y);tft.print(v);y+=15; };
      char dur[16]; snprintf(dur,sizeof(dur),"%u:%02u",lastSummary.durationSec/60,lastSummary.durationSec%60);
      row("Durasi",dur);
      row("Cadence Avg",String(lastSummary.avgCadence,1)+" RPM");
      row("Power Avg",String(lastSummary.avgPower,1)+" W");
      row("Level Avg",String(lastSummary.avgLevel,1));
      row("HR Avg",(lastSummary.avgHR>0)?String(lastSummary.avgHR,0)+" bpm":"-");
      row("Putaran",String(lastSummary.totalRotations,0));
      break;
    }
    case ST_FAULT: tftBanner("FAULT", faultMsg, TFT_MAROON); break;
    default: tftBanner("BOOT","Memulai...",TFT_NAVY); break;
  }
}
void tftDrawMetrics() {
  tft.setTextSize(2);
  auto field=[&](int x,int y,const String &v){ tft.fillRect(x,y,150,22,TFT_BLACK);tft.setTextColor(TFT_WHITE,TFT_BLACK);tft.setCursor(x,y);tft.print(v); };
  if (sysState == ST_RUNNING) {
    field(6,58,String(met.cadenceRpm,0));
    field(6,98,String(met.powerW,0));
    field(6,138,String(met.level));
    field(6,178,(met.heartRate>0)?String(met.heartRate,0):"--");
    field(170,58,String(met.servoAngle,0));
    char t[10]; snprintf(t,sizeof(t),"%02u:%02u",sessionElapsed/60,sessionElapsed%60);
    field(170,98,String(t));
    float prog=constrain((float)sessionElapsed/((float)targetDurationMin*60.0f),0.0f,1.0f);
    tft.fillRect(6,210,tft.width()-12,8,TFT_DARKGREY);
    tft.fillRect(6,210,(int)(prog*(tft.width()-12)),8,TFT_WHITE);
  } else if (sysState == ST_READY) {
    field(6,58,String(met.cadenceRpm,0));
    field(6,98,String(met.level));
    field(6,138,String(met.servoAngle,0));
    field(170,58,(met.heartRate>0)?String(met.heartRate,0):"--");
  }
}

// =============================================================================
// 17. HEART RATE TASK (core-1)
// =============================================================================
void hrTask(void *param) {
  for (;;) {
    pox.update();                 // wajib sesering mungkin
    float hr = pox.getHeartRate();
    g_heartRate = (hr > 30 && hr < 220) ? hr : 0;
    vTaskDelay(2 / portTICK_PERIOD_MS);
  }
}

// =============================================================================
// 18. WIFI (AP) & WEB SERVER
// =============================================================================
void setupWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);
  IPAddress ip = WiFi.softAPIP();
  if (ok) {
    Serial.printf("[WiFi] AP '%s' -> http://%s\n", AP_SSID, ip.toString().c_str());
    tftBanner("AP: " + String(AP_SSID), "http://" + ip.toString(), TFT_NAVY);
  } else tftBanner("WiFi GAGAL", "softAP error", TFT_MAROON);
  delay(800);
}

// Terima perintah dari aplikasi lewat WebSocket (JSON)
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    client->text(buildJson());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      StaticJsonDocument<256> in;
      if (deserializeJson(in, data, len) == DeserializationError::Ok) {
        String mode = in["mode"] | g_mode;
        bool hasGrade = in.containsKey("grade");
        bool hasLevel = in.containsKey("targetLevel");
        applyControl(mode, hasGrade, in["grade"] | 0.0f, hasLevel, in["targetLevel"] | targetLevel);
      }
    }
  }
}

void setupWebServer() {
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){ req->send_P(200,"text/html",INDEX_HTML); });
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *req){ req->send(200,"application/json",buildJson()); });

  // Kendali beban/mode dari aplikasi (alternatif WebSocket)
  server.on("/set", HTTP_GET, [](AsyncWebServerRequest *req){
    String mode = req->hasParam("mode") ? req->getParam("mode")->value() : g_mode;
    bool hasGrade = req->hasParam("grade");
    bool hasLevel = req->hasParam("level");
    float grade = hasGrade ? req->getParam("grade")->value().toFloat() : 0;
    int   level = hasLevel ? req->getParam("level")->value().toInt() : targetLevel;
    applyControl(mode, hasGrade, grade, hasLevel, level);
    req->send(200,"text/plain","OK");
  });

  server.on("/cmd", HTTP_GET, [](AsyncWebServerRequest *req){
    if (!req->hasParam("action")) { req->send(400,"text/plain","Missing action"); return; }
    String a = req->getParam("action")->value();
    if      (a=="start")   { doStartResume(); req->send(200,"text/plain","OK"); }
    else if (a=="pause")   { doPauseOrStop(); req->send(200,"text/plain","OK"); }
    else if (a=="stop")    { doStopFromRun(); req->send(200,"text/plain","OK"); }
    else if (a=="reset")   { doReset();       req->send(200,"text/plain","OK"); }
    else if (a=="durUp")   { durUp();         req->send(200,"text/plain","OK"); }
    else if (a=="durDown") { durDown();       req->send(200,"text/plain","OK"); }
    else if (a=="levelUp") { levelUp();       req->send(200,"text/plain","OK"); }
    else if (a=="levelDown"){ levelDown();    req->send(200,"text/plain","OK"); }
    else req->send(400,"text/plain","Unknown action");
  });

  server.on("/sessions", HTTP_GET, [](AsyncWebServerRequest *req){
    if (!fs_ok || !LittleFS.exists(SESSION_FILE)) { req->send(200,"text/csv","Belum ada sesi\n"); return; }
    req->send(LittleFS, SESSION_FILE, "text/csv");
  });
  server.on("/sessions/clear", HTTP_GET, [](AsyncWebServerRequest *req){
    if (fs_ok && LittleFS.exists(SESSION_FILE)) { LittleFS.remove(SESSION_FILE); sessionId=0; }
    req->send(200,"text/plain","cleared");
  });

  server.begin();
  Serial.println("[HTTP] aktif — / /data /cmd /set /sessions");
}
