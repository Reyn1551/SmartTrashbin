# Memperbaiki Stepper Tersendat-sendat (TB6600 + ESP32)

**Gejala:** motor berputar, tapi tersendat-sendat. Kadang seperti mau
berhenti lalu jalan lagi. Tidak mulus sama sekali.

**Penyebab:** sinyal dari ESP32 terlalu lemah untuk optocoupler TB6600.

**Solusi:** ubah wiring ke common-anode 5V. Firmware sudah disiapkan
untuk mode ini (aktif-LOW).

---

## Kenapa ini terjadi?

TB6600 **tidak menerima sinyal tegangan** — di dalamnya ada
**optocoupler** (LED + fototransistor). Yang dibaca TB6600 adalah
**arus yang mengalir melewati LED** itu, bukan tegangannya.

Datasheet TB6600: arus ideal **10–15 mA**.

### Wiring lama (common-cathode 3.3V) — PENYEBAB GETAR

```
PUL- ─────► GND
PUL+ ◄───── GPIO18   (3.3V)
```

Arus yang mengalir:

```
I = (3.3V − 1.2V) / 220Ω = 9.5 mA
```

Dua masalah:
1. **9.5 mA < 10 mA** minimum — sudah kurang dari kebutuhan
2. GPIO ESP32 **tidak sanggup** memasok arus itu dengan stabil; tegangan
   keluarannya turun di bawah 3.3V, sehingga arus jadi lebih kecil lagi

Akibatnya LED di dalam optocoupler **kadang menyala, kadang tidak**.
Setiap kali tidak menyala, TB6600 **melewatkan satu pulsa** → motor
tersendat.

### Wiring benar (common-anode 5V)

```
PUL+ ─────► 5V  (dari buck converter)
PUL- ◄───── GPIO18  →  tarik ke LOW
GND buck ──┬── GND ESP32   ← WAJIB, jangan lupa
```

Arus yang mengalir:

```
I = (5V − 1.2V) / 220Ω = 17 mA
```

Tapi sekarang **arus datang dari rel 5V**, bukan dari pin ESP32. Pin
GPIO hanya bertugas **menarik ke bawah** (sink), dan itu bisa dilakukan
dengan sangat baik. Optocoupler dapat arus penuh dan stabil → **setiap
pulsa terbaca** → motor mulus.

---

## Cara mengubahnya

### Bahan

- Buck converter yang sudah disetel ke **5V** (kamu sudah punya)
- Kabel secukupnya

### Langkah

**1. Putus sambungan lama**

Lepas semua kabel dari `PUL-`, `DIR-`, `ENA-` yang menuju GND.

**2. Sambungkan sisi `+` ke 5V**

```
TB6600 STPR_1          Buck 5V
  PUL+  ─────────────► +5V
  DIR+  ─────────────► +5V
  ENA+  ─────────────► +5V
```

```
TB6600 STPR_2          Buck 5V
  PUL+  ─────────────► +5V
  DIR+  ─────────────► +5V
  ENA+  ─────────────► +5V
```

**3. Sambungkan sisi `-` ke GPIO**

```
STPR_1 (lid sesuai wiring-mu)     ESP32
  PUL-  ────────────────────────► GPIO 18
  DIR-  ────────────────────────► GPIO 19
  ENA-  ────────────────────────► GPIO 23

STPR_2
  PUL-  ────────────────────────► GPIO 15
  DIR-  ────────────────────────► GPIO 4
  ENA-  ────────────────────────► GPIO 5
```

**4. SAMBUNGKAN GND — INI PALING PENTING**

```
GND buck converter ── GND ESP32 ── GND TB6600 ( jika ada ) ── GND baterai
```

Kalau GND tidak tersambung, sinyal tidak punya jalur kembali dan
**tidak akan bekerja sama sekali** (atau bekerja sangat tidak stabil).

**5. Upload firmware**

Firmware sekarang sudah diset `PULSE_ACTIVE = LOW` (common-anode).
Kalau kamu compile+upload sekarang, itu sudah sesuai.

**6. Uji**

Buka `http://192.168.4.1/debug`, tekan **Buka KIRI**.
Motor harus berputar **mulus** dari 0° ke 90°.

---

## Cara tahu berhasil atau belum

