// =============================================================================
//  config.h  --  Konfigurasi terpusat untuk Static Bike Monitor (ESP32)
//  Semua pin, konstanta kalibrasi, dan parameter jaringan ada di sini.
//  Ubah nilai di file ini saja saat merakit / mengkalibrasi.
// =============================================================================
#ifndef CONFIG_H
#define CONFIG_H

// -----------------------------------------------------------------------------
// 1. WIFI  --  ESP32 dijalankan sebagai Access Point ATAU menyambung ke router.
//    WIFI_MODE_AP  = HP/laptop menyambung langsung ke ESP32 (tanpa router).
//    WIFI_MODE_STA = ESP32 menyambung ke WiFi rumah, buka lewat IP lokal.
// -----------------------------------------------------------------------------
#define WIFI_MODE_AP    0
#define WIFI_MODE_STA   1
#define WIFI_MODE       WIFI_MODE_AP 

// Dipakai saat WIFI_MODE_AP (ESP32 membuat hotspot sendiri)
static const char* AP_SSID     = "Projectstatis";
static const char* AP_PASSWORD = "bike12345";   // minimal 8 karakter

// Dipakai saat WIFI_MODE_STA (menyambung ke router)
static const char* STA_SSID     = "BikeFPOK";
static const char* STA_PASSWORD = "BikeFPOK";

// -----------------------------------------------------------------------------
// 2. PIN ENCODER (Rotary quadrature A/B)  --  dekat pedal, terhubung ke gear.
//    Gunakan pin yang mendukung interrupt (hampir semua GPIO ESP32 bisa).
//    Hindari GPIO 6-11 (dipakai flash) dan GPIO 34-39 (input-only, tanpa pull-up).
// -----------------------------------------------------------------------------
#define ENCODER_PIN_A   25
#define ENCODER_PIN_B   26

// PPR = Pulse Per Revolution dari datasheet encoder (1 kanal).
// Untuk quadrature, resolusi efektif = PPR * 4 (mode x4).
#define ENCODER_PPR     600      // ganti sesuai encoder Anda (mis. 360 / 600 / 1000)
#define ENCODER_X4      1        // 1 = pakai dekoding x4 (paling presisi)

// Arah putaran: set -1 bila hasil RPM negatif saat dikayuh maju.
#define ENCODER_DIRECTION  1

// -----------------------------------------------------------------------------
// 3. VL53L0X (Time-of-Flight)  --  di pulley depan, mengukur jarak magnet beban.
//    Jarak (mm) berubah saat level beban berubah -> dipetakan ke level 1..N.
//    I2C default ESP32: SDA=21, SCL=22.
// -----------------------------------------------------------------------------
#define I2C_SDA_PIN     21
#define I2C_SCL_PIN     22
#define VL53_XSHUT_PIN  -1       // -1 bila XSHUT tidak dipakai

// Kalibrasi jarak->level. Ukur jarak (mm) pada beban PALING RINGAN dan PALING BERAT.
// Jika magnet makin DEKAT saat beban makin BERAT, DIST_AT_MIN_LOAD > DIST_AT_MAX_LOAD.
#define DIST_AT_MIN_LOAD_MM   120.0f   // jarak (mm) saat resistansi paling ringan
#define DIST_AT_MAX_LOAD_MM    30.0f   // jarak (mm) saat resistansi paling berat
#define LEVEL_COUNT            8        // jumlah level beban (mis. 1..8)

// -----------------------------------------------------------------------------
// 4. HEART RATE  --  sinyal dari sensor bawaan Bodymax (reverse engineering).
//    Karena tipe sinyal belum dipastikan, disediakan DUA mode. Pilih salah satu
//    setelah probing (lihat HEART_RATE_REVERSE_ENGINEERING.md).
//      HR_MODE_PULSE  = tiap detak menghasilkan 1 pulsa digital (dibaca interrupt)
//      HR_MODE_ANALOG = gelombang analog, dibaca ADC + deteksi puncak (threshold)
// -----------------------------------------------------------------------------
#define HR_MODE_PULSE   0
#define HR_MODE_ANALOG  1
#define HR_MODE         HR_MODE_PULSE

