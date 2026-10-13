# Hasil Analisa Laporan: trashbin-155s-boot1.txt

**Kesimpulan utama: perbaikan ramp BERHASIL.** Dua masalah baru
ditemukan, keduanya sudah diperbaiki.

---

## 1. PERBAIKAN RAMP BERHASIL ✅

| Kecepatan diminta | Sebelum perbaikan | Sesudah perbaikan |
|---|---|---|
| K600 | 679 (113%) | 867 |
| K900 | — | 1101 |
| **K1600** | **882 (55%)** ❌ | **1687 (105%)** ✅ |
| **K2000** | **1048 (52%)** ❌ | **2009 (100%)** ✅ |

Jitter tetap sempurna:

```
Jitter maksimum  : 4 us
Jitter rata-rata : 0.01 us
Pulsa terlambat  : 0 dari 23934
Pulsa macet      : TIDAK ADA
```

Ini **sepuluh kali lebih baik** dari sebelumnya.

---

## 2. Arah buka-tutup SUDAH BENAR ✅

Dari log laporan (baris 148–156):

```
[137609] [DIR] BUKA:  0 -> 800 step (dir=+1, GPIO19=LOW)
[138073] [DIR] TUTUP: 800 -> 0 step (dir=-1, GPIO19=HIGH)
```

**Firmware membalik arah dengan benar** dan kamu bisa menekan
Buka → Tutup berurutan tanpa masalah.

Kalau **fisiknya** masih searah, sekarang tersangka utamanya jelas:
**kabel `DIR+` belum ke 5V** (masih di GND).

---

## 3. MASALAH BARU A: pengukuran RAM saya salah

**Yang dilaporkan:**

```
RAM bebas   : 14204 byte
[X] RAM rendah
```

**Yang sebenarnya terjadi** (diukur langsung):

```
Saat boot          : RAM bebas 38612 byte
Setelah buka /debug: RAM bebas 37820 byte  (turun hanya 792 byte)
```

**Tidak ada kebocoran RAM.** Angka 14 KB itu **sesaat setelah** gerakan
800 pulsa @ 2000 step/s — pemakaian puncak memang tinggi. Setelah itu
RAM naik kembali.

**Pelajaran:** `freeHeap` yang diukur **sesaat** bisa menyesatkan.
Yang bermakna adalah:

| Angka | Artinya |
|---|---|
| `freeHeap` | RAM saat ini — bisa rendah sesaat lalu pulih |
| `minHeap` | titik terendah yang **pernah** dicapai |

**Sudah diperbaiki:** laporan sekarang menilai `minHeap`, bukan
`freeHeap` sesaat, dan menjelaskan kenapa.

---

## 4. MASALAH BARU B: UJI DIR salah melaporkan kecepatan

**Yang dilaporkan:**

```
[HATI] Gerakan 25 step/s aktual, diminta 2000 (1% dari target)
[HATI] Gerakan 30 step/s aktual, diminta 2000 (2% dari target)
```

Ini **tampak seperti motor hampir tidak bergerak**. Tapi itu **salah
baca dari alat saya**, karena dua sebab:

**Sebab 1 — pembanding yang salah.** UJI DIR memakai kecepatan 300
step/s, tapi log membandingkannya dengan `stepSpeedActive` (2000).
Jadi rasionya tampak 1%, padahal seharusnya ~100%.

**Sebab 2 — counter tidak di-reset.** Log menampilkan "dari 999 pulsa"
dan "dari 1199 pulsa", padahal jog hanya 200 step. Angka itu
**menumpuk** dari gerakan sebelumnya karena `stepperJog()` tidak
memanggil reset pelacak.

**Sudah diperbaiki:** `stepperJog()` sekarang me-reset pelacak, dan
UJI DIR melaporkan kecepatannya sendiri dengan benar.

---

## 5. Pengurangan RAM yang tetap dilakukan

Meski tidak ada kebocoran, `logRing` 120 baris memang memakai banyak
RAM statis:

| Versi | Ukuran | RAM |
|---|---|---|
| Awal | 40 × 96 | 3.840 byte |
| Sempat | 120 × 110 | 13.200 byte |
| **Sekarang** | **60 × 100** | **6.000 byte** |

60 baris tetap 50% lebih banyak dari semula, tapi hemat 7,2 KB.

---

## Yang perlu kamu lakukan sekarang

### 1. Sambungkan ulang ESP32

Port **COM6 hilang** saat mencoba upload:

```
Silicon Labs CP210x USB to UART Bridge (COM6)  →  Status: Unknown
```

**`Unknown`** berarti perangkat tercatat di sistem tapi **tidak
terhubung**. Coba:

1. Cabut kabel USB, tunggu 5 detik, tancapkan lagi
2. Pastikan **kabelnya kabel data**, bukan kabel charge-only
3. Coba port USB lain di laptop
4. Cek apakah **LED di ESP32 menyala**

Kalau COM6 muncul kembali, upload akan saya lanjutkan (perbaikan di
atas sudah siap, hanya belum terkirim).

### 2. Kalau ada waktu: cek kabel DIR+

Firmware sudah terbukti benar. Kalau fisik masih searah, periksa:

| Ujung | Harus ke |
|---|---|
| `DIR+` | **5V** (bukan GND!) |
| `DIR-` | GPIO19 (kiri) / GPIO4 (kanan) |

**Uji cepat:** tukar `DIR+` dan `DIR-`. Kalau arah jadi benar,
memang itu penyebabnya.

### 3. Uji dengan daya lengkap

Baterai motor **hidup** → buck 5V hidup → TB6600 dapat sinyal.
Tanpa ini, hasil uji tidak bisa dipercaya.

---

## Ringkasan status

| Lapisan | Status | Bukti |
|---|---|---|
| Waktu pulsa (jitter) | **SEMPURNA** | 4 us, 0/23934 terlambat |
| Kecepatan gerakan | **DIPERBAIKI** | 2009 vs 1048 (naik 92%) |
| Pulsa macet | **TIDAK ADA** | 0 lonjakan |
| Arah diperintahkan | **BENAR** | `dir=+1` lalu `dir=-1` |
| Level GPIO | **BENAR** | `GPIO19=LOW` lalu `HIGH` |
| RAM | **AMAN** | 38 KB, tidak bocor |
| **Arah fisik motor** | **BELUM TERBUKTI** | perlu cek kabel DIR+ |
| **Kemulusan fisik** | **BELUM TERBUKTI** | hanya kamu yang bisa lihat |
