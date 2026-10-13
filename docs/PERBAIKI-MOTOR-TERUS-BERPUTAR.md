# Motor Berputar Terus Walau Firmware Sudah Berhenti

**Gejala:** posisi di `/debug` sudah menunjukkan `90.0 / 90.0` dan
kecepatan `diam`, **tapi poros motor masih berputar**. Buka maupun tutup,
poros selalu berputar ke arah yang sama (CW).

---

## Ini BUKAN "salah arah"

Penting dibedakan:

| Gejala | Artinya |
|---|---|
| Motor berputar, tapi arahnya **tidak sesuai perintah** | Masalah sinyal DIR |
| Motor **masih berputar** padahal firmware sudah **diam** | Motor berputar **tanpa pulsa** |

Gejalamu yang kedua. Buktinya:

```
BUKA : 0 -> 800 pulsa   lalu BERHENTI
TUTUP: 800 -> 0 pulsa   lalu BERHENTI
Saat diam: stepTotal TIDAK berubah (0 pulsa baru)
```

Firmware **sudah berhenti mengirim pulsa**. Kalau motor tetap berputar,
puteranya **tidak berasal dari firmware**.

---

## Tiga penyebab, dan cara membedakannya

### Penyebab 1: Motor FREE + beban menariknya (paling mungkin)

Firmware normalnya **melepas** motor setelah gerakan selesai
(`ENA` tidak dialiri arus). Ini disengaja:
- hemat daya
- motor tidak panas

**Tapi motor yang free bisa berputar sendiri** karena:
- berat lid menariknya (gravitasi)
- getaran mekanik
- sisa medan magnet

Ini terlihat **persis** seperti "terus berputar", dan arahnya selalu
sama karena gravitasi selalu ke bawah.

**Uji:** tekan **TAHAN Motor ON** di `/debug`, lalu uji Buka/Tutup lagi.

| Hasil | Artinya |
|---|---|
| Motor **berhenti** berputar sendiri | Memang penyebab 1 — motor free + beban |
| Motor **masih** berputar | Lanjut ke penyebab 2 |

### Penyebab 2: Pulsa palsu dari noise

Kalau kabel `PUL` menangkap interferensi (dari motor DC, driver, atau
kabel daya yang sejajar), setiap spike dianggap **satu pulsa**. Hasilnya
motor berputar terus walau ESP32 diam.

**Ciri:** motor berputar **tidak teratur** dan **bergetar** — bukan
mulus. Kecepatannya bisa berubah-ubah.

**Perbaikan:**
- Jauhkan kabel `PUL` dari kabel motor DC dan kabel daya
- Pakai kabel **berpelindung (shielded)**, pelindung ke GND
- Tambah **kapasitor 100nF** dari `PUL-` ke GND (dekat TB6600)
- Perpendek kabel sinyal

### Penyebab 3: TB6600 rusak / arus terlalu tinggi

Kalau TB6600 kelebihan arus atau pernah kepanasan, outputnya bisa
"lari sendiri" tanpa input.

**Ciri:** motor berputar **mulus dan konstan** walau tidak ada sinyal.
Terkadang disertai bunyi mendesis.

**Uji:** matikan `ENA` sama sekali (lepas kabel `ENA-`). Kalau motor
**masih** berputar, TB6600 rusak.

---

## Langkah diagnosa berurutan

```
Motor berputar walau firmware diam
        │
        ├─ 1. Tekan TAHAN Motor ON di /debug
        │        │
        │        ├─ Berhenti ──► Penyebab 1: motor free + beban
        │        │                 └─ Pakai H1 permanen, atau tambah
        │        │                    pemberat/gesekan pada mekanik
        │        │
        │        └─ Masih berputar ──► lanjut
        │
        ├─ 2. Lepas kabel ENA- dari TB6600
        │        │
        │        ├─ Berhenti ──► sinyal ENA bermasalah
        │        │
        │        └─ Masih berputar ──► lanjut
        │
        └─ 3. Lepas kabel PUL- dari TB6600
                 │
                 ├─ Berhenti ──► Penyebab 2: pulsa palsu / noise
                 │
                 └─ Masih berputar ──► Penyebab 3: TB6600 rusak
```

---

## Cara mengaktifkan mode TAHAN permanen

Kalau penyebabnya ternyata motor free + beban, kamu bisa memakai mode
tahan sebagai solusi tetap.

**Sementara (tanpa upload ulang):**
- Tekan **TAHAN Motor ON** di `/debug`, atau
- Kirim perintah `H1`

Mode ini **hilang saat reboot** — harus diaktifkan lagi.

**Permanen:** ubah satu baris di `SmartTrashbin-WiFi.ino`:

```c
bool holdMode = true;    // dari false
```

Lalu compile dan upload ulang.

**Catatan:** dengan mode tahan, motor **selalu dialiri arus** sehingga
**menjadi panas**. Untuk NEMA 17 pada 1.5 A, itu normal tapi perlu
sirkulasi udara. Kalau khawatir, matikan robot saat tidak dipakai.

---

## Yang sudah dibuktikan (jangan diulang)

| Lapisan | Status | Bukti |
|---|---|---|
| Waktu pulsa | **BENAR** | jitter 4 µs, 0/11562 terlambat |
| Pulsa berhenti saat diam | **BENAR** | stepTotal tidak naik (0 pulsa) |
| Hitung arah | **BENAR** | `dir=+1` lalu `dir=-1` di log |
| Tulis GPIO | **BENAR** | `GPIO19=LOW` lalu `HIGH` di log |
| ESP32 reset | **BUKAN** | bootCount tetap 1 |
| **Mekanik / noise / TB6600** | **BELUM TERBUKTI** | ← fokus di sini |

**Jangan ubah firmware lagi** untuk masalah arah. Empat lapisan pertama
sudah terbukti benar. Yang tersisa adalah **mekanik** atau **noise**.

---

## Ringkasan tombol baru di /debug

| Tombol | Fungsi |
|---|---|
| **TAHAN Motor ON** | kunci motor setelah gerakan (uji penyebab 1) |
| **TAHAN Motor OFF** | lepas motor setelah gerakan (normal) |
| **UJI DIR KIRI / KANAN** | gerakkan maju-mundur (uji sinyal DIR) |

Baris **Mode tahan motor** di kartu *Uji DIR & Daya* menunjukkan status
sekarang.
