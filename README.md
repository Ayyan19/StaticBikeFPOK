# Rencana Pengembangan — Sepeda Statis FPOK
### Fase: Adaptasi Beban Otomatis (Stepper) + Simulasi Aplikasi + Heart Rate

Dokumen pegangan tim. Mencerminkan keputusan terbaru (aktuator **stepper**, MAX30100, protokol dua-arah).

---

## 1. Kondisi Proyek Saat Ini

- Platform: **ESP32 tunggal (1 MCU)**, WiFi sebagai **Access Point (AP) kontinu**.
- Kontrol lewat **web dashboard** + **aplikasi Android sendiri** (WiFi/JSON, offline).
- Sudah berjalan: **cadence** (rotary encoder 1:1), **torsi/power/speed**, **TFT**, **logging sesi** (LittleFS CSV), **heart rate** (MAX30100), dan **kendali beban via stepper**.
- Komunikasi **dua arah** aktif: unit kirim telemetri + terima perintah dari aplikasi.

---

## 2. Keputusan yang Dikunci

| No | Item | Nilai |
|---|---|---|
| 1 | Sensor beban VL53L0X | **Dihapus** |
| 2 | Aktuator beban | **Motor stepper NEMA23 5718HB3401** (~3,4 A) penarik seling |
| 3 | Driver stepper | **TB6600** atau **DM542** (wajib; A4988/DRV8825 tidak cukup) |
| 4 | Jumlah level beban | **8 level** |
| 5 | Heart rate | **MAX30100** (I2C) |
| 6 | MCU | **1 MCU (ESP32)** |
| 7 | Koneksi aplikasi | **WiFi AP**, aplikasi **buatan sendiri** (WiFi/JSON, offline) |
| 8 | Flywheel / puli depan | **6 kg**, diameter **25 cm** (r = 0,125 m); puli encoder sama diameter → rasio **1:1** |

---

## 3. Peta Pin (FINAL) — tidak ada konflik

Tetap (tidak diubah): Encoder A/B = GPIO 25/26 · TFT SPI (User_Setup.h) = CS15, DC2, RST4, MOSI23, SCK18, MISO19.

| Komponen | Pin ESP32 | Catatan |
|---|---|---|
| VL53L0X | — | Dihapus |
| **Stepper STEP / PUL+** | **GPIO 13** | ke driver |
| **Stepper DIR+** | **GPIO 27** | ke driver |
| **Stepper ENA+** | **GPIO 14** | aktif LOW |
| Driver PUL− / DIR− / ENA− | **GND ESP32** | common ground |
| Motor (A+/A−/B+/B−) | terminal driver | catu driver 12–24 V terpisah |
| **Limit switch homing** | **GPIO 32** + GND | opsional (`USE_HOMING`) |
| **MAX30100 SDA / SCL** | **GPIO 21 / 22** | bus I2C bekas VLX |
| MAX30100 VIN / GND | 3V3 / GND | logika 3,3 V |

**Wajib:** semua GND disatukan (ESP32, driver, catu daya, MAX30100). GPIO 12 sengaja tidak dipakai (strapping).

---

## 4. Komunikasi Dua Arah (WiFi AP)

Alamat unit: **192.168.4.1**. Satu koneksi, dua aliran.

**Unit → Aplikasi (telemetri, ~2×/detik)** via `/ws` atau `/data`:

| Field | Satuan |
|---|---|
| cadence | rpm |
| power | watt |
| torque | Nm (est) |
| speed | km/j (virtual) |
| heartRate | bpm |
| level | 1–8 (aktual) |
| actuatorPos | langkah stepper |
| mode | sim/manual/cognitive |
| grade | % |
| state, elapsed, stops | — |

**Aplikasi → Unit (kendali)** — field `level` diseragamkan di kedua jalur:

| Field | Fungsi |
|---|---|
| mode | `sim` / `manual` / `cognitive` |
| grade | kemiringan % (mode sim) |
| level | set level 1–8 (mode manual) |

Contoh (WebSocket ke `ws://192.168.4.1/ws`):
```json
{"mode":"sim","grade":8}
{"mode":"manual","level":6}
```
Alternatif HTTP: `GET /set?mode=sim&grade=8` atau `/set?mode=manual&level=6`.
Kontrol sesi: `GET /cmd?action=start|pause|stop|reset|levelUp|levelDown`.

---

## 5. Mekanisme Leveling (Stepper)

- Level 1–8 → **posisi langkah** stepper. `LEVEL_TRAVEL_STEPS` = total langkah level 1→8 (placeholder 4000, dikalibrasi).
- Gerakan halus otomatis lewat **akselerasi AccelStepper** (`STEPPER_MAX_SPEED`, `STEPPER_ACCEL`).
- **Homing** (opsional, disarankan): saat menyala, stepper mencari limit switch (GPIO 32) → titik nol = level 1. Bila `USE_HOMING 0`, aktuator harus diset ke level 1 sebelum power-on (stepper tak tahu posisi awal).
- Aturan **grade → level** (mode sim, dapat diubah): `level = 1 + round(grade / 2)`, dibatasi 1–8; grade negatif → level 1.

---

## 6. Pemilih Mode

- **`sim`** — beban ikut `grade` dari aplikasi (fokus sekarang).
- **`manual`** — operator set `level`.
- **`cognitive`** — (menyusul) otomatis dari variabilitas kadens sesuai paten.

Servo/stepper hanya digerakkan satu sumber sesuai mode aktif.

---

## 7. Heart Rate (MAX30100)

- Dibaca di **task core-1** ESP32 agar tak terganggu WiFi; hasil `heartRate` masuk telemetri.
- Klip di jari/telinga yang diam; kelas kebugaran, **bukan diagnosis**.

---

## 8. Library yang Perlu Di-install

`AccelStepper` · `MAX30100lib` · `ESPAsyncWebServer` (ESP32Async) + `Async TCP` (ESP32Async) · `ESP32Encoder` · `TFT_eSPI` · `ArduinoJson`.

---

## 9. Kalibrasi yang Masih Perlu

- **LEVEL_TRAVEL_STEPS**: hitung berapa langkah stepper untuk menarik seling dari beban paling ringan (level 1) ke paling berat (level 8).
- **Arah stepper** (`HOME_DIR`) dan setelan **driver** (arus ≈ 3–3,4 A, microstep).
- Konfirmasi bentuk puli untuk inersia (`FLYWHEEL_SHAPE`: 0 = cakram, 1 = tepi).

---

## 10. Catatan Sisi Aplikasi Android (WiFi AP)

- Selama tersambung ke AP unit, HP **tanpa internet** — aplikasi berjalan offline.
- **Wajib**: ikat koneksi aplikasi ke jaringan WiFi unit (`ConnectivityManager.requestNetwork` + `bindProcessToNetwork`) agar permintaan tidak lari ke jaringan seluler.

---

## 11. Roadmap

- **Fase A (sekarang):** stepper leveling + mode sim/manual + MAX30100 + protokol dua-arah.
- **Fase B:** kalibrasi Level→langkah presisi; homing dengan limit switch; penguatan mekanik.
- **Fase C:** mode `cognitive` (paten) — sampling encoder dipercepat ≥50 Hz untuk variabilitas kadens.

> Catatan hukum (berkas paten): jangan publikasikan data uji / dokumen invensi sebelum tanggal penerimaan permohonan paten.