Di halaman `/debug`, bagian **Diagnosa Otomatis** harusnya menampilkan:

```
✓ Polaritas firmware: COMMON-ANODE 5V (aktif-LOW).
  Pastikan wiring: PUL+/DIR+/ENA+ ke 5V, PUL-/DIR-/ENA- ke GPIO,
  dan GND 5V tersambung ke GND ESP32.
```

Kalau masih muncul peringatan kuning `common-cathode 3.3V (lemah)`,
berarti firmware belum diupload ulang.

---

## Kalau masih tersendat setelah ini

Kalau wiring sudah common-anode 5V tapi **masih** tersendat, periksa
berurutan:

### 1. Arus TB6600 kurang untuk motor

NEMA 17 biasanya butuh **1.5–2.0 A**. Setel dip switch arus TB6600.
Terlalu kecil = torsi kurang = tersendat saat menahan beban.

### 2. Microstep harus 16

Setelan dip switch TB6600: **SW4 OFF, SW5 ON, SW6 ON**.
Kalau tidak 16, semua perhitungan derajat di firmware jadi salah.

### 3. Tegangan suplai motor

TB6600 butuh **12–24 V** untuk motor. Kalau hanya 9V, torsi kurang
dan motor mudah stall. Kalau bateraimu 9V, itu kemungkinan penyebabnya.

### 4. Brownout ESP32

Gerakan motor menarik arus besar dan bisa membuat tegangan turun
sesaat, cukup untuk me-restart ESP32. Cek di `/debug`:

| Baris | Seharusnya |
|---|---|
| Alasan reset terakhir | `POWERON` atau `SW` |
| Boot ke- | `1` |

Kalau tertulis `BROWNOUT` atau boot ke- lebih dari 1, tambahkan:
- Kapasitor **1000 µF elektrolitik** + **100 nF keramik** dekat pin
  5V/3V3 ESP32
- Perkuat kabel GND
- Jangan ambil daya ESP32 dari jalur yang sama dengan motor

### 5. Kecepatan terlalu tinggi

Kembali ke **K600** (tombol di `/debug`). Kalau mulus di K600 tapi
tersendat di K1600, berarti masalah torsi, bukan sinyal.

---

## Yang sudah dibuktikan lewat pengujian

Supaya kamu tahu mana yang **bukan** penyebabnya:

| Dugaan | Status | Bukti |
|---|---|---|
| Pulsa dari `loop()` tersendat WiFi | **Sudah diperbaiki** | pindah ke task core 1 |
| `vTaskDelayUntil` tick 1 ms | **Sudah diperbaiki** | spin penuh saat gerak |
| Ramp akselerasi salah | **Sudah diperbaiki** | lid kini selalu sampai 90.0° |
| Sampel jitter palsu | **Sudah diperbaiki** | field `warmUp` |
| ESP32 reset saat gerak | **Bukan** | 12 gerakan, bootCount tetap 1 |
| Timing pulsa | **Bukan** | jitter 4 µs, 0/9576 terlambat |

**Jitter pulsa sekarang 4 µs** (sebelumnya 490 µs) dan **nol pulsa
terlambat dari 9576**. Artinya dari sisi software sudah sempurna —
yang tersisa memang **level sinyal ke TB6600**.

---

## Ringkasan perubahan firmware

Konstanta di `SmartTrashbin-WiFi.ino`:

```c
#define PULSE_ACTIVE   LOW    // common-anode 5V   (SEKARANG)
// #define PULSE_ACTIVE   HIGH  // common-cathode 3.3V (tidak disarankan)
```

Turunannya otomatis:
- `PULSE_ON` / `PULSE_OFF` — pulsa & idle
- `DIR_FOR_POS` / `DIR_FOR_NEG` — arah (common-anode membalik logika)
- `ENA_ACTIVE` / `ENA_INACTIVE` — kunci & lepas motor

Semua ditulis lewat konstanta ini, jadi **tidak ada nilai hard-coded**
yang bisa tertinggal saat mengganti polaritas.

**Jangan pernah mencampur**: wiring common-anode 5V dengan firmware
aktif-HIGH akan membuat motor bergerak pada waktu yang salah — bisa
membuang step atau bahkan diam.
