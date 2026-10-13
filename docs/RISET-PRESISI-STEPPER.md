# Riset Presisi Posisi Lid Stepper (TB6600 + NEMA 17 + ESP32)

## Masalah yang Diriset

Firmware `SmartTrashbin-WiFi.ino` menggerakkan dua lid dengan stepper NEMA 17 lewat driver
TB6600 (microstep 16, 3200 pulse/rev, `STEPS_PER_DEG = 8.8889`). Pengguna memberi perintah
OPEN lalu CLOSE berulang kali. Sekitar 80 persen siklus lid kembali tepat ke titik awal,
tetapi sekitar 20 persen berhenti di posisi yang sedikit bergeser, seolah kehilangan
beberapa step.

Dokumen ini mencari sumber primer (datasheet, dokumen resmi, source code resmi) untuk
menjawab tujuh pertanyaan riset: penyebab step loss di TB6600, waktu setup DIR, mekanisme
kehilangan step di ESP32, pengaruh microstepping terhadap presisi, cara menutup loop posisi,
pembangkitan pulsa deterministik, dan optimasi firmware yang ada.

Penting dan harus disampaikan sejak awal: posisi lid saat ini dihitung dari **jumlah pulsa
yang DIKIRIM ke driver**, bukan dari sensor posisi. Karena itu, langkah yang hilang di sisi
mekanik tidak terlihat oleh firmware sama sekali. Firmware akan selalu mengira lid berada di
posisi target, walaupun rotor sebenarnya tidak sampai ke sana. Ini adalah batas fundamental
dari sistem open loop, dan sebagian besar rekomendasi di bawah berkaitan dengan itu.

Catatan tentang kejujuran sumber: mesin pencari `web_search` di lingkungan ini gagal total
("Every engine for the web source failed"), dan beberapa situs vendor (Mouser, DigiKey,
Toshiba, alldatasheet, Oriental Motor, Trinamic) menolak akses otomatis dengan HTTP 403.
Klaim di bawah hanya diberi sitasi jika saya benar-benar berhasil membaca isi dokumennya.
Klaim yang tidak bisa saya verifikasi ditandai secara eksplisit.

---

## 1. TB6600 dan Step Loss: Timing PUL, DIR, dan Microstepping pada Arus Rendah

### 1.1 Datasheet asli TB6600HG (Toshiba)

Saya berhasil mengunduh dan membaca datasheet asli Toshiba TB6600HG. Berkasnya berjudul
"TB6600HG PWM Chopper-Type bipolar Stepping Motor Driver IC", bertanggal 2014-01-30.
PDF resmi tersedia di beberapa mirror, salah satu yang bisa saya baca isinya:
`https://www.makerguides.com/wp-content/uploads/2019/10/TB6600HG-Datasheet.pdf`
(salinan lain: `https://data.emotion-tech.com/ftp/Datasheets_et_sources/Datasheets_electronique/Datasheet_Toshiba_TB6600.pdf`).
Halaman produk resmi Toshiba: `https://toshiba.semicon-storage.com/ap-en/semiconductor/product/motor-driver-ics/stepping-motor-driver-ics/detail.TB6600H.html`

Temuan dari tabel **Electrical Characteristics** (Ta = 25 C, Vcc = 24 V):

- **Minimum CLK pulse width**: `twCLKH` dan `twCLKL`, Min = **2.2 us**, tanpa nilai Max.
  Ini satu-satunya batas lebar pulsa di seluruh datasheet. Firmware memakai
  `PULSE_MIN_US = 10`, jadi sudah lebih dari 4 kali batas minimum. Dari sisi datasheet,
  lebar pulsa bukan penyebab masalah.
- **Input voltage High** `VIH`: Min **2.0 V**, Max 5.5 V, untuk pin M1, M2, M3, CW/CCW,
  CLK, RESET, ENABLE, Latch/Auto, TQ.
- **Input voltage Low** `VIL` Min -0.2 V, Max **0.8 V**.
- **Input hysteresis voltage** `VH`: sekitar 400 mV.
- **Input current** pada VIN = 5.0 V: tipikal 50 sampai 75 uA (70 sampai 105 uA untuk TQ).

Implikasi penting: `VIH` minimum hanya 2.0 V, dan hysteresis sekitar 400 mV. Sinyal 3.3 V
dari ESP32 secara **spesifikasi datasheet sudah memenuhi** level logika HIGH TB6600HG, bukan
di bawahnya. Jadi pernyataan di dokumen proyek sebelumnya (`MENGATASI-STEPPER-BERGETAR.md`)
bahwa penyebab nomor satu adalah "3.3V terlalu lemah" tidak didukung oleh tabel datasheet
TB6600HG itu sendiri. Yang benar adalah datasheet menyebut 2.0 V sebagai ambang HIGH, dan
3.3 V memberi margin sekitar 1.3 V di atasnya. Ini tidak berarti wiring pasti baik: margin
terhadap noise, panjang kabel, dan arus optocoupler tetap isu nyata, tetapi alasan teknisnya
bukan "3.3 V di bawah ambang logika".

### 1.2 Waktu setup DIR: klaim "5 us" dan "2.2 us" perlu dikoreksi

Ini temuan yang paling penting untuk dokumen ini, dan berlawanan dengan catatan sebelumnya.

**Datasheet TB6600HG tidak memuat parameter waktu setup atau hold untuk DIR.** Alasan
teknisnya: pin nomor 22 pada TB6600HG **bukan bernama DIR**, melainkan **CW/CCW**, dan
deskripsinya di tabel pin adalah:

> "22 Input CW/CCW Forward/reverse control pin L: CW, H: CCW"

Ini adalah **input level**, bukan input yang harus di-setup relatif terhadap tepi pulsa.
Tidak ada satu pun nilai numerik setup time atau hold time di dokumen itu. Saya sudah
memeriksa seluruh teks tabel Electrical Characteristics, bagian Timing Waveforms, dan
"Description of Functions", dan yang ada hanya `twCLKH`/`twCLKL` (2.2 us) serta timing
gelombang CLK.

Konsekuensinya, angka "DIR stabil 5 us sebelum pulsa pertama" yang tertulis di
`MENGATASI-STEPPER-BERGETAR.md` baris 98 **tidak berasal dari datasheet TB6600HG**. Angka
itu adalah praktik umum yang diwariskan dari driver step/dir lain. Saya **tidak bisa
memverifikasi** angka 5 us sebagai rekomendasi TB6600 dari sumber primer. Saya juga tidak
bisa membuktikan bahwa angka itu salah secara praktik, hanya bahwa datasheet TB6600 tidak
menyebutkannya.

Pembanding dari driver lain yang **memang** mencantumkan parameter ini secara eksplisit
adalah TI DRV8825 (SLVSA73F), datasheet di `https://www.ti.com/lit/ds/symlink/drv8825.pdf`,
tabel 7.6 Timing Requirements:

- Pulse duration, STEP high: Min **1.9 us**
- Pulse duration, STEP low: Min **1.9 us**
- Setup time, command before STEP rising: Min **650 ns**
- Hold time, command after STEP rising: Min **650 ns**
- Enable time, nENBL active to STEP: 650 ns
- Step frequency: Max 250 kHz

Perhatikan bahwa bahkan TI, yang mencantumkan parameter ini, menyebutnya **650 ns**, bukan
5 us. Dari sini bisa disimpulkan bahwa "beberapa mikrodetik" adalah angka yang sangat
konservatif, dan 650 ns adalah nilai yang dijamin dokumen TI. Untuk TB6600HG sendiri,
karena CW/CCW adalah input level murni, waktu setup 5 us yang sudah ada di firmware
kemungkinan besar sudah sangat memadai, tetapi **statusnya adalah asumsi konservatif, bukan
persyaratan yang terdokumentasi**.

Satu detail TB6600HG yang justru terdokumentasi dan jarang diperhatikan:

> "Input signal is not accepted for about 200 us after releasing the standby mode."

Jika firmware pernah memakai mode standby lewat kombinasi M1/M2/M3, maka ada jendela 200 us
di mana input diabaikan. Pada DIP switch microstep 16 normal (M1/M2/M3 pada setelan tetap),
mode standby tidak aktif, jadi ini kemungkinan tidak relevan, tetapi perlu diperiksa pada
setelan DIP switch fisik.

