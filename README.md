# SmartTrashbin

Robot tong sampah pintar berbasis ESP32. Dua motor DC untuk penggerak roda
(driver BTN7970) dan dua motor stepper NEMA 17 untuk membuka dan menutup
tutup tong (driver TB6600). Kontrol utama lewat WiFi, Bluetooth sebagai
cadangan.

ESP32 memancarkan hotspot sendiri, dan antarmuka kontrolnya sudah tertanam
di dalam firmware, jadi tidak perlu aplikasi tambahan dan tidak perlu
internet.

| | |
|---|---|
| Nama hotspot | `SmartTrashbin` |
| Password | `12345678` |
| Alamat kontrol | http://192.168.4.1 |
| Halaman diagnosa | http://192.168.4.1/debug |

## Perangkat keras

| Bagian | Komponen |
|---|---|
| Pengendali | ESP32 DevKit V4 32D (ESP32-D0WD-V3 rev 3.1) |
| Penggerak roda | BTN7970 x2 |
| Motor roda | DC gearbox, differential drive |
| Penggerak tutup | TB6600 x2 |
| Motor tutup | NEMA 17, 1.8 derajat |

### Pemetaan pin

| Fungsi | GPIO |
|---|---|
| Roda kiri PWM / D1 / D2 | 25 / 27 / 26 |
| Roda kanan PWM / D1 / D2 | 33 / 32 / 14 |
| Tutup kiri PUL / DIR / ENA | 18 / 19 / 23 |
| Tutup kanan PUL / DIR / ENA | 15 / 4 / 5 |

Sisi positif TB6600 (PUL+, DIR+, ENA+) dihubungkan ke 5V dari buck
converter, dan GND 5V wajib disambungkan ke GND ESP32.

## Cara pakai

1. Nyalakan robot.
2. Di HP, sambungkan WiFi ke jaringan `SmartTrashbin` dengan password
   `12345678`. Kalau Android bertanya "WiFi tanpa internet, tetap
   sambungkan?", jawab **Tetap Sambungkan**. Matikan data seluler supaya
   HP tidak berpindah kembali ke internet.
3. Buka browser ke http://192.168.4.1

Tombol arah di sisi kiri menggerakkan robot selama tombol **ditahan**.
Tombol di sisi kanan membuka dan menutup tutup.

## Build dan upload

Proyek ini memakai `arduino-cli` dengan core ESP32 versi 3.x.

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app --upload -p COM6 .
```

Skema partisi `huge_app` **wajib** dipakai. Firmware dengan UI yang
tertanam berukuran sekitar 1,76 MB, dan skema partisi bawaan tidak cukup
besar.

### Mengubah tampilan kontrol

File `web_ui.h` adalah hasil generate, jangan disunting langsung. Sunting
`controller-wifi.html`, lalu jalankan:

```powershell
python konversi-html.py
```

Skrip itu menulis ulang `web_ui.h` dari HTML. Setelah itu baru compile dan
upload.

## Perintah

Perintah bisa dikirim lewat HTTP (`/cmd?val=X`), lewat Bluetooth, atau
lewat Serial pada 115200 baud.

### Roda

| Perintah | Fungsi |
|---|---|
| `F` / `B` | maju / mundur |
| `L` / `R` | belok kiri / kanan |
| `P` | stop |
| `V<n>` | kecepatan PWM, 0 sampai 255 |

### Tutup

| Perintah | Fungsi |
|---|---|
| `C` / `S` | buka / tutup kedua tutup |
| `IL1` / `IL0` | buka / tutup tutup kiri |
| `IR1` / `IR0` | buka / tutup tutup kanan |
| `W<sudut>` | atur sudut bukaan, dipakai semua perintah buka |
| `A` | kalibrasi, posisi sekarang dianggap 0 derajat |
| `K<sps>` | kecepatan stepper, step per detik |

### Penyetelan

| Perintah | Fungsi |
|---|---|
| `YL1` / `YL0` | balik / normalkan arah motor kiri |
| `YR1` / `YR0` | balik / normalkan arah motor kanan |
| `H1` / `H0` | mode tahan motor, motor dikunci atau dilepas |
| `D` | uji arah, jog maju lalu mundur |

Setelan sudut dan pembalik arah motor disimpan di memori flash, jadi tidak
hilang saat robot dimatikan.

## Diagnosa

Halaman `/debug` menampilkan jitter pulsa, presisi gerakan, status motor
penggerak, dan log terakhir. Endpoint yang tersedia:

| Alamat | Isi |
|---|---|
| `/debug` | halaman diagnosa |
| `/debug/json` | seluruh status dalam JSON |
| `/debug/log` | log teks |
| `/debug/download` | unduh laporan lengkap sebagai file |
| `/debug/reset` | nolkan statistik |

Karena tidak ada sensor posisi, **posisi tutup dihitung dari jumlah pulsa
yang dikirim ke driver, bukan dari pengukuran**. Motor yang kehilangan
langkah tetap terbaca sesuai target walaupun tutup belum sampai. Panel
"Presisi Gerakan" di `/debug` ada untuk menunjukkan berapa sering hal itu
terjadi.

Untuk merekam log dari HP tanpa laptop:

```powershell
python ambil-log.py --detik 60 --catatan "buka tutup 10 kali"
```

## Catatan penting

Laporan hasil diagnosa hanya sah kalau pengujian dijalankan dengan
**baterai motor hidup**, karena buck 5V butuh daya dari baterai untuk
memberi tegangan ke sisi positif TB6600. Kalau robot hanya diberi daya
dari laptop, sinyal ke driver tidak punya tegangan dan hasil pengujian
tidak menggambarkan kondisi sebenarnya.

Selama proses pengembangan, pengujian dari laptop dilakukan dengan baterai
motor dimatikan sengaja, untuk mencegah arus balik. Karena itu jangan
menarik kesimpulan tentang perangkat keras dari pengujian seperti itu.

## Struktur berkas

```
.
|-- SmartTrashbin-WiFi.ino    firmware utama
|-- controller-wifi.html      antarmuka kontrol, sunting berkas ini
|-- controller-ble.html       antarmuka Bluetooth cadangan
|-- web_ui.h                  hasil generate dari controller-wifi.html
|-- konversi-html.py          pembuat web_ui.h
|-- ambil-log.py              perekam log dari HP
|-- docs/                     dokumentasi dan catatan perbaikan
|-- _lama/                    berkas versi lama sebagai cadangan
```

## Lisensi

Belum ditentukan.
