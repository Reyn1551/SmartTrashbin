# Debug dari HP (tanpa laptop)

Halaman diagnostik di dalam ESP32. Bisa dibuka dari HP saat robot jalan
dari baterai — **tidak perlu kabel, tidak perlu laptop, tidak perlu install apa pun.**

---

## Cara buka

1. HP tersambung ke WiFi `SmartTrashbin`
2. Buka browser ke:

```
http://192.168.4.1/debug
```

Selesai. Halaman ini memperbarui sendiri setiap 1 detik.

---

## Isi halaman

### 1. Stepper · Lid

Posisi dan kecepatan tiap lid:

| Baris | Arti |
|---|---|
| `KIRI pos / target` | posisi sekarang vs tujuan (derajat) |
| `KIRI kecepatan` | kecepatan pulsa saat ini (step/s) atau "diam" |

### 2. Jitter Monitor

Ini alat utama untuk mendiagnosa gerakan kasar:

| Baris | Arti |
|---|---|
| **Jitter maksimum** | keterlambatan pulsa terburuk |
| **Jitter rata-rata** | rata-rata keterlambatan |
| **Pulsa terlambat >50µs** | berapa pulsa yang lewat jadwal |
| **Total pulsa keluar** | jumlah pulsa sejak reset |

**Cara membaca:**

| Jitter maksimum | Arti |
|---|---|
| **< 50 µs** | halus — tidak ada masalah |
| **50–200 µs** | agak kasar, mungkin terasa sedikit tersentak |
| **> 200 µs** | tersentak — ada yang menahan pulsa |

Warna bar juga mengikuti: **hijau / kuning / merah**.

### 3. Diagnosa Otomatis

Bagian ini **membaca semua data dan memberi tahu penyebabnya** dalam bahasa
manusia. Contoh keluaran:

```
✓ Jitter sangat baik. Gerakan seharusnya halus.
✓ RAM cukup: 49 KB bebas.
! Kecepatan stepper tinggi (3000 step/s). Kalau motor bergetar,
  turunkan ke K800 atau K600.
! CATATAN: posisi di atas dihitung dari jumlah pulsa yang DIKIRIM
  ke driver, bukan dari sensor...
```

Kalau ada masalah, di sini akan muncul peringatan merah dengan saran
perbaikan konkret.

### 4. Sistem

Uptime, RAM bebas/minimum, jumlah klien WiFi, kecepatan stepper aktif,
PWM roda, dan status gerak.

### 5. Tes Cepat

Tombol untuk menguji tanpa panel utama:

| Tombol | Fungsi |
|---|---|
| Buka KIRI / Tutup KIRI | uji lid kiri saja |
| Buka KANAN / Tutup KANAN | uji lid kanan saja |
| K600 (aman) | lambat, torsi paling besar |
| K900 | sedang |
| **K1600 (default)** | kecepatan bawaan robot |
| K2000 (tercepat) | paling cepat |
| Kalibrasi 0° | set posisi sekarang = 0° |
| Tutup KEDUA | tutup kedua lid |
| STOP RODA | stop motor roda |
| Reset Statistik | nol-kan hitungan jitter |

> **Setelah reboot, kecepatan kembali ke K1600.** Kalau kamu sudah ubah
> kecepatan dari tombol, itu tidak tersimpan permanen.

### 6. Log (40 terakhir)

40 pesan terakhir dari firmware, dengan timestamp. Termasuk semua perintah
yang diterima, hasilnya, dan error. **Ini pengganti serial monitor.**

Contoh isi:
```
[   83026] [WEB] Terima: I (arg=L1)
[   83027] [LID] Lid KIRI -> BUKA (90.0 deg)
[   88999] [WEB] Terima: I (arg=L0)
[   88999] [LID] Lid KIRI -> TUTUP (0.0 deg)
```

---

## Endpoint (untuk skrip / terminal)

Kalau mau otomatis, ambil data mentah:

| URL | Isi |
|---|---|
| `/debug` | halaman HTML (untuk HP) |
| `/debug/json` | semua angka dalam JSON |
| `/debug/log` | 40 pesan log, teks biasa |
| `/debug/reset` | nol-kan statistik jitter |

Contoh dari terminal:
```powershell
# lihat semua data
Invoke-WebRequest "http://192.168.4.1/debug/json" | Select -Expand Content

# lihat log
Invoke-WebRequest "http://192.168.4.1/debug/log" | Select -Expand Content

# nol-kan statistik
Invoke-WebRequest "http://192.168.4.1/debug/reset"
```

---

## Field JSON