### 1.3 Interaksi microstepping dengan arus rendah

Datasheet TB6600HG menyebut dirinya "PWM chopper-type single-chip bipolar **sinusoidal**
micro-step stepping motor driver" dengan pilihan eksitasi "1/1, 1/2, 1/4, 1/8, dan 1/16 step".
Datasheet menyatakan arus diatur oleh tegangan Vref pada pin Vref, dengan:
`VNF` (current limit voltage) = 0.9 sampai 1.1 V pada Vref = 3.0 V, TQ = H, dan
**divider ratio Vref/VNF = 3** (jadi pembagi arus penuh adalah Vref/3).

Yang **tidak** ada di datasheet TB6600HG: tidak ada satu pun pembahasan bahwa pada arus
rendah motor akan "menempel" ke full step terdekat dan mengabaikan microstep. Datasheet juga
tidak membahas akurasi posisi absolut microstep. Jadi soal ini harus diambil dari sumber
lain, lihat bagian 4.

---

## 2. Waktu Setup DIR dalam Praktik

Ringkasan dari bagian 1:

1. **Apakah firmware sekarang menyetel DIR lalu langsung memberi pulsa?** Ya. Di
   `stepperMoveToDeg()` (sekitar baris 840) firmware menyetel `s.dir` dan pin DIR, lalu
   memberi jeda kecil sebelum pulsa pertama. Berdasarkan struktur kode, urutannya adalah
   set DIR, tunggu (diklaim 5 us), lalu mulai pulsa.

2. **Berapa delay yang direkomendasikan datasheet TB6600?** Datasheet TB6600HG tidak
   mencantumkan angka apa pun untuk ini, karena pin-nya adalah **CW/CCW** (input level),
   bukan DIR dengan parameter setup. Perhatikan bahwa nama "DIR" di header wiring firmware
   (`Lid KIRI: PUL=18 DIR=19 ENA=23`) adalah penamaan di sisi board modul TB6600, bukan
   nama pin chip TB6600HG.

3. **Apakah "5 us" benar?** Tidak terverifikasi di sumber primer TB6600. Sebagai pembanding
   terdokumentasi, TI DRV8825 hanya menuntut **650 ns** setup sebelum tepi naik STEP. Angka
   5 us aman secara praktik tetapi bukan persyaratan datasheet TB6600.

4. **Apakah DIR setup time kemungkinan penyebab 20 persen step loss?** Kemungkinan kecil,
   dan ada alasan kuat: step loss muncul saat perintah OPEN/CLOSE bergantian, dan arah
   (DIR) hanya berubah sekali di awal setiap gerakan, bukan di setiap pulsa. Kalau DIR setup
   time jadi masalah, gejalanya adalah pulsa pertama salah arah, bukan kehilangan beberapa
   step di akhir gerakan. Kehilangan beberapa step di akhir gerakan lebih cocok dengan
   masalah torsi/akselerasi (bagian 4) dan gangguan timing di ESP32 (bagian 3).

Yang perlu diperiksa di perangkat, bukan diasumsikan: apakah modul TB6600 yang dipakai
memang modul dengan rangkaian optocoupler tambahan di depan input chip. Banyak modul
TB6600 siap pakai menambahkan rangkaian input (sering dengan resistor dan optocoupler) yang
**tidak** ada di datasheet chip TB6600HG, sehingga timing efektifnya bisa berbeda dari
datasheet chip. Untuk itu, datasheet chip tidak sepenuhnya bisa dipakai sebagai acuan
tunggal. Saya **tidak bisa memverifikasi** skematik modul tertentu milik pengguna.

---

## 3. Mekanisme Kehilangan Step di ESP32 (Task FreeRTOS)

### 3.1 Cache dimatikan saat operasi flash: ini terdokumentasi dan relevan langsung

Ini temuan paling kuat untuk pertanyaan ini. Dokumentasi resmi Espressif "Concurrency
Constraints for Flash on SPI0/1":
`https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/spi_flash/spi_flash_concurrency.html`

Kutipan penting, verbatim:

> "Caches are disabled during all SPI1 operations. **Most tasks will be disabled, and access
> to Flash/PSRAM is forbidden.**"

> "When the caches are disabled, all non-IRAM-safe interrupts will be disabled, and all
> other tasks are suspended. **The other core will be polling in a busy loop.** Only
> IRAM-safe interrupt handlers will be executed."

Ini menjelaskan dua hal sekaligus:

1. Operasi tulis flash (termasuk `esp_flash_write()`, `esp_flash_erase_region()`) mematikan
   cache, dan selama itu **task lain di-suspend** dan **core lain berputar di busy loop**.
2. Karena itu, setiap operasi tulis flash dapat menghentikan task pulsa stepper.

Dan ini **sangat relevan** dengan firmware ini: `simpanSudutGlobal()` memanggil
`Preferences::putFloat()`, yang menulis ke NVS di flash, dan ini dipanggil dari
`executeCommand()` pada perintah `W` (baris sekitar 1653). Ditambah lagi, halaman dokumentasi
yang sama menyebut CPU dan cache harus menunggu sampai perintah tulis flash selesai.

Konsekuensi praktis: jika ada perintah `W` (set sudut) atau operasi NVS lain berdekatan
dengan gerakan lid, task stepper bisa terhenti selama seluruh durasi tulis flash, dan pulsa
yang jatuh tempo selama jendela itu hilang. Ini adalah kandidat penyebab step loss yang
terdokumentasi di sumber primer, bukan spekulasi.

Catatan penting: log firmware sendiri melaporkan jitter maksimum hanya 4 us dengan 0 dari
23934 pulsa yang terlambat. Tetapi pengukuran itu dilakukan pada skenario uji tertentu.
Jika pengukuran itu tidak dijalankan bersamaan dengan operasi tulis flash atau dengan trafik
WiFi berat, maka jitter 4 us itu tidak mewakili kondisi terburuk. Saya **tidak bisa
memverifikasi** kondisi pengukuran jitter tersebut dari dokumen yang tersedia.

### 3.2 Interrupt latency ESP32: angka terukur tidak saya temukan di dokumen resmi

Saya mencari angka interrupt latency di ESP32 Technical Reference Manual
(`https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf`)
dan ESP32 datasheet
(`https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf`),
serta halaman "High Priority Interrupts" ESP-IDF
(`https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/hlinterrupts.html`).

**Saya tidak menemukan angka interrupt latency yang dijamin secara numerik di dokumen resmi
tersebut.** TRM memang menjelaskan arsitektur interrupt (level 1 sampai 7, NMI) tetapi tidak
memberi angka latency dalam mikrodetik. Jadi: klaim "interrupt latency ESP32 sekitar X us"
**tidak dapat saya buktikan dari sumber primer Espressif** dan sebaiknya tidak dipakai
sebagai dasar keputusan. Yang bisa diandalkan adalah pernyataan kualitatif dari dokumen
flash di atas dan dari dokumen esp_timer di bawah.

Dokumen resmi esp_timer (`https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/esp_timer.html`)
memberi panduan yang sangat relevan:

> "For even smaller timeout values, for example, to generate or receive waveforms or do bit
> banging, the resolution of ESP Timer may be insufficient. In this case, it is recommended
> to use dedicated peripherals, such as GPTimer or RMT, and their DMA features if available."

Dokumen yang sama juga menyatakan bahwa metode Task Dispatch (default) "Suitable for handling
timer callbacks that are not time-critical", dan bahwa "**If other FreeRTOS tasks with higher
priority are running, such as an SPI flash operation, callback dispatching will be delayed**".

Perhatikan kalimat terakhir itu: Espressif sendiri secara eksplisit menyebut operasi SPI
flash sebagai contoh hal yang menunda eksekusi. Ini konsisten dengan bagian 3.1.

### 3.3 ESP_TIMER_ISR tidak tersedia: klaim proyek terverifikasi

Firmware menyatakan `ESP_TIMER_ISR` tidak tersedia di core versi ini. Ini **terverifikasi
sepenuhnya** di source code resmi.

Di header ESP-IDF, `esp_timer.h` (saya baca dari `release/v5.5`, yang merupakan basis
Arduino core 3.x):
`https://raw.githubusercontent.com/espressif/esp-idf/release/v5.5/components/esp_timer/include/esp_timer.h`

