# Mengatur Torsi Stepper: Apa yang Bisa dan Tidak Bisa

## Setelan yang dipakai sekarang

| Parameter | Nilai |
|---|---|
| Microstep | **1/8** (DIP SW1=OFF, SW2=ON, SW3=OFF) |
| `STEPS_PER_REV` | 1600 |
| `STEPS_PER_DEG` | 4.4444 |
| Torsi per langkah | 19,51 % dari torsi holding |
| Arus TB6600 | **2,0 A** (SW4=ON, SW5=OFF, SW6=OFF) |

Profil kecepatan:

| Profil | Stepper | PWM roda |
|---|---|---|
| SLOW | 200 step/detik | 45 |
| MEDIUM | 450 step/detik | 110 |
| FAST | 800 step/detik | 200 |

Angka step per detik ini sengaja **separuh** dari nilai saat microstep
1/16, supaya kecepatan gerak lid yang terlihat tidak berubah. Pada
microstep 1/8, 1600 pulsa sama dengan satu putaran penuh motor,
sedangkan pada 1/16 baru setengah putaran.

## Jawaban singkat

Torsi stepper **tidak bisa diatur dari firmware**. Torsi ditentukan
oleh perangkat keras, bukan oleh program.

Uji `Q` yang ditambahkan **mengukur** pada kecepatan berapa motor mulai
kehilangan langkah. Uji itu tidak menambah tenaga.

## Kenapa torsi tidak bisa diatur dari firmware

Torsi motor stepper ditentukan dua hal, dan keduanya ada di perangkat
keras:

### 1. Arus yang disetel di TB6600

Ini yang paling menentukan. Arus diatur oleh DIP switch **SW1, SW2,
dan SW3** di modul TB6600. Nilai yang tersedia biasanya 0.5 sampai
4.0 A.

Motor NEMA 17 umumnya butuh **1.5 sampai 2.0 A**.

Selama arusnya terlalu kecil, motor akan kehilangan langkah atau tidak
kuat mengangkat beban, dan **tidak ada perintah dari ESP32 yang bisa
menambahnya**. Sinyal PUL dan DIR hanya menentukan kapan dan ke arah
mana motor berputar, bukan seberapa kuat.

### 2. Level microstep (SW4, SW5, SW6)

Ini berlawanan dengan intuisi, tetapi **menaikkan microstep justru
menurunkan torsi per langkah**.

Menurut Faulhaber Application Note 015, torsi per microstep dibanding
torsi holding penuh:

| Microstep | Torsi per microstep |
|---|---|
| 1 | 100.00 % |
| 2 | 70.71 % |
| 4 | 38.27 % |
| 8 | 19.51 % |
| **16** | **9.80 %** |

Pada microstep 16, satu langkah hanya menghasilkan kurang dari 10
persen torsi holding. Sementara itu detent torque motor sendiri
**5 sampai 20 persen** dari holding torque.

Artinya torsi satu microstep seringkali **tidak cukup** untuk
mengalahkan detent torque, sehingga sebagian perintah langkah tidak
menghasilkan gerakan sama sekali.

Kutipan langsung dari dokumen Faulhaber:

> "Simply stated, taking a microstep does not mean the motor will
> actually move!"

> "In summary, although Microstepping gives the designer more
> resolution, improved accuracy is not realized."

Jadi untuk beban berat, **microstep rendah memberi torsi lebih
besar**. Lid tong sampah tidak butuh resolusi 3200 pulsa per putaran,
jadi menurunkan ke 8 atau 4 adalah pertukaran yang menguntungkan:
resolusi yang tidak dipakai ditukar dengan torsi yang memang
dibutuhkan.

## Dua versi chip TB6600 yang beredar

Modul TB6600 dijual dalam dua versi, dan keduanya punya tabel DIP
switch yang berbeda. Periksa dulu yang mana milikmu.

| Chip | Microstep maksimal | Cara mengenali |
|---|---|---|
| **TB6600HG** (asli Toshiba) | **hanya sampai 1/16** | ada pin menonjol keluar dari PCB, chip besar, heatsink bagus |
| **TB67S109AFTG** ("upgrade") | sampai 1/32 | chip kecil, tidak ada pin menonjol |

Ini penting: kalau modulnya memakai **TB6600HG asli**, setelan 1/32
**tidak tersedia**. Tabel resmi menyebut kombinasi itu sebagai `NC`
(not connected, tidak terdefinisi), jadi menyetelnya ke 1/32 bisa
masuk mode yang tidak diharapkan.

Kedua chip ini sama-sama tidak mengubah kesimpulan soal torsi:
microstep tinggi tetap berarti torsi per langkah kecil.

## Tabel DIP switch microstep

Nomor switch di modul adalah **SW1, SW2, SW3** untuk microstep.

| Microstep | Pulsa/putaran | SW1 | SW2 | SW3 | Torsi per langkah |
|---|---|---|---|---|---|
| 1/1 | 200 | ON | ON | OFF | 100,00 % |
| 1/2 | 400 | ON | OFF | ON | 70,71 % |
| 1/4 | 800 | ON | OFF | OFF | 38,27 % |
| **1/8** | **1600** | **OFF** | **ON** | **OFF** | **19,51 %** |
| 1/16 | 3200 | OFF | OFF | ON | 9,80 % |
| 1/32 | 6400 | OFF | OFF | OFF | 4,91 % |

## Tabel DIP switch arus