| Field | Arti |
|---|---|
| `uptime` | detik sejak nyala |
| `freeHeap` / `minHeap` | RAM bebas / terendah (byte) |
| `wifiClients` | jumlah HP tersambung |
| `ll` / `lr` | posisi lid kiri / kanan (derajat) |
| `llTarget` / `lrTarget` | tujuan lid kiri / kanan |
| `llMoving` / `lrMoving` | 1 = sedang bergerak |
| `llSpeed` / `lrSpeed` | kecepatan pulsa (step/s) |
| `jitMaxUs` | jitter maksimum (µs) |
| `jitAvgUs` | jitter rata-rata (µs) |
| `jitSamples` | jumlah sampel jitter |
| `jitLate` | pulsa terlambat >50 µs |
| `stepTotal` | total pulsa keluar |
| `stepSpeed` | kecepatan stepper aktif |
| `rampSteps` | panjang ramp akselerasi |
| `pulseMinUs` | lebar pulsa minimum |
| `pwm` | kecepatan roda |
| `move` | `stop` / `run` |
| `lopen` | sudut buka penuh (90) |

---

## Keterbatasan yang perlu kamu tahu

**Posisi lid dihitung dari jumlah pulsa yang DIKIRIM, bukan dari sensor.**

Artinya: kalau motor kehilangan langkah (stall) karena arus kurang atau
getar, firmware **tetap melaporkan 90°** padahal lid belum sampai.

**Cara tahu kehilangan langkah tanpa sensor:** perhatikan lid secara
visual. Kalau lid tidak sampai penuh tapi halaman debug bilang `90.0°`,
itu kehilangan langkah. Solusinya:
- turunkan kecepatan (`K300`)
- naikkan arus TB6600 ke 1.5–2.0 A
- perbaiki level sinyal PUL ke 5V

Kalau nanti kamu mau akurasi sebenarnya, perlu **sensor** — mis.
limit switch di posisi tutup, atau encoder. Bilang saya kalau mau
ditambahkan.

---

## Jitter: penyebab dan riwayat perbaikannya

Bagian ini penting kalau kamu mau paham kenapa gerakannya sekarang halus.

### Versi awal — pulsa dari `loop()` (JITTER PARAH)

Pulsa stepper dikeluarkan dari `loop()`. Masalahnya `loop()` tersendat
oleh WiFi: `server.handleClient()` dan stack WiFi bisa memblokir
puluhan milidetik. Selama tersendat, pulsa **berhenti** → motor tersentak.

Ini penyebab utama gerakan kasar yang kamu rasakan.

### Perbaikan 1 — pindah ke task core 1

WiFi di ESP32 berjalan di **core 0**. Pulsa stepper dipindah ke **task
terpisah di core 1**, jadi keduanya tidak saling mengganggu.

Terukur (beban WiFi 20 request/detik):
- Jitter rata-rata: **39 µs → 17 µs**

Tapi maksimum masih naik-turun (1156–1761 µs). Jadi core saja belum cukup.

### Perbaikan 2 — spin penuh selama gerakan (INI YANG MENUNTASKAN)

Masalahnya: task masih memakai `vTaskDelayUntil` dengan tick **1 ms**.
Sementara perioda pulsa pada K600 hanya **0.83 ms**. Jadi setiap kali task
tidur, pulsa berikutnya harus menunggu tick berikutnya → keterlambatan
rata-rata **~490 µs** (hampir separuh tick).

**Solusi:** selama ada motor bergerak, task **tidak tidur sama sekali**.
Saat motor diam, task tidur 10 ms (hemat CPU).

Terukur setelah perbaikan:

| Kondisi | Jitter rata-rata | Jitter max | Pulsa terlambat |
|---|---|---|---|
| Tanpa beban WiFi | **0.2 µs** | 218 µs | 1 / 1600 |
| Beban WiFi berat | **0.73 µs** | 1175 µs | 2 / 3200 |

Turun dari **490 µs ke 0.73 µs** — perbaikan ~670×.

**Trade-off:** core 1 terpakai penuh selama lid bergerak. Ini wajar karena
gerakan hanya <2 detik, dan core 1 memang disediakan khusus untuk ini.

### Kenapa bukan pakai `ESP_TIMER_ISR`?

`ESP_TIMER_ISR` (pulsa dari interrupt, prioritas tertinggi) **tidak
tersedia** di build Arduino core 3.3.12, karena header membungkusnya dengan:
```c
#if CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
```
dan opsi itu tidak diaktifkan pada core Arduino.

Jadi pendekatan task core-pinned adalah cara terbaik yang tersedia.

---

## Hasil pengujian kecepatan (data nyata)

Diuji pada robot ini, lid kiri, buka 90 derajat, **tanpa beban lid**:

