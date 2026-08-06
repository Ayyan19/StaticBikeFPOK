# Static Bike Monitor (ESP32)

Firmware untuk memonitor sepeda statis (mis. Bodymax): menghitung **cadence, speed, torsi, level beban, power, energi, kalori,** dan **heart rate**, menampilkannya di **TFT LCD**, lalu menyiarkannya secara **wireless** lewat WiFi internal ESP32 ke perangkat lain (HP/laptop) melalui **web dashboard + WebSocket** real-time.

## Ringkasan arsitektur

```
                +-------------------- ESP32 --------------------+
 Rotary enc A/B | GPIO25/26 (interrupt, quadrature x4)          |
 (dekat pedal)  |    -> cadence (rpm) -> speed virtual          |
                |                                               |
 VL53L0X (I2C)  | GPIO21 SDA / GPIO22 SCL                       |
 (pulley depan) |    -> jarak magnet -> level beban -> torsi    |
                |                                               |----> TFT LCD (SPI)
 Heart rate     | GPIO27 (pulse) atau GPIO34 (analog)          |
 (bawaan bike)  |    -> BPM                                     |----> WiFi AP/STA
                |                                               |        http://<ip>/
                |  power = torsi x kecepatan sudut engkol       |        WebSocket /ws
                +-----------------------------------------------+
```

## File dalam proyek

| File | Isi |
|------|-----|
| `StaticBikeMonitor.ino` | Program utama: baca sensor, kalkulasi, TFT, web server. |
| `config.h` | **Semua** pin, konstanta kalibrasi, kredensial WiFi. Ubah di sini. |
| `index_html.h` | Dashboard live (PROGMEM) yang disajikan ESP32. |
| `HEART_RATE_REVERSE_ENGINEERING.md` | Panduan membaca sinyal HR bawaan Bodymax. |

## Library yang dibutuhkan

Instal via Arduino Library Manager (atur di `platformio.ini` bila pakai PlatformIO):

- **ESP32Encoder** — pembacaan encoder quadrature berbasis hardware PCNT.
- **Adafruit_VL53L0X** — sensor jarak ToF.
- **TFT_eSPI** (Bodmer) — driver TFT. **Wajib** mengatur `User_Setup.h`.
- **ESPAsyncWebServer** + **AsyncTCP** — web server & WebSocket async.

> Board di Boards Manager: paket **esp32 by Espressif Systems**. Pilih "ESP32 Dev Module".

## Wiring

### Encoder (quadrature A/B)
| Encoder | ESP32 |
|---------|-------|
| VCC | 3V3 (atau 5V bila encoder butuh 5V + level shifter untuk sinyal) |
| GND | GND |
| A (out A) | GPIO25 |
| B (out B) | GPIO26 |

Pasang di poros dekat pedal yang berputar bersama gear. Jika encoder di poros yang berputar **lebih cepat** dari engkol (via pulley), isi `GEAR_RATIO_ENC_PER_CRANK` di `config.h`.

> ⚠️ Banyak encoder industri (mis. LPD3806) beroperasi 5–24V dan output open-collector. Gunakan resistor pull-up ke 3V3 dan pastikan level sinyal aman untuk GPIO ESP32 (maks 3.3V). Bila output 5V, gunakan level shifter/pembagi tegangan.

### VL53L0X (I2C)
| VL53L0X | ESP32 |
|---------|-------|
| VIN | 3V3 |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |
| XSHUT | (opsional) set pin di `config.h` |

Arahkan sensor ke besi/target yang jaraknya berubah terhadap magnet beban saat level diputar. Rentang efektif VL53L0X ~30–1200 mm (paling stabil < 500 mm).

### TFT LCD (SPI) — contoh ILI9341
| TFT | ESP32 |
|-----|-------|
| VCC | 3V3 |
| GND | GND |
| CS | GPIO15 |
| DC/RS | GPIO2 |
| RST | GPIO4 |
| SDI/MOSI | GPIO23 |
| SCK | GPIO18 |
| LED | 3V3 |
| SDO/MISO | GPIO19 (opsional) |

