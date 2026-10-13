# Kenapa Lid Selalu Searah — dan Cara Membedakan Penyebabnya

**Gejala:** tekan Buka → lid naik. Tekan Tutup → lid **tetap naik**.

Ada **dua penyebab** yang gejalanya **identik** dari layar. Panduan ini
membuat kamu bisa membedakannya **tanpa multimeter**.

---

## Penting dulu: soal sumber daya saat menguji

Ini yang paling mudah terlewat, dan kemungkinan besar inilah masalahmu.

Kalau saat menguji kamu:
- memberi **ESP32 daya dari laptop**, DAN
- **mematikan baterai motor** (untuk mencegah arus balik)

Maka **buck converter 5V juga ikut mati** (kalau buck disuplai dari
baterai motor).

Akibatnya:

```
PUL+  → 5V  → MATI     (buck tidak menyala)
DIR+  → 5V  → MATI     (buck tidak menyala)
ENA+  → 5V  → MATI     (buck tidak menyala)
```

TB6600 **tidak punya referensi tegangan sama sekali**. Sinyal dari GPIO
tidak punya jalur arus yang benar.

**Yang terjadi:** motor kadang tetap berputar, tapi hanya karena arus
bocor lewat jalur yang tidak seharusnya. Dan arus bocor itu **hanya
cukup untuk satu arah** — karena DIR butuh arus yang lebih tegas.

**Jadi: uji dulu dengan daya lengkap (baterai hidup + buck hidup)
sebelum menyimpulkan wiring-nya salah.**

---

## Cara membedakan kedua penyebab: pakai UJI DIR

Buka `http://192.168.4.1/debug`, tekan tombol **UJI DIR KIRI**.

Motor akan digerakkan **maju 200 step, lalu mundur 200 step**.

**Perhatikan lid-nya:**

| Yang kamu lihat | Artinya | Perbaikan |
|---|---|---|
| Lid **bergoyang bolak-balik** (naik lalu balik turun) | **DIR BEKERJA.** Masalahnya di tempat lain — lihat bagian bawah | cek langkah berikutnya |
| Lid **jalan terus satu arah** | **DIR TIDAK SAMPAI** ke TB6600 | lanjut ke bawah |

Log di `/debug` akan menampilkan:

```
[UJIDIR] KIRI: gerakan1=+200, gerakan2=-200 step
[UJIDIR] KIRI: firmware benar (maju lalu mundur)
```

Ini membuktikan firmware **sudah benar** — sekarang tinggal cek hardware.

---

## Kalau UJI DIR jalan terus satu arah

Berarti sinyal DIR tidak sampai ke driver. Periksa **berurutan**:

### 1. Buck converter 5V hidup?

Ukur dengan multimeter (DC) antara output buck dan GND-nya.

| Hasil | Tindakan |
|---|---|
| **0V** | Inilah penyebabnya. Buck sedang mati. Hidupkan sumbernya. |
| ~5V | Lanjut ke langkah berikutnya |

**Kalau kamu menguji dari laptop dengan baterai mati, kemungkinan besar
bukannya 0V.** Hidupkan baterai (dan buck) lalu ulangi uji DIR.

### 2. Kabel DIR tersambung?

| Ujung | Harus ke |
|---|---|
| `DIR+` | 5V buck converter |
| `DIR-` | GPIO19 (kiri) / GPIO4 (kanan) |

Cabut dan tancapkan ulang kabelnya di kedua ujung. Tarik perlahan —
kalau mudah lepas, krimpingnya longgar.

### 3. `DIR+` masih di GND?

Ini **sangat sering terjadi** saat pindah dari common-cathode ke
common-anode: orang memindahkan `PUL+` ke 5V tapi **lupa `DIR+`**.

Pastikan **KETIGA** pin `+` sudah pindah:

| Driver | Ke 5V | Ke GPIO |
|---|---|---|
| STPR_1 | `PUL+` `DIR+` `ENA+` | 18, **19**, 23 |
| STPR_2 | `PUL+` `DIR+` `ENA+` | 15, **4**, 5 |

### 4. GND 5V tersambung ke GND ESP32?

Kalau tidak, level LOW/HIGH **tidak punya referensi** dan tidak berarti.

Ukur dengan multimeter (mode kontinuitas) antara GND buck dan GND
ESP32 — harus nyambung (bunyi).

### 5. Coba tukar DIR+ dan DIR-

| Hasil | Artinya |
|---|---|
| Arah jadi benar | DIR memang penyebabnya |
| Masih searah | Sinyal DIR tidak sampai sama sekali — kabel putus / salah pin |
| Terbalik terus | DIR bekerja, polaritas kebalik — biarkan tertukar |

---

## Kalau UJI DIR bergoyang (DIR bekerja)

Kalau lid **bergoyang bolak-balik** saat UJI DIR, berarti DIR dan PUL
keduanya bekerja. Masalahnya beda:

### A. Masalah mekanik

Kopling motor **selip**. Motor berputar dua arah, tapi beban lid tidak
ikut kembali.

**Cek:** beri tanda pada poros motor. Saat Tutup, apakah **porosnya**
juga berbalik arah? Kalau poros berbalik tapi lid tidak → kopling selip.

### B. Terlalu berat / torsi kurang

Motor tidak sanggup mengangkat beban, sehingga stall sebelum sampai.

**Cek:** lepas beban lid, ulangi. Kalau tanpa beban berhasil, berarti
masalah torsi:
- Naikkan arus TB6600 ke **1.5–2.0 A**
- Turunkan kecepatan ke **K600**
- Pastikan microstep **16** (SW4 OFF, SW5 ON, SW6 ON)

### C. Yang bergerak lid yang salah

Pastikan `IL1` benar-benar menggerakkan **lid kiri**, bukan kanan.

---

## Ringkasan alur diagnosa

```
Lid buka-tutup searah
        │
        ├─ Uji dengan daya LENGKAP (baterai + buck hidup)
        │        │
        │        └─ Masih searah? ──► lanjut
        │
        ├─ Tekan UJI DIR KIRI di /debug
        │        │
        │        ├─ BERGOYANG bolak-balik
        │        │     └─ DIR bekerja ──► cek mekanik / torsi / arus
        │        │
        │        └─ JALAN TERUS satu arah
        │              └─ DIR tidak sampai
        │                    ├─ buck 5V mati?
        │                    ├─ DIR+ belum ke 5V?
        │                    ├─ kabel DIR lepas/salah pin?
        │                    └─ GND 5V ↔ GND ESP32 tidak nyambung?
```

---

## Yang sudah dibuktikan (jangan diulang)

| Lapisan | Status | Bukti |
|---|---|---|
| Waktu pulsa | **BENAR** | jitter 4 µs, 0/9576 terlambat |
| ESP32 reset | **BUKAN masalah** | bootCount tetap 1 saat 12 gerakan |
| Hitung arah | **BENAR** | log `dir=+1` lalu `dir=-1` |
| Tulis GPIO | **BENAR** | log `GPIO19=LOW` lalu `HIGH` |
| Uji DIR internal | **BENAR** | `gerakan1=+200, gerakan2=-200` |
| **Daya 5V & kabel DIR** | **BELUM TERBUKTI** | ← fokus di sini |

**Jangan ubah firmware lagi.** Empat lapisan pertama sudah terbukti
benar lewat log firmware sendiri. Yang tersisa adalah **hardware**.