#define HR_PULSE_PIN    27       // dipakai saat HR_MODE_PULSE (input digital)
#define HR_ANALOG_PIN   34       // dipakai saat HR_MODE_ANALOG (ADC1, input-only OK)
#define HR_ANALOG_THRESHOLD 2200 // ambang ADC (0-4095) untuk deteksi puncak; kalibrasi
#define HR_MIN_BPM      40       // abaikan interval yang menghasilkan BPM di luar
#define HR_MAX_BPM      220      //   rentang wajar ini (buang noise)
#define HR_TIMEOUT_MS   4000     // bila tak ada detak sekian ms, BPM dianggap 0

// -----------------------------------------------------------------------------
// 5. TFT LCD  --  konfigurasi PIN sebenarnya diatur di User_Setup.h milik
//    library TFT_eSPI (lihat README). Di sini hanya opsi tampilan.
// -----------------------------------------------------------------------------
#define TFT_ROTATION    1        // 0..3 orientasi layar
#define TFT_REFRESH_MS  200      // periode refresh layar (ms)

// -----------------------------------------------------------------------------
// 6. PARAMETER MEKANIS / FISIK  --  untuk konversi ke torsi, speed, dan power.
// -----------------------------------------------------------------------------
// Rasio gear: berapa putaran poros-encoder per 1 putaran engkol pedal.
// Jika encoder dipasang langsung di poros pedal, GEAR_RATIO = 1.0.
// Jika encoder di poros yang berputar lebih cepat (via gigi/pulley), isi rasionya.
#define GEAR_RATIO_ENC_PER_CRANK   1.0f

// Keliling "roda virtual" (meter) untuk menghitung kecepatan simulasi.
// Sepeda 700c ~ 2.105 m. Sesuaikan agar terasa realistis.
#define WHEEL_CIRCUMFERENCE_M      2.105f

// Rasio transmisi virtual: berapa putaran roda per 1 putaran engkol pedal.
// Sepeda umum ~ 3.0 (chainring/cog). Naikkan untuk speed lebih tinggi per cadence.
#define DRIVE_RATIO_WHEEL_PER_CRANK 3.0f

// -----------------------------------------------------------------------------
// 7. KURVA TORSI vs LEVEL BEBAN  --  estimasi torsi kayuhan (Nm) di tiap level.
//    Rem magnetik: torsi ~ fungsi kekuatan medan (jarak magnet) DAN kecepatan.
//    Model sederhana:  torque = BASE + K_LEVEL*(level-1), lalu dikoreksi cadence.
//    Kalibrasikan dengan alat ukur (crank power meter) bila ingin akurat.
// -----------------------------------------------------------------------------
#define TORQUE_BASE_NM         3.0f    // torsi minimum (level 1) pada cadence acuan
#define TORQUE_PER_LEVEL_NM    2.2f    // tambahan torsi per naik 1 level
#define CADENCE_REF_RPM        60.0f   // cadence acuan untuk kurva torsi
// Rem magnetik: torsi naik seiring kecepatan. Faktor koreksi linier terhadap cadence.
#define TORQUE_CADENCE_GAIN    0.010f  // per (rpm - CADENCE_REF): +1% torsi tiap rpm

// -----------------------------------------------------------------------------
// 8. TIMING SAMPLING
// -----------------------------------------------------------------------------
#define SAMPLE_WINDOW_MS   250   // jendela hitung RPM (250ms = 4 update/detik)
#define WS_PUSH_MS         500   // periode kirim data ke dashboard via WebSocket
#define RIDER_WEIGHT_KG    75.0f // untuk estimasi kalori (opsional)

#endif 
