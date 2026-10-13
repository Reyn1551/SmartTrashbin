# Cara Mengambil & Mengirim Log Diagnosa

Kalau masih ada masalah, kirim **satu file** ke pengembang. File itu
berisi semua yang dibutuhkan — tidak perlu foto layar atau ketik ulang.

Ada **dua cara**. Pilih yang paling mudah untukmu.

---

## Cara 1: Dari HP (paling mudah)

1. Sambungkan HP ke WiFi **`SmartTrashbin`**
2. Buka `http://192.168.4.1/debug`
3. **Lakukan uji yang bermasalah** (mis. tekan Buka lalu Tutup,
   atau UJI DIR KIRI)
4. Gulir ke bawah, tekan tombol **UNDUH LAPORAN LENGKAP**
5. File `trashbin-XXXs-bootX.txt` akan tersimpan di folder **Download**
6. Kirim file itu ke pengembang

Catatan: HP mungkin memberi peringatan "file tidak aman" karena
berakhiran `.txt` — file ini hanya teks biasa, aman dibuka.

---

## Cara 2: Dari laptop (lebih lengkap)

Skrip `ambil-log.py` **merekam terus selama kamu menguji**, sehingga
perubahan status terekam dari detik ke detik.

### Rekam sambil menguji (disarankan)

```
cd C:\Users\Reynboo\Downloads\esp32-setup\SmartTrashbin-WiFi
python ambil-log.py --detik 60 --catatan "buka lalu tutup, masih searah"
```

Setelah muncul `Merekam 60 detik`, **lakukan ujinya sekarang**.
Skrip akan mencatat status setiap detik.

### Rekam tanpa batas waktu

```
python ambil-log.py --pantau
```

Tekan `Ctrl+C` untuk berhenti.

### Ambil sekali saja

```
python ambil-log.py
```

### Hasil

File `log-smarttrashbin-<tanggal>.txt` di folder yang sama.
Kirim file itu ke pengembang.

---

## PENTING: kondisi daya saat merekam

Sebelum merekam, **pastikan dayanya lengkap**:

| Harus | Alasan |
|---|---|
| Baterai motor **hidup** | supaya buck converter 5V menyala |
| Buck 5V **menyala** | TB6600 butuh 5V di `PUL+`/`DIR+`/`ENA+` |
| ESP32 boleh dari laptop | tidak masalah, asal buck tetap hidup |

Kalau baterai motor dimatikan (untuk mencegah arus balik), maka buck 5V
juga mati → TB6600 tidak dapat sinyal → **hasil uji tidak bisa
dipercaya**. Tulis kondisi daya di `--catatan` supaya pengembang tahu.

Contoh:

```
python ambil-log.py --detik 60 --catatan "baterai HIDUP, buck 5V hidup"
```

---

## Isi file laporan

File laporan punya **tiga bagian penting**:

### 1. Penilaian otomatis

Memberi tanda di setiap pemeriksaan:

| Tanda | Arti |
|---|---|
| `[OK]` | normal |
| `[?]` | belum ada data (belum diuji) |
| `[!]` | perlu diperhatikan |
| `[X]` | **masalah** |

Dan di bawahnya ada **KESIMPULAN** yang langsung menyimpulkan.

### 2. Jejak perubahan status (khusus skrip Python)

Mencatat setiap perubahan yang terjadi selama perekaman:

```
[    29 s] pulsa +800 | KIRI MULAI gerak | KIRI 0.0->90.0 derajat
[    32 s] pulsa +800 | KIRI BERHENTI | KIRI 90.0->0.0 derajat
```

Baris seperti ini membuat pengembang bisa melihat **urutan kejadian**
tanpa harus menebak.

### 3. Log lengkap

**120 baris terakhir** dari ESP32, termasuk setiap perintah yang
diterima dan setiap perubahan arah (`[DIR]`).

---

## Uji yang sebaiknya direkam

Supaya laporannya paling berguna, lakukan ini **berurutan**:

1. **Kalibrasi** — tekan `Kalibrasi 0°`
2. **Buka** — tekan `Buka KIRI`, tunggu sampai berhenti
3. **Tutup** — tekan `Tutup KIRI`, tunggu sampai berhenti
4. **Uji DIR** — tekan `UJI DIR KIRI`, **perhatikan motornya**
5. **Mode tahan** — tekan `TAHAN Motor ON`
6. **Buka-tutup lagi** dengan motor dikunci

Lalu **tuliskan apa yang kamu lihat** di `--catatan`. Contoh:

```
--catatan "buka OK, tutup malah naik terus, jitter terasa parah,
motor kadang balik sendiri setelah berhenti"
```

Keterangan mata manusia **tidak bisa digantikan oleh log** — karena
sensor posisi tidak ada, hanya kamu yang bisa melihat lid-nya
sungguhan.

---

## Kenapa ini berguna

Firmware sudah terbukti benar di banyak lapisan:

| Lapisan | Bukti |
|---|---|
| Waktu pulsa | jitter 4 µs, 0/11562 terlambat |
| Pulsa berhenti saat diam | 0 pulsa baru selama 6 detik |
| Arah diperintahkan | log `dir=+1` lalu `dir=-1` |
| Level GPIO | log `GPIO19=LOW` lalu `HIGH` |
| ESP32 tidak reset | bootCount tetap 1 |

Jadi kalau masalah masih terasa, sisa kemungkinannya adalah:

- **Mekanik** — motor free + beban menarik (uji dengan `TAHAN Motor ON`)
- **Noise** — pulsa palsu di kabel sinyal
- **Daya** — buck 5V tidak menyala saat diuji

Laporan ini dirancang supaya ketiga kemungkinan itu bisa dibedakan
**dari jarak jauh**.

---

## Perintah yang berguna di halaman /debug

| Tombol | Fungsi |
|---|---|
| **Buka / Tutup KIRI atau KANAN** | gerakkan satu lid |
| **UJI DIR KIRI / KANAN** | gerakkan maju-mundur untuk menguji sinyal DIR |
| **TAHAN Motor ON / OFF** | kunci atau lepas motor setelah gerakan |
| **K600 / K900 / K1600 / K2000** | ubah kecepatan stepper |
| **Kalibrasi 0°** | tetapkan posisi sekarang sebagai 0 |
| **Reset Statistik** | nol-kan data jitter |
| **UNDUH LAPORAN LENGKAP** | ambil file laporan |

---

## Langkah berikutnya

1. Ambil laporan dengan **daya lengkap**
2. Tulis apa yang kamu lihat di `--catatan`
3. Kirim file-nya
4. Dari laporan itu akan terlihat apakah masalahnya mekanik, noise,
   atau daya