| Kecepatan | Posisi akhir | Jitter max | Jitter rata-rata | Pulsa terlambat | Waktu |
|---|---|---|---|---|---|
| K300 | 90.0° | 4 µs | 0.01 µs | 0 / 798 | 1.77 s |
| K600 | 90.0° | 2 µs | 0.01 µs | 0 / 798 | 1.23 s |
| K900 | 90.0° | 3 µs | 0.01 µs | 0 / 798 | 1.10 s |
| K1200 | 90.0° | 3 µs | 0.02 µs | 0 / 798 | 1.00 s |
| **K1600** | **90.0°** | **4 µs** | **0.01 µs** | **0 / 798** | **0.94 s** |
| K2000 | 90.0° | 4 µs | 0.01 µs | 0 / 798 | 0.81 s |

Dua lid bersamaan pada K1600: **jitter 1 µs, 0 pulsa terlambat dari 1596.**

**Semua kecepatan tergolong HALUS** (jitter < 50 µs). K1600 dipilih
sebagai default karena paling cepat dengan margin aman.

### Bug ramp yang ditemukan saat pengujian ini

Versi awal punya **`RAMP_STEPS` tetap = 400 step**. Akibatnya:

- Pada **K300**, ramp 400 step memakan hampir seluruh gerakan, dan
  deselerasi turun sampai 10% × 300 = **30 step/s** — terlalu pelan.
  Motor **stall di akhir** dan lid berhenti di **84.0°**, bukan 90°.
  Ini terjadi **3 dari 3 kali**, jadi sistematis, bukan kebetulan.
- Pada K600–K1600 hasilnya normal, sehingga masalah ini mudah terlewat.

**Perbaikannya** — ramp dibuat proporsional terhadap kecepatan:

```c
float rampLenF = s.speedTarget * 0.5f;          // K1600 -> 800 step
if (rampLenF < RAMP_MIN_STEPS) rampLenF = ...;  // min 150
if (rampLenF > RAMP_MAX_STEPS) rampLenF = ...;  // max 800
if (frac < SLOW_FLOOR_FRAC) frac = 0.35f;       // jangan melambat terlalu pelan
if (s.speedCurrent < SPEED_MIN_SPS) ...         // min 100 step/s
```

Nilai yang bisa disetel kalau perlu:

| Konstanta | Nilai | Guna |
|---|---|---|
| `RAMP_MIN_STEPS` | 150 | ramp terpendek |
| `RAMP_MAX_STEPS` | 800 | ramp terpanjang |
| `SLOW_FLOOR_FRAC` | 0.35 | lantai deselerasi (naikkan kalau stall di akhir) |
| `SPEED_MIN_SPS` | 100 | kecepatan minimum (jaga torsi) |

### Satu lagi: "1 pulsa terlambat" yang menyesatkan

Setelah ramp diperbaiki, muncul **selalu 1 pulsa terlambat** di setiap
pengujian, dengan jitter max 1700–8299 µs. Ternyata itu **sampel palsu**:

Pulsa pertama dan kedua setelah gerakan dimulai punya perioda yang belum
stabil, karena `speedCurrent` baru di-ramp naik **dari nol**. Itu transisi
yang sah, bukan gangguan.

**Perbaikan:** tambah field `warmUp` — 2 pulsa pertama dilewati dari
statistik. Setelah itu jitter max turun dari 8299 µs ke **2–4 µs** dan
"1 pulsa terlambat" hilang sepenuhnya.

**Pelajaran:** kalau jitter terlihat buruk hanya di awal gerakan, itu
transisi ramp — jangan buru-buru mengubah timer atau task.

### Catatan penting saat mengukur

**Jitter TIDAK boleh diukur dari selisih waktu antar-pulsa.** Perioda pulsa
memang berubah sesuai ramp (motor naik/turun kecepatan) — itu bukan
gangguan. Yang diukur adalah **kelebihan tunggu**:

```c
unsigned long elapsed = now - lastPulseUs;
if (elapsed < halfPeriodUs) return;
unsigned long over = elapsed - halfPeriodUs;   // <-- ini jitter
```

---

## Kalau robot dari baterai: apa yang perlu diperhatikan

Karena nanti robot dilepas dari laptop:

**1. Tidak ada serial monitor.** Pakai `/debug` di HP. Log 40 pesan
terakhir tetap tersimpan selama robot hidup.

**2. Cek RAM.** Kalau `freeHeap` turun di bawah 20 KB, restart robot.
Ditampilkan di halaman debug, dan diagnosa otomatis akan memperingatkan.

**3. Cek jitter setelah pemanasan.** Biarkan robot jalan 10 menit, lalu
lihat `/debug`. Kalau jitter memburuk setelah panas, kemungkinan:
- catu daya turun (baterai lemah)
- TB6600 kepanasan (pasang heatsink / kurangi arus)

**4. Arus TB6600 stabil?** Kalau gerakan berubah kasar saat baterai mulai
lemah, itu tanda tegangan suplai turun. Cek tegangan baterai.

**5. Uptime.** Kalau `uptime` ter-reset sendiri, berarti ESP32 reboot —
biasanya karena catu daya tidak stabil. Cek log untuk melihat penyebabnya.
