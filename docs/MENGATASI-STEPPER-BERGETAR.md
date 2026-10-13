# Mengatasi Stepper Bergetar (TB6600 + NEMA 17)

Motor stepper **bergetar / berdengung / berdebar tapi tidak berputar** — panduan ini
mengurutkan penyebabnya dari yang paling sering, dan mana yang bisa diperbaiki
dari software vs harus diperbaiki dari wiring.

---

## Ringkasan cepat

| Prioritas | Penyebab | Diperbaiki dari mana |
|---|---|---|
| **1** | **Level tegangan sinyal kurang (3.3V vs 5V)** | **Wiring** — ini penyebab paling sering |
| 2 | Kecepatan terlalu tinggi (stall) | Firmware — sudah diperbaiki (ramp + 600 step/s) |
| 3 | Arus TB6600 terlalu rendah | DIP switch TB6600 |
| 4 | Microstep tidak cocok | DIP switch TB6600 |
| 5 | Urutan kabel motor salah | Wiring motor |

---

## Penyebab #1 — Level tegangan sinyal (PALING SERING)

### Masalahnya

Input TB6600 (`PUL+`, `DIR+`, `ENA+`) adalah **LED optocoupler dengan resistor seri
yang dirancang untuk 5V**.

ESP32 hanya mengeluarkan **3.3V**. Di 3.3V, arus ke LED turun sekitar 35–40%.
Akibatnya LED kadang nyala, kadang tidak — tergantung suhu dan toleransi komponen.
Motor menerima pulsa yang kacau → **bergetar**.

### Wiring yang SALAH (3.3V)

```
GPIO 3.3V ------ PUL+       <- terlalu lemah, TB6600 butuh 5V
GND       ------ PUL-         (common cathode / sinking)
```

Ini yang terjadi kalau `PUL-`, `DIR-`, `ENA-` semua masuk ke GND dan GPIO masuk ke `+`.

### Wiring yang BENAR (5V)

```
5V        ------ PUL+       <- dari sumber 5V terpisah
GPIO 18   ------ PUL-         (common anode / sinking)
5V        ------ DIR+
GPIO 19   ------ DIR-
5V        ------ ENA+
GPIO 23   ------ ENA-
GND ESP32 ------ GND TB6600  <- COMMON GROUND WAJIB
```

Di rangkaian ini, **GPIO menarik ke GND** (active-LOW) untuk menyalakan LED.
Karena GPIO hanya jadi jalur ke ground, tidak masalah tegangannya 3.3V.

### Cara mendapat 5V

Karena PSU kamu **9–42 VDC** (tidak ada output 5V), pakai salah satu:

**a. Buck converter** (paling disarankan)
- Input: 9–42V (dari PSU yang sama)
- Output: 5V
- Hubungkan 5V ke `PUL+`, `DIR+`, `ENA+` kedua driver
- Murah, efisien, dan bisa menyalurkan arus untuk dua TB6600

**b. Pin 5V dari ESP32** (kalau USB tersambung)
- ESP32 DevKit punya pin 5V (VIN/5V) dari USB
- **Batas:** arus dari USB terbatas (~500 mA laptop, ~1A charger)
- Satu TB6600 butuh ~10–15 mA per input × 3 input = ~45 mA
- Dua driver = ~90 mA — masih aman dari USB charger
- **Tapi** jangan pakai kalau USB dari laptop yang lemah

**c. Modul 5V terpisah** — apa saja yang keluarannya stabil 5V.

> **Jangan** pakai pin 3.3V ESP32. Sama masalahnya.

### Kalau tetap mau coba 3.3V (common cathode)

Bisa dicoba, tapi **tidak dijamin**. Yang bisa dilakukan:
- Turunkan kecepatan ke `K300` (perintah baru — lihat bawah)
- Kalau masih getar setelah itu, penyebabnya pasti level tegangan, bukan kecepatan

---

## Penyebab #2 — Kecepatan terlalu tinggi (sudah diperbaiki di firmware)

Motor stepper **tidak bisa langsung digerakkan dari 0 ke kecepatan tinggi**. Kalau
dipaksa, rotor kehilangan sinkronisasi dengan medan magnet → motor hanya bergetar.

Firmware sebelumnya langsung memulai di 1600 step/detik. Sekarang sudah diperbaiki:

| Perbaikan | Nilai |
|---|---|
| Kecepatan default | 1600 → **600** step/detik |
| **Ramp akselerasi** | **baru** — kecepatan naik bertahap dari 0 |
| Ramp deselerasi | **baru** — melambat mendekati tujuan |
| Lebar pulsa minimum | **10 µs** (TB6600 butuh min 2.2 µs) |
| Jeda DIR stabil | 5 µs sebelum pulsa pertama |

### Tuning kecepatan tanpa upload ulang

Sekarang ada perintah **`K`** untuk mengubah kecepatan langsung:

```powershell
# Lambat & paling aman untuk diagnosis getar
Invoke-WebRequest "http://192.168.4.1/cmd?val=K300"

# Sedang
Invoke-WebRequest "http://192.168.4.1/cmd?val=K800"

# Cepat (butuh sinyal 5V yang benar)
Invoke-WebRequest "http://192.168.4.1/cmd?val=K1500"
```