Pin ini **didefinisikan di `User_Setup.h` milik TFT_eSPI**, bukan di `config.h`. Contoh isi:

```cpp
#define ILI9341_DRIVER
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4
#define SPI_FREQUENCY 40000000
```

### Heart rate
Lihat `HEART_RATE_REVERSE_ENGINEERING.md`. Setelah tahu tipe sinyal, set `HR_MODE` di `config.h`:
- **Pulse digital** → `HR_MODE_PULSE`, sambungkan ke **GPIO27**.
- **Analog** → `HR_MODE_ANALOG`, sambungkan ke **GPIO34** (input ADC1).

## Kalibrasi (langkah wajib)

1. **Encoder PPR** — isi `ENCODER_PPR` sesuai datasheet. Verifikasi: kayuh 10 putaran penuh, cek `encoder.getCount()` ≈ `PPR*4*10`.
2. **VL53L0X level** — buka Serial Monitor, baca `distMm`:
   - Putar beban ke **paling ringan**, catat jarak → `DIST_AT_MIN_LOAD_MM`.
   - Putar ke **paling berat**, catat jarak → `DIST_AT_MAX_LOAD_MM`.
   - Set `LEVEL_COUNT` sesuai jumlah level fisik sepeda.
3. **Speed** — sesuaikan `WHEEL_CIRCUMFERENCE_M` dan `DRIVE_RATIO_WHEEL_PER_CRANK` agar terasa realistis (cadence 60 rpm ≈ 25–30 km/j pada rasio umum).
4. **Torsi** — `TORQUE_BASE_NM`, `TORQUE_PER_LEVEL_NM`, `TORQUE_CADENCE_GAIN`. Untuk akurat, bandingkan dengan power meter komersial dan sesuaikan konstanta.

> Torsi di sini adalah **estimasi model** (rem magnetik: torsi ≈ fungsi jarak magnet + kecepatan), bukan pengukuran langsung. Untuk torsi sebenarnya butuh strain-gauge/load cell pada engkol.

## Cara upload

1. Buka `StaticBikeMonitor.ino` di Arduino IDE.
2. Instal semua library di atas + atur `User_Setup.h` TFT_eSPI.
3. Pilih board "ESP32 Dev Module", pilih port, klik Upload.
4. Buka Serial Monitor (115200) untuk melihat IP.

## Menghubungkan perangkat lain

- **Mode AP** (default): di HP/laptop, sambung ke WiFi **`BikeMonitor`** (pass `bike12345`), buka **`http://192.168.4.1`**.
- **Mode STA**: set `WIFI_MODE = WIFI_MODE_STA` + isi SSID/pass router, lalu buka IP yang tampil di Serial Monitor.
- Endpoint tambahan: **`GET /data`** mengembalikan JSON (untuk integrasi/aplikasi sendiri).

Contoh JSON:
```json
{"cadence":62.0,"speed":26.4,"torque":7.35,"level":4,"power":47.7,
 "hr":128,"energyKJ":12.34,"cal":12.7,"distMm":78,"elapsed":183}
```

## Rumus yang dipakai

- Cadence: `rpm = (Δcount / (PPR·4)) / Δt · 60 / GEAR_RATIO`
- Speed: `v = cadence · DRIVE_RATIO · keliling_roda`, dikonversi ke km/j
- Torsi: `τ = (TORQUE_BASE + TORQUE_PER_LEVEL·(level−1)) · (1 + gain·(rpm−rpm_ref))`
- Power: `P = τ · ω`, dengan `ω = 2π · rpm/60` (rad/s) → **Watt**
- Energi: `E = ∫P dt` (kJ); Kalori metabolik ≈ `(E/4.184)/0.24`

## Catatan keselamatan & hukum

Reverse engineering sinyal HR dilakukan pada perangkat milik sendiri untuk interoperabilitas. Jangan pernah menyambungkan elektronik ke bagian yang terhubung listrik jala-jala; sepeda statis magnetik umumnya pasif/baterai sehingga aman diprobe pada tegangan rendah. Selalu ukur tegangan sebelum menyambung ke ESP32 (maks 3.3V per GPIO).