```c
typedef enum {
    ESP_TIMER_TASK,     //!< Callback is dispatched from esp_timer task
#if CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD || __DOXYGEN__
    ESP_TIMER_ISR,      //!< Callback is dispatched from interrupt handler
#endif
    ESP_TIMER_MAX,      //!< Sentinel value for the number of callback dispatch methods
} esp_timer_dispatch_t;
```

Dan di Kconfig ESP-IDF
(`https://raw.githubusercontent.com/espressif/esp-idf/release/v5.5/components/esp_timer/Kconfig`):

```
config ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
    bool "Support ISR dispatch method"
    default n
```

`default n` berarti nonaktif. Karena Arduino-ESP32 memakai library ESP-IDF yang sudah
dibangun sebelumnya (prebuilt) tanpa fitur eksperimental ini, `ESP_TIMER_ISR` tidak
tersedia saat kompilasi. Jadi **pilihan ISR dispatch untuk pulsa stepper memang tertutup**
pada setup ini, dan ini terdokumentasi, bukan asumsi. Ini memperkuat kasus untuk beralih ke
peripheral keras (bagian 6).

### 3.4 WiFi, task affinity, dan lintas core

Dokumentasi RF Coexistence Espressif
(`https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/coexist.html`)
menyatakan bahwa WiFi dan Bluetooth berbagi satu modul RF 2.4 GHz:

> "Each type of board has only one 2.4 GHz ISM band RF module, shared by two or three
> modules. Consequently, a module cannot receive or transmit data while another module is
> engaged in data transmission or reception. In such scenarios, ESP32 employs the
> time-division multiplexing method to manage the reception and transmission of packets."

Jadi WiFi dan BLE di firmware ini memang saling berebut radio, dan berjalan time-division.
Dokumen yang sama juga menyebut WiFi dan BLE masing-masing punya time slice, dan pada skenario
"Wi-Fi CONNECTED and BLE CONNECTED", "the time slices of Wi-Fi and BLE in a coexistence
period each account for 50%".

Namun, penting untuk tidak melebih-lebihkan: **saya tidak menemukan pernyataan resmi
Espressif yang menjamin bahwa aktivitas radio WiFi/BLE akan mempreempt task yang di-pin ke
core lain dengan cara tertentu pada tingkat mikrodetik.** Yang terdokumentasi adalah
time-division penggunaan radio dan bahwa operasi flash memblokir kedua core. Klaim bahwa
"WiFi di core 0 menyebabkan step loss di core 1" sebagai mekanisme langsung
**tidak dapat saya buktikan dari sumber primer**; yang bisa dibuktikan adalah mekanisme
tidak langsung lewat operasi flash dan lewat beban CPU WiFi.

### 3.5 Preemption task dan `portENTER_CRITICAL`

Firmware memakai `xTaskCreatePinnedToCore(..., priority 3)` dan spin loop saat bergerak.
Saya tidak menemukan dokumen resmi Espressif yang mengukur secara numerik berapa lama task
prioritas 3 pada core 1 bisa tertunda. Yang terdokumentasi adalah prinsip operasi flash
(bagian 3.1) dan penggunaan `portENTER_CRITICAL` untuk melindungi section kritis.