Lewat serial juga bisa — ketik `K300` lalu Enter.

**Panduan tuning:** mulai dari `K300`. Kalau berputar halus, naikkan bertahap
(`K500`, `K800`, `K1000`, ...). Kecepatan tertinggi yang masih halus = batas sistem kamu.
Kalau mulai getar lagi, turunkan satu tingkat.

---

## Penyebab #3 — Arus TB6600 terlalu rendah

NEMA 17 umumnya butuh **1.5–1.7 A**. Kalau DIP switch TB6600 di-set lebih rendah,
torsi tidak cukup dan motor mudah stall (getar).

**Setelan DIP switch arus TB6600** (pada umumnya — cek label modul kamu):

| SW1 | SW2 | SW3 | Arus |
|---|---|---|---|
| ON | ON | ON | 0.5 A |
| ON | OFF | ON | 1.0 A |
| ON | ON | OFF | 1.5 A |
| ON | OFF | OFF | **2.0 A** ← baik untuk NEMA 17 |

Coba **1.5 A atau 2.0 A**.

---

## Penyebab #4 — Microstep tidak cocok

Firmware mengasumsikan **microstep 16 → 3200 step/rev**. Kalau DIP switch kamu
berbeda, sudut jadi tidak akurat.

**Setelan microstep TB6600** (umumnya):

| SW4 | SW5 | SW6 | Microstep | Step/rev |
|---|---|---|---|---|
| ON | ON | ON | 1 | 200 |
| ON | OFF | ON | 2 | 400 |
| ON | ON | OFF | 4 | 800 |
| ON | OFF | OFF | 8 | 1600 |
| OFF | ON | ON | **16** | **3200** ← yang dipakai firmware |

Kalau microstep kamu bukan 16, ubah `STEPS_PER_REV` di firmware dan upload ulang.

---

## Penyebab #5 — Urutan kabel motor salah

Motor NEMA 17 punya 4 kabel (A+, A−, B+, B−). Kalau pasangan-nya tertukar,
motor hanya akan **bergetar tanpa berputar sama sekali**.

Cek dengan multimeter:
- Kabel dalam **satu pasangan** (A+ & A−) punya **kontinuitas** (ada hambatan ~1–5 Ω)
- **Antara dua pasangan** tidak ada kontinuitas (terbuka)

Kalau tidak jelas mana pasangan mana, cari datasheet motor kamu berdasarkan
kode di labelnya. Urutan umum: **Merah-Biru = A, Hijau-Hitam = B**.

---

## Cara diagnosis cepat

Lakukan berurutan:

**1. Cek LED di TB6600**
- Ada LED power menyala? → PSU OK
- Ada LED sinyal berkedip saat kirim perintah? → sinyal masuk
- LED redup / tidak nyala → **level tegangan kurang** (penyebab #1)

**2. Turunkan kecepatan ke `K300`**
```powershell
Invoke-WebRequest "http://192.168.4.1/cmd?val=K300"
Invoke-WebRequest "http://192.168.4.1/cmd?val=C"
```
- Berputar halus? → penyebabnya kecepatan (#2), naikkan bertahap
- Masih getar? → **bukan kecepatan**, lanjut ke langkah 3

**3. Naikkan arus TB6600 ke 2.0 A**, coba lagi.

**4. Perbaiki level tegangan ke 5V** (wiring common anode) — ini yang paling
sering menyelesaikan masalah kalau langkah 1–3 belum berhasil.

**5. Cek urutan kabel motor** dengan multimeter.

---

## Ringkas: apa yang harus dilakukan sekarang

Karena wiring kamu saat ini **GPIO ke `X+` dengan 3.3V** (dan `X-` ke GND):

1. **Coba dulu:** upload firmware baru sudah dilakukan. Set `K300` lalu tes.
2. **Kalau masih getar:** kamu perlu **5V untuk sisi `X+`**. Pilih salah satu:
   - Buck converter dari PSU 9–42V → 5V (disarankan)
   - Pin 5V ESP32 (kalau pakai USB charger yang cukup)
3. Setelah pindah ke 5V, wiring jadi **common anode**:
   `X+` ke 5V, `X-` ke GPIO. Ini **membalik logika** — bilang saya, nanti
   saya ubah firmware jadi active-LOW.
4. Naikkan arus TB6600 ke **2.0 A**.
5. Pastikan **microstep = 16** (SW4 OFF, SW5 ON, SW6 ON).

---

## Catatan penting soal perubahan logika

Firmware sekarang **active-HIGH** (dirancang untuk `X+` = GPIO, `X-` = GND).

Kalau kamu pindah ke wiring 5V common-anode (`X+` = 5V, `X-` = GPIO),
logikanya **terbalik** — bilang saya, saya ubah:

```cpp
// Dari active-HIGH (sekarang)
digitalWrite(s.pinPul, HIGH);   // pulsa

// Menjadi active-LOW (untuk common anode 5V)
digitalWrite(s.pinPul, LOW);    // pulsa
```

Jangan coba pakai wiring 5V common-anode dengan firmware active-HIGH sekarang —
motor akan diam total atau bergerak kebalikan.
