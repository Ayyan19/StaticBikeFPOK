# Reverse Engineering Sinyal Heart Rate Bodymax

Tujuan: mengetahui bentuk sinyal detak jantung dari sensor bawaan sepeda (grip/hand-pulse atau ear-clip) agar bisa dibaca ESP32. Kita **tidak** membongkar firmware apa pun — cukup mengukur sinyal listrik pada konektor sensor.

## Langkah 1 — Kenali jenis sensor HR

Kebanyakan sepeda fitness memakai salah satu dari:

1. **Hand-grip (contact) sensor** — dua pelat logam di setang. Menghasilkan sinyal EKG lemah yang diperkuat papan internal. Output ke konsol biasanya berupa **pulsa digital** (satu pulsa per detak) atau kadang gelombang analog.
2. **Ear-clip / finger photoplethysmograph (PPG)** — LED + fotodioda, jack **3.5 mm mono/stereo**. Output analog kecil, sering sudah dikondisikan menjadi pulsa.
3. **Penerima chest-strap 5 kHz (Polar) / ANT+ / BLE** — bila begini, tidak ada kabel untuk diprobe; lebih baik ESP32 langsung menerima BLE HRM (opsi terpisah).

Cari kabel/konektor antara sensor dan konsol. Jack 3.5 mm hampir selalu menandakan tipe (1) atau (2).

## Langkah 2 — Alat yang dibutuhkan

- Multimeter (wajib).
- Osiloskop **atau** logic analyzer murah (sangat membantu). Alternatif: ESP32 sendiri sebagai perekam ADC (skrip di bawah).
- Kabel jumper, buaya kecil.

## Langkah 3 — Petakan pin konektor

Pada jack 3.5 mm: **Tip – Ring – Sleeve**. Biasanya Sleeve = GND.

1. Ukur tegangan DC tiap pin terhadap GND saat perangkat menyala **tanpa** memegang sensor. Catat idle level (mis. 0V, 1.5V, 3.3V).
2. Pegang sensor / jepit telinga. Amati pin mana yang **berubah/berdenyut** seiring detak. Itu pin sinyal.

> ⚠️ Pastikan tegangan sinyal ≤ 3.3V sebelum menyambung ke GPIO ESP32. Jika 5V, pakai pembagi tegangan (mis. 10k/20k) atau level shifter. Jangan sambungkan pin bertegangan > 3.3V langsung.

## Langkah 4 — Tentukan digital vs analog

- **Digital (pulse)**: sinyal melompat antara dua level (mis. 0V↔3.3V), satu lonjakan per detak, tepi tajam. → set `HR_MODE = HR_MODE_PULSE`, sambung ke **GPIO27**.
- **Analog**: gelombang kecil yang naik-turun mulus mengikuti denyut. → set `HR_MODE = HR_MODE_ANALOG`, sambung ke **GPIO34**, dan kalibrasi `HR_ANALOG_THRESHOLD`.

## Langkah 5 — Perekam ADC pakai ESP32 (bila tak ada osiloskop)

Upload sketch kecil ini sementara untuk "melihat" sinyal lewat Serial Plotter:

```cpp
// probe_hr.ino -- rekam sinyal HR ke Serial Plotter (Tools > Serial Plotter)
#define HR_PIN 34            // sambungkan pin sinyal sensor ke GPIO34 + GND bersama
void setup(){ Serial.begin(115200); analogReadResolution(12); }
void loop(){
  Serial.println(analogRead(HR_PIN));   // 0..4095
  delay(5);                             // ~200 Hz sampling
}
```

Buka **Serial Plotter**. Pegang sensor:
- Jika terlihat **gelombang periodik** mengikuti denyut → sinyal **analog**. Catat nilai puncak & lembah untuk menentukan `HR_ANALOG_THRESHOLD` (di antara keduanya).
- Jika terlihat **kotak tegas 0/4095** → sebenarnya **digital**; pindah ke `HR_MODE_PULSE`.

Hitung BPM manual untuk verifikasi: jumlah puncak dalam 15 detik × 4.

## Langkah 6 — Set config & uji

Di `config.h`:
- Pilih `HR_MODE`.
- Pulse: hanya sambungkan pin ke `HR_PULSE_PIN` (GPIO27). Interrupt menghitung interval antar detak → BPM.
- Analog: set `HR_ANALOG_PIN` (GPIO34) dan `HR_ANALOG_THRESHOLD` di antara puncak & lembah. Firmware mendeteksi puncak dengan histeresis + refractory period.

Firmware sudah menyaring BPM di luar `HR_MIN_BPM..HR_MAX_BPM` dan memberi `HR_TIMEOUT_MS` (BPM → 0 bila tak ada detak).

## Alternatif termudah: BLE Heart Rate

Jika sepeda hanya mengirim via chest strap BLE, lupakan probing kabel. ESP32 bisa jadi **BLE client** ke standar *Heart Rate Service* (UUID `0x180D`, karakteristik `0x2A37`). Beri tahu saya bila mau versi ini — saya siapkan modul BLE terpisah yang tinggal menggantikan `updateHeartRate()`.

## Ringkasan keputusan

| Yang Anda lihat | Mode | Pin | Aksi |
|-----------------|------|-----|------|
| Pulsa 0↔3.3V, 1/detak | `HR_MODE_PULSE` | GPIO27 | Langsung jalan |
| Gelombang analog mulus | `HR_MODE_ANALOG` | GPIO34 | Set threshold |
| >3.3V | (mana pun) | + pembagi tegangan | Turunkan dulu |
| Hanya BLE/ANT+ | modul BLE | — | Minta versi BLE |