Nomor switch di modul adalah **SW4, SW5, SW6** untuk arus.

| Arus | Puncak | SW4 | SW5 | SW6 |
|---|---|---|---|---|
| 0,5 A | 0,7 A | ON | ON | ON |
| 1,0 A | 1,2 A | ON | OFF | ON |
| 1,5 A | 1,7 A | ON | ON | OFF |
| **2,0 A** | **2,2 A** | **ON** | **OFF** | **OFF** |
| 2,5 A | 2,7 A | OFF | ON | ON |
| 2,8 A | 2,9 A | OFF | OFF | ON |
| 3,0 A | 3,2 A | OFF | ON | OFF |
| 3,5 A | 4,0 A | OFF | OFF | OFF |

**Peringatan:** motor NEMA 17 umumnya dinilai 1,5 sampai 2,0 A. Melebihi
itu membuat motor panas berlebih dan bisa merusak isolasi kumparan.
Kalau motor menjadi sangat panas setelah beberapa menit, turunkan satu
tingkat. **Selalu matikan daya sebelum mengubah DIP switch.**

## Urutan perbaikan kalau torsi kurang

| Urutan | Tindakan | Perkiraan manfaat |
|---|---|---|
| **1** | Naikkan arus TB6600 lewat SW4-SW6 ke 2,0 A | **Terbesar dan paling murah** |
| 2 | Turunkan microstep dari 1/16 ke 1/8 | Dua kali lipat torsi per langkah |
| 3 | Naikkan tegangan suplai motor ke 24 V | Sedang, terutama di kecepatan tinggi |
| 4 | Turunkan kecepatan gerak | Kecil |

## Kalau mengubah microstep, firmware wajib ikut diubah

Konstanta `STEPS_PER_DEG` di `SmartTrashbin.ino` harus disesuaikan.
Kalau tidak, sudut yang diminta akan salah.

| Microstep | Pulsa/putaran | `STEPS_PER_DEG` |
|---|---|---|
| 4 | 3600 | `3600.0 / 360.0` = **10.0000** |
| 8 | 1600 | `1600.0 / 360.0` = **4.4444** |
| 16 | 3200 | `3200.0 / 360.0` = **8.8889** (nilai sekarang) |

## Cara memakai alat uji

Buka `http://192.168.4.1/debug`, panel **"Uji Torsi & Kecepatan
Maksimal"**, lalu tekan **LID KIRI**, **LID KANAN**, atau **KEDUA**.

Atau lewat Serial:

| Perintah | Fungsi |
|---|---|
| `QL` | uji lid kiri |
| `QR` | uji lid kanan |
| `QB` | uji kedua lid |

### Cara kerjanya

Motor digerakkan bolak-balik 800 langkah (90 derajat) pada kecepatan
yang naik bertahap:

```
200, 400, 600, 800, 1000, 1200, 1400, 1600, 1800, 2000 step/detik
```

Pada setiap tahap, firmware membandingkan **jumlah langkah yang
diminta** dengan **jumlah pulsa yang benar-benar keluar**. Kalau
berbeda, tahap itu ditandai meleset.

Uji berhenti lebih awal kalau **dua tahap berturut-turut** sudah
meleset, supaya motor tidak dipaksa terus saat sudah jelas tidak kuat.

Seluruh proses sekitar 30 detik.

### Membaca hasilnya

Panel di `/debug` menampilkan ringkasan, dan log memuat rincian per
tahap seperti:

```
[SWEEP] 600 step/s TEPAT 800 langkah
[SWEEP] 1600 step/s MELESET -12 langkah (minta 800, dapat 788)
[SWEEP] Kecepatan aman tertinggi: 1200 step/s
[SWEEP] Di atas itu mulai kehilangan langkah. Pakai 960 step/s untuk KIRI.
```

### Tiga kemungkinan kesimpulan

**Semua tahap tepat.** Motor kuat sampai 2000 step/detik. Kecepatan
bukan masalah.

**Sebagian meleset, sebagian tepat.** Ketemu batasnya. Pakai sekitar
80 persen dari kecepatan aman tertinggi supaya ada cadangan tenaga.

**Meleset di semua kecepatan, termasuk 200 step/detik.** Ini yang
paling penting: **masalahnya bukan kecepatan, melainkan arus**.
Periksa DIP switch SW1-SW3, dan pertimbangkan turunkan microstep.

## Catatan penting

Uji ini menggerakkan motor **tanpa beban lid** kalau lid dilepas. Hasil
pada kondisi tanpa beban adalah **batas atas**: begitu lid dipasang,
batas sebenarnya akan lebih rendah.

Untuk hasil yang paling berguna, jalankan uji dengan **lid terpasang**,
karena itulah kondisi kerja sebenarnya.

## Batas yang perlu disadari

Posisi lid dihitung dari **jumlah pulsa yang dikirim**, bukan dari
sensor. Jadi kalau motor kehilangan langkah secara mekanis, firmware
tetap melaporkan posisi sesuai target. Uji `Q` ini bekerja justru
karena membandingkan pulsa yang dikirim dengan pulsa yang seharusnya,
sehingga kehilangan langkah **di dalam firmware** bisa terdeteksi.

Kehilangan langkah yang terjadi **di dalam driver atau di rotor**
(motor diam padahal pulsa terkirim) tidak bisa dideteksi tanpa sensor
posisi. Untuk itu perlu encoder atau homing ke stop mekanis.