Catatan penting soal `portENTER_CRITICAL` pada ESP-IDF dual core: secara default, critical
section hanya menonaktifkan interrupt pada **core yang memanggilnya**, bukan di kedua core.
Saya **tidak berhasil menemukan satu halaman dokumentasi resmi** yang saya baca yang
menyatakan angka latency maksimum yang dijamin untuk kondisi ini, jadi saya tidak
mencantumkan angka. Ini perlu diperiksa di perangkat (lihat bagian "Yang Tidak Bisa
Dibuktikan Tanpa Perangkat").

---

## 4. Microstepping dan Presisi: Apakah Microstep 16 Meningkatkan Akurasi?

Ini bagian yang paling langsung menjelaskan gejala 20 persen, dan sumbernya jelas.

### 4.1 Faulhaber Application Note 015 (produsen motor stepper)

Sumber: `https://www.faulhaber.com/fileadmin/Import/Media/AN015_EN.pdf`
Judul: "Application Note 015, Microstepping". Faulhaber adalah produsen motor stepper,
jadi ini sumber pihak pertama untuk perilaku motor.

Pernyataan kunci, verbatim:

> "While microstepping control allows an increase of positioning **resolution**, the
> **accuracy** limitations linked to distortion need to be taken into account."

> "In case of incremental movement displacement angle lower than a full step, load torque
> will result in a keeping the rotor in position until sufficient torque is generated."

> "Simply stated, taking a microstep does not mean the motor will actually move! And if
> reversing direction, motor shaft torque must be decremented from whatever positive value
> it has to a negative value that will have sufficient torque to cause motion in the
> negative direction."

> "Well, the stepper motor still has friction torque due to its bearings and it has a
> **detent torque, in addition to torque to overcome the bearing friction. Even more
> disruptive than the bearing friction is the detent torque, which is typically 5 to 20% of
> the holding torque.** Sometimes the detent torque is adding to the overall torque
> generation, sometimes it is sub-tracting from the powered torque generation. **In any case
> it wrecks havoc with your overall accuracy.**"

> "In summary, **although Microstepping gives the designer more resolution, improved
> accuracy is not realized.** Reduction in mechanical and electromagnetically induced noise
> is, however, a real benefit."

Tabel 1 dari dokumen itu, "Incremental torque per microstep as the number of microsteps per
full step increases":

| Microsteps/full step | % full step torque per microstep |
|---|---|
| 1 | 100.00 % |
| 2 | 70.71 % |
| 4 | 38.27 % |
| 8 | 19.51 % |
| **16** | **9.80 %** |
| 32 | 4.91 % |
| 64 | 2.45 % |
| 128 | 1.23 % |
| 256 | 0.61 % |

Dan dokumen itu menyimpulkan secara eksplisit: "At 16 Microsteps per Full Step the
Incremental Torque for One Microstep is less than 10% of the Full Step Holding Torque".

### 4.2 Mengapa ini menjelaskan gejala 20 persen

Dari tabel Faulhaber, pada microstep 16 setiap microstep hanya menghasilkan kurang dari
10 persen torsi holding. Pada saat yang sama, dokumen yang sama menyatakan **detent torque
tipikal 5 sampai 20 persen dari holding torque**.

Artinya, torsi satu microstep (di bawah 10 persen) berada **di dalam atau di bawah rentang
detent torque (5 sampai 20 persen)**. Jadi wajar bahwa sebagian microstep tidak
menghasilkan gerakan rotor sama sekali, terutama saat:
- arah dibalik (OPEN lalu CLOSE), karena torsi harus melintasi nol dan melawan detent,
- ada beban lid (berat tutup, gesekan engsel),
- arus driver rendah (Vref rendah),
- kecepatan rendah mendekati akhir gerakan.

Ini adalah penjelasan mekanis yang cocok dengan gejala: 80 persen siklus kembali tepat,
20 persen bergeser beberapa step, dan pergeseran hanya terlihat setelah gerakan bolak-balik.
Faulhaber menyebutkan "taking a microstep does not mean the motor will actually move" dan
"if reversing direction, motor shaft torque must be decremented... to a negative value that
will have sufficient torque".

### 4.3 Subtlety penting: microstep 16 menurunkan torsi per microstep

Kesimpulan yang praktis berlawanan dengan intuisi: **menaikkan microstep 16 ke angka yang
lebih tinggi akan memperburuk**, karena torsi per microstep makin kecil (tabel Faulhaber).
Sebaliknya, **menurunkan microstep dari 16 ke 8 atau 4 akan menaikkan torsi per microstep**
(19.51 persen pada 8, 38.27 persen pada 4), dengan biaya resolusi lebih kasar.

Pada konteks aplikasi ini, resolusi 3200 pulse/rev untuk membuka lid ternyata jauh melebihi
kebutuhan. Untuk akurasi **absolut dan repetabilitas**, sumber ini menyatakan microstepping
tinggi **tidak** membantu, yang membantu adalah torsi per langkah, dan itu lebih baik pada
microstep rendah.

### 4.4 Konfirmasi dari Marlin (firmware motion control resmi)

Sumber: `https://raw.githubusercontent.com/MarlinFirmware/Marlin/bugfix-2.1.x/Marlin/Configuration_adv.h`
(berkas konfigurasi resmi firmware Marlin, dipakai luas di printer 3D dan CNC). Komentarnya:

> "Improve homing repeatability by homing to stepper coil's nearest absolute phase position.
> Trinamic drivers use a stepper phase table with 1024 values spanning 4 full steps with 256
> positions each (ergo, 1024 positions). **Full step positions (128, 384, 640, 896) have the
> highest holding torque.**"

Ini adalah pernyataan pihak pertama dari proyek motion control besar yang secara langsung
menyatakan bahwa **posisi full step memiliki torsi holding tertinggi**, dan bahwa menuju
posisi itu meningkatkan **repetabilitas** homing. Ini konsisten dengan Faulhaber di 4.1 dan
mengarah ke rekomendasi yang sama.

### 4.5 Sumber yang tidak bisa saya verifikasi

- Saya **tidak bisa** memverifikasi klaim umum bahwa "akurasi stepper sekitar 1/10 full step"
  dari satu dokumen pihak pertama tertentu. Angka itu beredar luas, tetapi saya tidak
  menemukannya di dokumen yang berhasil saya baca. Yang bisa saya dukung hanyalah pernyataan
  Faulhaber di 4.1 dan Marlin di 4.4.
- Datasheet TB6600HG tidak memuat informasi akurasi microstep maupun pengaruh arus rendah,
  jadi untuk bagian 4 ini datanya harus diambil dari pihak ketiga (Faulhaber, Marlin), bukan
  dari pembuat TB6600. Ini keterbatasan yang perlu dicatat.

---

## 5. Menutup Loop: Mendeteksi Posisi Sebenarnya

### 5.1 Sensor optik reflektif: TCRT5000 (datasheet produsen tersedia)

Sumber: `https://www.vishay.com/docs/83760/tcrt5000.pdf` (Vishay Semiconductors, Document
Number 83760, Rev. 1.7, 17-Aug-09).

Spesifikasi relevan dari datasheet:
- "Reflective Optical Sensor with Transistor Output" (emitter inframerah + fototransistor).
- Peak operating distance: **2.5 mm**; operating range dengan relative collector current
  di atas 20 persen: **0.2 mm sampai 15 mm**.
- Emitter wavelength: 950 nm.
- Ada "daylight blocking filter".

Yang paling relevan: bagian APPLICATIONS di datasheet itu sendiri menyebut:
> "**Limit switch for mechanical motions** in VCR"
> "Position sensor for shaft encoder"

Jadi Vishay sendiri mencantumkan fungsi limit switch sebagai aplikasi resmi chip ini.
Tradeoff: murah, 1 pin GPIO digital. Tetapi hanya memberi sinyal biner (ada atau tidak ada
tanda), tidak memberi sudut absolut. Bergantung pada kontras reflektansi permukaan dan jarak
2.5 mm, jadi butuh penempatan mekanis yang tepat dan bisa terpengaruh cahaya sekitar
walau ada daylight filter. Kompleksitas kode: rendah, cukup baca satu GPIO.

### 5.2 Sensor magnetik absolut: AS5600

AS5600 adalah sensor sudut absolut 12-bit dengan antarmuka I2C, dari ams-OSRAM.
Halaman produk resmi: `https://ams-osram.com/products/sensor-solutions/position-sensors/ams-as5600-position-sensor`

**Kejujuran sumber:** saya **berhasil mengunduh halaman produk resminya**, tetapi halaman
itu bergantung JavaScript sehingga teks spesifikasinya tidak bisa saya baca sebagai teks
sederhana. Upaya mengunduh PDF datasheet AS5600 dari `ams.com`, mirror Mouser, dan SparkFun
gagal (404, 403, atau berkas yang diunduh ternyata bukan PDF). Karena itu saya **tidak bisa
memverifikasi angka 12-bit, jumlah langkah, atau spesifikasi I2C AS5600 pada sumber primer**
dalam sesi ini. Saya hanya bisa menyatakan bahwa chip ini diproduksi oleh ams-OSRAM dan
dipasarkan sebagai sensor sudut absolut magnetik, dan bahwa ini adalah kandidat yang tepat
untuk aplikasi ini. Verifikasi datasheet AS5600 dari situs resmi ams-OSRAM perlu dilakukan
sebelum memakai spesifikasinya.

Tradeoff jika dipakai: memberi sudut absolut penuh 0 sampai 360 derajat, sehingga bisa
mengoreksi posisi kapan saja, tidak hanya pada satu titik (bukan hanya homing). Magnet
dipasang di poros, sensor di PCB menghadap poros. Butuh I2C (2 pin) dan kode pembacaan, plus
kalibrasi offset. Kelemahan: menambah komponen mekanis (magnet dan jarak celah) dan
menambah I2C di bus yang juga dipakai untuk hal lain. Ini adalah opsi paling kuat untuk
"menutup loop", karena memberi umpan balik posisi kontinu, bukan hanya titik referensi.

### 5.3 Encoder rotary inkremental pada poros stepper

Pendekatan: memasang encoder inkremental pada poros stepper dan menghitung pulsa encoder
untuk mendeteksi perbedaan antara pulsa yang dikirim dan gerakan sebenarnya. Ini adalah cara
paling langsung untuk mengukur step loss secara kuantitatif. Namun, **saya tidak berhasil
mengunduh datasheet encoder spesifik** (misal keluarga HEDS) dalam sesi ini karena batas
akses situs, jadi saya tidak bisa mengutip part number dan spesifikasinya dari sumber primer.
Saya menyebut pendekatan ini sebagai teknik yang dikenal, dan **menyatakan eksplisit bahwa
saya tidak memverifikasi datasheet part tertentu**.

Tradeoff: ini memberi diagnosis sejati atas masalah (bisa membuktikan berapa step yang
hilang per siklus), tetapi menambah kode penghitung quadrature dan pin, serta pemasangan
mekanis pada poros. Kompleksitas kode: paling tinggi di antara opsi di bagian ini.

### 5.4 Homing ke stop mekanis: pola yang dikenal dan terdokumentasi

**Ya, ini pola yang diakui dan terdokumentasi**, dan cara penggunaannya untuk meningkatkan
repetabilitas juga terdokumentasi di source code resmi.

Sumber: `https://raw.githubusercontent.com/MarlinFirmware/Marlin/bugfix-2.1.x/Marlin/src/module/motion.cpp`
fungsi homing Marlin. Komentar di kode itu menerangkan urutan langkahnya secara eksplisit:

```
// Fast move towards endstop until triggered
do_homing_move(axis, move_length, 0.0, !use_probe_bump);

// If a second homing move is configured...
if (bump) {
  // Move away from the endstop by the axis HOMING_BUMP_MM
  do_homing_move(axis, -bump, ...);

  // Slow move towards endstop until triggered
  const float rebump = bump * 2;
  do_homing_move(axis, rebump, get_homing_bump_feedrate(axis), true);
}
```

Dan `get_homing_bump_feedrate()` di berkas yang sama menyatakan komentarnya:
```
/**
 * Homing bump feedrate (mm/s)
 */
```
dengan pembagi kecepatan `HOMING_BUMP_DIVISOR`, yang didefinisikan di
`https://raw.githubusercontent.com/MarlinFirmware/Marlin/bugfix-2.1.x/Marlin/Configuration_adv.h`:

```
#define HOMING_BUMP_MM      { 5, 5, 2 }       // (linear=mm, rotational=°) Backoff from endstops after first bump
#define HOMING_BUMP_DIVISOR { 2, 2, 4 }       // Re-Bump Speed Divisor (Divides the Homing Feedrate)
```

Jadi pola yang terdokumentasi di Marlin adalah: **sentuh stop dengan cepat, mundur sedikit,
lalu sentuh ulang dengan lambat** (kecepatan homing dibagi `HOMING_BUMP_DIVISOR`). Alasan
teknisnya: ayunan mekanis dan overshoot bergantung pada kecepatan, jadi pendekatan lambat
ke stop menghasilkan posisi berhenti yang jauh lebih konsisten daripada pendekatan cepat.

**Pro dan kontra dari "re-home ke stop mekanis setiap close":**

- Pro: tidak butuh sensor tambahan sama sekali (hanya stop mekanis, yang sudah ada pada
  banyak desain lid, yaitu batas engsel atau permukaan yang menahan). Menghapus akumulasi
  error sepenuhnya, karena setiap close mengulang dari referensi absolut yang sama. Pola
  yang sangat mapan dan terbukti.
- Kontra: menutup lid harus menyentuh stop dengan gaya tertentu setiap kali, yang menambah
  keausan mekanis dan memerlukan struktur yang kuat. Perlu dua tahap (cepat lalu lambat)
  untuk akurat, sehingga close memakan waktu lebih lama dibanding sekali gerakan. Juga harus
  memastikan motor tidak kehilangan sinkronisasi saat menekan stop, umumnya dengan
  menahan agar arus tetap ada atau mendeteksi bahwa motor sudah berhenti.
- Kontra penting untuk aplikasi ini: "menekan stop" berarti motor **sengaja kehilangan step
  dalam jumlah besar** setiap close, lalu mengoreksi posisi internal firmware ke titik nol.
  Ini bekerja hanya jika firmware tahu kapan stop tercapai. Tanpa sensor, firmware tidak
  tahu, dan justru menekan terus. Karena itu, homing ke stop **tetap butuh** semacam
  deteksi, entah saklar mekanis, sensor optik 5.1, atau deteksi kehabisan torsi.

### 5.5 Sensorless homing dan TB6600

Teknik "sensorless homing" memakai deteksi stall pada driver. Marlin menyebut fitur ini
untuk driver Trinamic (lihat `SENSORLESS_HOMING` di Configuration_adv.h). Trinamic
menyediakan fitur StallGuard untuk ini.

**TB6600 tidak mendukung sensorless homing.** Dasarnya langsung dari datasheet TB6600HG yang
sudah saya baca: pin keluaran monitoringnya hanya `ALERT` (tanda kesalahan seperti TSD, ISD,
UVLO) dan `MO` (electrical angle monitor), serta chip ini tidak punya antarmuka SPI, UART,
atau I2C sama sekali. Semua pilihan mode adalah pin strapping. Tidak ada register status torsi
dan tidak ada fitur deteksi beban. Jadi sensorless homing ala StallGuard **tidak tersedia**
di perangkat keras ini, dan tidak bisa diprogram lewat firmware.

Satu kemungkinan alternatif yang perlu dievaluasi di perangkat: mendeteksi bahwa motor tidak
lagi bergerak dengan mengamati apakah ALERT aktif, atau dengan asumsi lonjakan arus.
Tidak ada keduanya yang dijami oleh datasheet, jadi **tidak bisa saya rekomendasikan
berdasarkan sumber primer**.

---

## 6. Pembangkitan Pulsa Deterministik: RMT, MCPWM, LEDC

### 6.1 RMT di Arduino core 3.x: YA, bisa jumlah pulsa sembarang dengan durasi per pulsa

Ini jawaban tegas untuk pertanyaan eksplisit di tugas ini.

API RMT Arduino-ESP32 core 3.x didefinisikan di:
`https://raw.githubusercontent.com/espressif/arduino-esp32/release/v3.3.x/cores/esp32/esp32-hal-rmt.h`
dan diimplementasikan di:
`https://raw.githubusercontent.com/espressif/arduino-esp32/release/v3.3.x/cores/esp32/esp32-hal-rmt.c`

Struktur data satu simbol RMT (dari header, verbatim):

```c
// Each RMT Symbols has 4 bytes
typedef union {
  struct {
    uint32_t duration0 : 15;
    uint32_t level0    : 1;
    uint32_t duration1 : 15;
    uint32_t level1    : 1;
  };
  uint32_t val;
} rmt_data_t;
```

Poin krusial: **satu `rmt_data_t` berisi DUA pasangan (durasi, level)**. Jadi satu simbol
sudah bisa menyatakan "HIGH selama duration0, lalu LOW selama duration1". Karena masing
masing durasi 15 bit, satu simbol RMT secara teori bisa menyatakan satu pulsa lengkap dengan
lebar HIGH dan lebar LOW yang independen.

Fungsi tulisnya (dari header, verbatim):

```c
bool rmtWrite(int pin, rmt_data_t *data, size_t num_rmt_symbols, uint32_t timeout_ms);
bool rmtWriteAsync(int pin, rmt_data_t *data, size_t num_rmt_symbols);
bool rmtWriteLooping(int pin, rmt_data_t *data, size_t num_rmt_symbols);
bool rmtWriteRepeated(int pin, rmt_data_t *data, size_t num_rmt_symbols, uint32_t loop_count);
```

**Jawaban eksplisit untuk pertanyaan "apakah driver RMT TX di Arduino core 3.x bisa
mengeluarkan jumlah pulsa sembarang dengan durasi per pulsa":**

**YA.** Berdasarkan tanda tangan API di atas:
- `rmtWrite(pin, data, num_rmt_symbols, timeout)` mengirim sebanyak `num_rmt_symbols` simbol.
  Karena setiap simbol bisa berisi satu pulsa lengkap (HIGH dan LOW), jumlah pulsa yang
  dikirim sama dengan `num_rmt_symbols` yang ditentukan pemanggil. Jadi jumlah pulsa
  **sembarang**, tidak terbatas pada satu, dan ditentukan saat kompilasi maupun runtime.
- Durasi setiap pulsa **per pulsa independen**, karena setiap elemen array `rmt_data_t` punya
  `duration0` dan `duration1` sendiri. Jadi sebuah ramp bisa dikodekan langsung sebagai array
  simbol dengan lebar yang makin pendek atau makin panjang.
- `rmtWriteRepeated(..., loop_count)` memungkinkan mengulang satu buffer sebanyak
  `loop_count` kali, berguna untuk pola pulsa identik yang panjang tanpa membuat array besar.
- Implementasi di `esp32-hal-rmt.c` meneruskannya ke `rmt_transmit()` dengan
  `transmit_cfg.loop_count` dan encoder `rmt_copy_encoder_h`, yaitu encoder "copy" yang
  mengirim isi buffer apa adanya, sehingga durasi tiap simbol dihormati.

Batasan yang perlu diketahui: jumlah simbol dibatasi oleh memori blok RMT yang dicadangkan
(`rmt_reserve_memsize_t`, 1 sampai 8 blok), dan total kapasitas adalah
`RMT_SYMBOLS_PER_CHANNEL_BLOCK` simbol per blok. Untuk gerakan dengan ribuan pulsa, buffer
harus dipecah atau memakai `rmtWriteRepeated`, karena menampung 3200 simbol sekaligus dalam
satu blok mungkin tidak memungkinkan. Header juga menyatakan untuk ESP32 ada 8 MEM BLOCK
total yang dibagi antara Reading dan Writing. Jadi strategi yang realistis adalah mengisi
beberapa blok, mengirim, lalu mengisi lagi (ping-pong), atau mengirim per segmen ramp.
Perhitungan kapasitas persisnya perlu diverifikasi di perangkat.

Catatan penting soal RMT dan jumlah channel: ESP32 memiliki 8 channel RMT (4 TX dan 4 RX
pada ESP32), cukup untuk dua lid dengan dua pin PUL. Saya tidak bisa mengonfirmasi angka
persis pembagian TX/RX dari header ini saja, karena di header yang saya baca konstanta itu
bergantung pada `SOC_RMT_TX_CANDIDATES_PER_GROUP` dari lapisan SOC. Angka 4 TX / 4 RX adalah
yang umum diketahui untuk ESP32 klasik, tetapi **perlu diverifikasi** di
`soc/esp32/include/soc/soc_caps.h` untuk memastikannya.

### 6.2 RMT di sisi ESP-IDF: kapabilitasnya konsisten

Dokumentasi resmi RMT ESP-IDF:
`https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/rmt.html`

Dokumen itu menyatakan RMT "was designed to act as an infrared transceiver. However, due to
the flexibility of its data format, RMT can be extended to a versatile and general-purpose
transceiver", dan bahwa unit data minimalnya adalah simbol dengan pasangan durasi dan level.
Dokumen itu juga menyebut encoder dan mode ping-pong untuk mengatasi keterbatasan memori
blok, serta menyarankan menaruh fungsi encoder di IRAM untuk menghindari cache miss. Ini
mendukung rencana pemakaian RMT untuk pulsa step.

### 6.3 MCPWM

Dokumentasi resmi:
`https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/mcpwm.html`

Dokumen itu menyatakan MCPWM "is a versatile PWM generator" dengan Timer, Operator,
Comparator, Generator, Dead Time, Carrier Modulation, Fault, dan Capture sebagai submodul.
Skenario yang disebutkan mencakup "Digital motor control", "Generate Space Vector PWM".

Yang **tidak** ada di dokumen itu: tidak ada mekanisme penghitungan pulsa bawaan. MCPWM
menghasilkan gelombang PWM secara kontinu berdasarkan perbandingan nilai timer dengan
threshold comparator, bukan "kirim tepat N pulsa lalu berhenti". Jadi untuk aplikasi ini,
MCPWM bisa dipakai untuk menghasilkan frekuensi pulsa, tetapi **menghitung jumlah pulsa dan
berhenti tepat di N pulsa tidak disediakan sebagai fitur baku** (harus dihitung di software
lewat interrupt, atau menghentikan generator setelah N pulsa, yang mengembalikan masalah
timingnya ke software). Perlu dicatat bahwa saya **tidak menemukan** fitur hitung pulsa
eksplisit di dokumen yang saya baca; ini adalah kesimpulan dari apa yang ada maupun tidak ada
di dokumen, bukan kutipan.

MCPWM punya dua operator per grup, jadi dua channel independen tersedia, tetapi untuk dua lid
ini **RMT lebih cocok** karena RMT lebih langsung menyatakan "N pulsa dengan durasi X".

### 6.4 LEDC

Dokumentasi resmi: `https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/ledc.html`

Dokumen itu menyatakan: "The LED control (LEDC) peripheral is primarily designed to control
the intensity of LEDs, although it can also be used to generate PWM signals for other
purposes. It has 8 channels which can generate independent waveforms."

LEDC punya 8 channel independen, yang mana cukup untuk dua PUL (dan memang sudah dipakai
firmware ini untuk dua motor DC). Tapi LEDC adalah PWM kontinu berbasis duty cycle dan
frekuensi, **tidak punya penghitung pulsa dan tidak bisa berhenti otomatis setelah N pulsa**.
Jadi LEDC tidak memenuhi kebutuhan "jumlah pulsa terdefinisi, ramp, dua channel independen"
untuk stepper. LEDC hanya bisa memenuhi bagian "dua channel" dan bagian frekuensi.

### 6.5 Ringkasan kemampuan untuk tiga syarat yang diminta

| Peripheral | Jumlah pulsa terdefinisi | Ramp (durasi per pulsa) | Dua channel independen |
|---|---|---|---|
| **RMT (Arduino core 3.x)** | **Ya**, lewat `num_rmt_symbols` dan `rmtWriteRepeated` | **Ya**, tiap simbol punya `duration0`/`duration1` sendiri | **Ya** (beberapa kanal TX; angka persis perlu verifikasi di soc_caps) |
| **MCPWM** | Tidak ada penghitung pulsa baku; perlu software | Bisa, tapi lewat mekanisme compare yang lebih rumit | Ya (dua operator per grup) |
| **LEDC** | Tidak | Tidak (duty/frekuensi kontinu) | Ya (8 channel) |

Kesimpulan: **RMT adalah satu-satunya dari ketiganya yang memenuhi ketiga syarat secara
langsung di Arduino core 3.x**, dan kemampuannya mengeluarkan jumlah pulsa sembarang dengan
durasi per pulsa **terkonfirmasi dari source code resmi**, bukan dari blog.

---

## 7. Optimasi Firmware yang Ada

### 7.1 `IRAM_ATTR`: berguna untuk kode kritis, tapi ada biaya

Sumber: `https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/performance/speed.html`

Kutipan relevan, verbatim:

> "Move frequently executed code to IRAM. By default, all code in the app is executed from
> flash cache. This means that it is possible for the CPU to have to wait on a "cache miss"
> while the next instructions are loaded from flash. Functions which are copied into IRAM are
> loaded once at boot time, and then always execute at full speed. **IRAM is a limited
> resource, and using more IRAM may reduce available DRAM, so a strategic approach is needed
> when moving code to IRAM.**"

Jadi `IRAM_ATTR` **membantu** untuk fungsi yang dieksekusi sangat sering atau di dalam ISR
yang harus aman saat cache mati. Tetapi **merugikan** jika dipakai berlebihan, karena IRAM
terbatas dan memakainya mengurangi DRAM yang tersedia.

Untuk kasus ini: meletakkan fungsi pulsa inti di IRAM masuk akal hanya jika fungsi itu
benar-benar menjadi jalur panas. Namun karena fungsi pulsa saat ini adalah bagian dari task
FreeRTOS yang memanggil `digitalWrite()` dan `micros()` dari library Arduino, menandai satu
fungsi dengan `IRAM_ATTR` saja **tidak menjamin** seluruh call tree-nya ada di IRAM, dan
dokumentasi flash (bagian 3.1) menyatakan bahwa "You must ensure that all data and functions
accessed by these interrupt handlers, including the ones that handlers call, are located in
IRAM or DRAM" untuk handler yang aman saat cache mati. Jadi `IRAM_ATTR` pada satu fungsi
saja tidak menyelesaikan masalah cache; ia hanya mempercepat fungsi itu sendiri. Ini adalah
nuansa yang sering salah dipahami.

### 7.2 Prioritas task dan pinning core

Firmware sudah memakai `xTaskCreatePinnedToCore(..., priority 3, core 1)`, dan core 0 untuk
WiFi/HTTP. Ini sejalan dengan saran umum. Namun berdasarkan pencarian saya, **saya tidak
menemukan halaman dokumentasi resmi Espressif yang memberi angka prioritas tertentu yang
direkomendasikan untuk kerja latency sensitive**, maupun angka prioritas maksimum yang dijamin
aman terhadap task sistem. Yang bisa saya kutip adalah dokumen esp_timer (bagian 3.2) yang
menyebut bahwa task berprioritas lebih tinggi bisa menunda callback, dan dokumen flash
(bagian 3.1) yang menyatakan bahwa operasi flash men-suspend task lain dan membuat core lain
berputar di busy loop, terlepas dari prioritas. Jadi **prioritas lebih tinggi tidak
menyelamatkan dari operasi flash**, dan ini penting: menaikkan prioritas task stepper tidak
akan memperbaiki kehilangan step yang disebabkan tulis flash.

### 7.3 Watchdog

Saya tidak menemukan pernyataan resmi Espressif tentang interaksi langsung antara task
pulsa stepper dan watchdog task pada firmware ini. Dokumen yang saya baca
(`https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/wdts.html`)
tidak memberi angka yang bisa saya kutip untuk kasus ini. Karena firmware memakai spin loop
tanpa `vTaskDelay` saat bergerak, penting untuk dicatat bahwa spin loop pada satu task
**tidak** dengan sendirinya memicu task watchdog selama task itu sendiri memberi tanda
hidup ke scheduler dengan cara yang tepat; ini perlu diuji di perangkat, bukan diasumsikan.

### 7.4 Mematikan WiFi/BLE selama gerakan: apa yang bisa dan tidak bisa dibuktikan

Yang **bisa** dibuktikan dari sumber primer:
- WiFi dan BLE berbagi satu radio dan dijalankan time-division (bagian 3.4, dokumen RF
  Coexistence). Jadi mematikan salah satu memang mengurangi kerja radio.
- Operasi tulis flash men-suspend task lain dan membuat core lain berputar di busy loop
  (bagian 3.1). Ini berlaku terlepas dari WiFi.

Yang **tidak bisa** saya buktikan: **tidak ada** pernyataan resmi Espressif yang saya
temukan yang mengklaim angka pengurangan latency tertentu (misalnya "mematikan WiFi
mengurangi jitter sebesar X us"). Jadi klaim "mematikan WiFi selama gerakan mengurangi
latency secara terukur" **tidak terverifikasi di sumber primer** dan harus diukur sendiri
di perangkat. Efek yang paling mungkin dari mematikan radio adalah pengurangan beban CPU dan
pengurangan kemungkinan aktivitas flash, bukan hilangnya preemption langsung pada tingkat
mikrodetik.

---

## Rekomendasi Berurutan

Diurutkan dari perkiraan dampak tertinggi ke terendah terhadap konsistensi posisi.

### 1. Turunkan microstep dari 16 ke 4 atau 8, dan ubah `STEPS_PER_DEG` agar sesuai

**Perkiraan manfaat:** tertinggi. Ini menyerang akar masalah yang paling jelas: pada microstep
16 torsi per microstep di bawah 10 persen torsi holding, sementara detent torque 5 sampai 20
persen, sehingga sebagian microstep tidak menghasilkan gerakan (Faulhaber AN015). Pada
microstep 8 torsi per microstep naik ke 19.51 persen, pada microstep 4 ke 38.27 persen.
Karena aplikasi ini tidak butuh resolusi sudut halus (hanya membuka dan menutup lid),
menurunkan microstep menukar resolusi yang tidak dibutuhkan dengan torsi yang memang
dibutuhkan, dan langsung menaikkan probabilitas setiap pulsa benar-benar menggerakkan rotor.

**Biaya atau risiko:** perlu mengubah DIP switch fisik TB6600 (SW4, SW5, SW6 sesuai tabel
microstep), dan **wajib** mengubah konstanta firmware `STEPS_PER_DEG` serta semua perhitungan
yang bergantung padanya, karena kalau tidak semua sudut jadi salah secara skala. Resolusi
sudut jadi lebih kasar (720 pulse/rev pada microstep 8, 360 pada microstep 4), tetapi untuk
lid ini masih jauh lebih dari cukup. Harus diuji di perangkat.

### 2. Ganti pembangkitan pulsa dari task FreeRTOS ke peripheral RMT

**Perkiraan manfaat:** sangat tinggi untuk determinisme. Source code RMT Arduino core 3.x
membuktikan `rmtWrite()` bisa mengirim jumlah pulsa sembarang dengan durasi per pulsa
independen (bagian 6.1). Ini menghilangkan seluruh kelas masalah jitter berbasis software:
tidak ada lagi task yang bisa dipreempt di tengah pulsa, tidak ada `digitalWrite()` yang
lambat, dan pulsa dihasilkan oleh hardware. Karena `ESP_TIMER_ISR` memang tidak tersedia
(terverifikasi, bagian 3.3), RMT adalah jalur deterministik yang paling praktis.

**Biaya atau risiko:** implementasi paling besar di daftar ini. Perlu menyusun array
`rmt_data_t` untuk ramp (setiap simbol menyatakan satu pulsa), menangani batas memori blok
RMT sehingga jumlah pulsa besar perlu dipecah atau diulang, dan menghitung sendiri kapan
seluruh buffer selesai agar state `moving` tetap benar. Perlu juga memastikan dua channel RMT
tersedia untuk dua lid (verifikasi jumlah TX channel di soc_caps ESP32). Risiko regresi besar
pada state machine yang sudah bekerja.

### 3. Pindahkan semua tulis flash (NVS) keluar dari jalur waktu kritis gerakan

**Perkiraan manfaat:** tinggi dan berbiaya rendah. Dokumentasi resmi menyatakan cache
dimatikan selama operasi SPI1 dan "Most tasks will be disabled" serta core lain berputar
busy loop (bagian 3.1). `simpanSudutGlobal()` yang menulis NVS saat perintah `W` adalah
kandidat langsung yang bisa menghentikan task pulsa. Cukup dengan hanya menulis NVS saat
tidak ada gerakan yang berjalan, dampaknya bisa menghilangkan satu penyebab step loss yang
terdokumentasi.

**Biaya atau risiko:** rendah, hanya perubahan logika penjadwalan (tulis NVS ditunda atau
hanya saat idle). Risiko: jika ditunda terlalu lama dan daya hilang, sudut terakhir mungkin
tidak tersimpan, yang perlu diberi kompromi eksplisit.

### 4. Tambahkan referensi posisi fisik: homing ke stop dengan pola cepat lalu lambat

**Perkiraan manfaat:** tinggi untuk menghapus akumulasi error sepenuhnya. Marlin, proyek
motion control resmi, mendokumentasikan pola ini di source code: cepat ke endstop, mundur,
lalu lambat ke endstop dengan kecepatan dibagi `HOMING_BUMP_DIVISOR` (bagian 5.4). Karena
error saat ini terakumulasi tak terlihat (dihitung dari pulsa yang dikirim), satu referensi
absolut akan menghentikan akumulasi itu.

**Biaya atau risiko:** memerlukan sensor, karena TB6600 **tidak** mendukung sensorless homing
(tidak ada SPI/UART/stall detection, hanya ALERT dan MO, terverifikasi dari datasheet).
Opsi paling murah adalah sensor optik reflektif TCRT5000, yang oleh Vishay sendiri
dicantumkan sebagai "Limit switch for mechanical motions" (bagian 5.1). Biaya: dua sensor,
dua pin GPIO, penempatan mekanis yang tepat pada jarak sekitar 2.5 mm, dan menambahkan
prosedur homing ke state machine. Risiko: keausan mekanis jika menekan stop setiap close,
dan penambahan waktu siklus.

### 5. Tambahkan sensor sudut absolut (misalnya AS5600) untuk umpan balik posisi kontinu

**Perkiraan manfaat:** tertinggi untuk diagnosis dan untuk koreksi berkelanjutan, lebih tinggi
daripada homing titik tunggal karena memberi sudut sebenarnya kapan saja, bukan hanya di satu
referensi. Ini juga satu-satunya cara untuk benar-benar mengukur berapa step yang hilang per
siklus dan memverifikasi apakah rekomendasi 1 sampai 4 berhasil.

**Biaya atau risiko:** kode pembacaan I2C, kalibrasi offset, penambahan magnet dan celah
mekanis. **Catatan kejujuran: saya tidak berhasil memverifikasi spesifikasi AS5600 di sumber
primer dalam sesi ini** (halaman produk resmi ams-OSRAM bergantung JavaScript, dan unduhan
PDF gagal). Datasheet AS5600 harus diverifikasi dari situs resmi ams-OSRAM sebelum
mengandalkan parameter spesifiknya. Menempatkan opsi ini di bawah karena biaya integrasinya,
bukan karena manfaatnya yang lebih rendah.

### 6. Perpanjang atau perhalus profil ramp, dan perkecil kecepatan puncak

**Perkiraan manfaat:** sedang. Datasheet TB6600HG hanya membatasi lebar pulsa minimum 2.2 us
(bagian 1.1), tetapi tidak membatasi akselerasi. Perilaku kehilangan sinkronisasi saat
akselerasi terlalu cepat adalah fenomena mekanis yang sudah diketahui, dan firmware sudah
punya ramp. Namun karena belum ada pengukuran yang mengaitkan step loss dengan profil ramp
tertentu, ini hanya perbaikan heuristik sampai ada data dari perangkat.

**Biaya atau risiko:** menambah waktu gerakan. Tanpa sensor umpan balik, tidak bisa
dipastikan apakah aramp sudah cukup, jadi ini pengaturan buta sampai ada pengukuran.

### 7. `IRAM_ATTR` pada fungsi pulsa inti (dan mengevaluasi apakah benar membantu)

**Perkiraan manfaat:** rendah sampai sedang, dan bergantung. Dokumentasi Speed Optimization
menyatakan IRAM membantu menghindari cache miss tetapi IRAM terbatas dan mengurangi DRAM
(bagian 7.1). Karena fungsi pulsa memanggil fungsi library, `IRAM_ATTR` pada satu fungsi
tidak menjamin seluruh call tree ada di IRAM.

**Biaya atau risiko:** rendah, tetapi manfaatnya mungkin kecil, dan jika dilakukan
sembarangan dapat menghabiskan IRAM tanpa manfaat jelas. Jika rekomendasi 2 (RMT) diambil,
rekomendasi ini menjadi tidak relevan untuk jalur pulsa.

### 8. Kurangi beban radio selama gerakan (optimisasi paling akhir)

**Perkiraan manfaat:** tidak terverifikasi besarannya. WiFi dan BLE memang berbagi satu radio
secara time-division (bagian 3.4), jadi mengurangi beban radio mengurangi beban CPU. Namun
**tidak ada sumber primer Espressif yang mengklaim angka pengurangan latency tertentu**, jadi
manfaatnya harus diukur sendiri dan tidak boleh diasumsikan.

**Biaya atau risiko:** mematikan WiFi atau BLE mengurangi kegunaan produk (mode WiFi AP
adalah jalur kontrol utama), jadi ini kemungkinan besar bukan tradeoff yang layak kecuali
pengukuran menunjukkan manfaat nyata.

---

## Yang Tidak Bisa Dibuktikan Tanpa Perangkat

Bagian ini mencatat apa yang hanya bisa dipastikan dengan perangkat keras dan cara
mengukurnya. Semua butuh pengukuran, bukan pembacaan dokumen.

### 1. Berapa step yang sebenarnya hilang, dan apakah konsisten

Tidak bisa diketahui dari dokumen atau dari firmware saat ini, karena posisi dihitung dari
pulsa yang dikirim. Cara mengukur: tandai titik referensi pada lid atau poros (garis di
cakram dan penunjuk tetap), lalu jalankan siklus OPEN/CLOSE berulang, misalnya 50 kali
berturut-turut, dan catat pergeseran posisi setiap kali. Ini menentukan apakah error
bertambah satu arah (menandakan kehilangan step sistematis) atau acak (menandakan
ketidakstabilan torsi atau noise).

### 2. Apakah microstep 16 benar-benar penyebabnya

Uji langsung: jalankan 50 siklus pada microstep 16, catat pergeserannya; lalu ubah DIP
switch ke microstep 8 (dan sesuaikan `STEPS_PER_DEG`), ulangi 50 siklus, dan bandingkan.
Kalau hipotesis bagian 4 benar, pergeseran harus berkurang signifikan. Tanpa uji ini,
diagnosis microstep tetap hipotesis yang berdasar kuat dari Faulhaber, tetapi belum
terbukti pada perangkat ini.

### 3. Kualitas sinyal PUL dan DIR pada pin driver

Yang perlu diukur dengan osilator atau logic analyzer pada pin PUL dan DIR di sisi TB6600
(bukan di sisi ESP32, karena rangkaian input modul bisa mengubah bentuk sinyal):
- Apakah tepi naik dan tepi turun tajam, atau ada ringing.
- Apakah level LOW benar-benar mendekati 0 V dan level HIGH benar-benar di atas `VIH` (2.0 V
  menurut datasheet TB6600HG) **di sisi pin chip**, setelah rangkaian input modul.
- Berapa sebenarnya jeda antara perubahan DIR dan tepi naik PUL pertama. Firmware berniat
  memberi 5 us, tetapi itu perlu diverifikasi dengan pengukuran karena jeda itu bergantung
  pada implementasi kode dan waktu eksekusi, bukan hanya pada konstanta.

### 4. Apakah jitter 4 us bertahan pada kondisi terburuk

Log firmware melaporkan max 4 us dan 0 dari 23934 pulsa terlambat. Itu angka bagus, tetapi
konteks pengukurannya tidak jelas dari dokumen yang ada. Cara menguji kondisi terburuk:
ulangi pengukuran jitter sambil melakukan hal-hal berikut secara bersamaan, dan catat apakah
jitter masih 4 us:
- trafik HTTP aktif dari klien (bukan hanya AP menyala),
- koneksi BLE aktif,
- perintah `W` yang menulis NVS dijalankan berdekatan dengan gerakan,
- akses WiFi terus menerus, misalnya membuka halaman secara berulang.

Jika jitter melonjak hanya pada kondisi tersebut, itu bukti kuat untuk rekomendasi 3.

### 5. Arus driver yang sebenarnya

Datasheet TB6600HG menyatakan arus diatur oleh Vref dengan divider ratio 3, dan `VNF` 0.9
sampai 1.1 V pada Vref 3.0 V. Tetapi arus yang benar-benar mengalir ke motor bergantung pada
setelan potensiometer atau DIP switch di modul, nilai resistor sense di modul, dan setting
arus motor. Cara mengukur: ukur tegangan Vref di modul dengan multimeter, hitung arus
menurut rumus modul, lalu bandingkan dengan arus yang dibutuhkan motor NEMA 17. **Ini tidak
bisa disimpulkan dari dokumen** karena rumus modul bisa berbeda dari rumus chip, dan skematik
modul milik pengguna tidak saya verifikasi.

### 6. Apakah stop mekanis bisa diandalkan sebagai referensi

Cara mengukur: perintah CLOSE berulang 50 kali dengan pola cepat lalu lambat ke stop, lalu
ukur penyebaran posisi berhenti dengan penunjuk dan cakram berskala. Jika penyebarannya
kecil dan konsisten, homing ke stop layak dijadikan referensi. Jika penyebarannya besar,
masalahnya ada di struktur mekanis (stop terlalu lembut, engsel punya kelonggaran), dan
harus diselesaikan secara mekanis sebelum bisa dipakai sebagai referensi.

### 7. Jumlah channel RMT TX yang benar-benar tersedia

Source code Arduino menyatakan jumlah channel bergantung pada `SOC_RMT_TX_CANDIDATES_PER_GROUP`
dari lapisan SOC. Untuk memastikan dua channel TX tersedia untuk dua lid pada ESP32 ini,
periksa `soc/esp32/include/soc/soc_caps.h` di ESP-IDF versi yang dipakai, atau uji langsung
dengan memanggil `rmtInit()` pada dua pin berbeda dan cek nilai kembaliannya.

### 8. Apakah mengubah prioritas task benar-benar mengubah hasil

Cara mengukur: naikkan prioritas task stepper (misalnya dari 3 ke 5) dan ulangi uji 50
siklus. Berdasarkan dokumen flash (bagian 3.1), operasi flash men-suspend task terlepas dari
prioritas, jadi ekspektasi saya perubahan ini tidak menghilangkan step loss. Tetapi ini
layak diuji karena murah, dan hasilnya akan mengonfirmasi atau membantah analisis itu.

---

## Catatan Penutup tentang Batas Riset Ini

Tiga hal yang perlu disampaikan secara jujur:

1. **Datasheet TB6600HG tidak memuat parameter DIR setup time.** Karena pin-nya bernama
   CW/CCW dan merupakan input level, tidak ada angka setup yang bisa dikutip. Angka 5 us yang
   sudah ada di firmware adalah praktik konservatif, bukan persyaratan datasheet. Angka
   2.2 us (lebar pulsa CLK minimum) **terkonfirmasi** dari datasheet. Untuk timing DIR,
   pembanding terdokumentasi terdekat adalah TI DRV8825 dengan 650 ns, yang menunjukkan
   bahwa "beberapa mikrodetik" sudah sangat aman.

2. **Datasheet TB6600HG tidak membahas akurasi microstep atau perilaku arus rendah.**
   Karena itu, penjelasan terkuat untuk gejala 20 persen datang dari produsen motor
   (Faulhaber AN015) dan proyek motion control (Marlin), bukan dari produsen chip driver.
   Ini bukan kelemahan argumen, tetapi perlu dinyatakan bahwa sumbernya berbeda pihak.

3. **Saya tidak menemukan angka interrupt latency ESP32 yang dijamin di dokumen resmi
   Espressif.** Saya sengaja tidak mencantumkan angka apa pun untuk ini. Yang ada di dokumen
   resmi adalah pernyataan kualitatif: cache dimatikan saat operasi flash dan task lain
   di-suspend, serta esp_timer menganjurkan peripheral khusus untuk "bit banging". Kedua
   pernyataan itu saja sudah cukup untuk mengarahkan solusi ke RMT.

Dokumen ini dibuat berdasarkan pembacaan langsung terhadap datasheet Toshiba TB6600HG,
datasheet TI DRV8825, Faulhaber Application Note 015, source code Marlin, source code
Arduino-ESP32 core 3.3.x, source code dan Kconfig ESP-IDF release v5.5, datasheet Vishay
TCRT5000, serta dokumentasi resmi Espressif untuk RMT, MCPWM, LEDC, esp_timer, coexistensi
RF, concurrency flash, dan optimasi kecepatan.
