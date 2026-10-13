# Mengatur Torsi Stepper: Apa yang Bisa dan Tidak Bisa

## Jawaban singkat

Torsi stepper **tidak bisa diatur dari firmware**. Torsi ditentukan
oleh perangkat keras, bukan oleh program.

Uji `Q` yang baru ditambahkan **mengukur** pada kecepatan berapa motor
mulai kehilangan langkah. Uji itu tidak menambah tenaga.

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

## Urutan perbaikan kalau torsi kurang

| Urutan | Tindakan | Perkiraan manfaat |
|---|---|---|
| **1** | Naikkan arus TB6600 lewat SW1-SW3 ke 1.5 sampai 2.0 A | **Terbesar dan paling murah** |
| 2 | Turunkan microstep dari 16 ke 8 | Besar |
| 3 | Naikkan tegangan suplai motor ke 24 V | Sedang |
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
