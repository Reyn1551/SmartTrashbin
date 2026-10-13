# SmartTrashbin — Cara Pakai (WiFi) ⭐

Robot tong sampah: **ESP32 + 2 motor DC (roda) + 2 stepper NEMA 17 (lid)**

**Cara pakai paling mudah** — ESP32 jadi hotspot sendiri, tidak perlu WiFi rumah, tidak perlu laptop, tidak perlu file apa pun di HP.

---

## Cara pakai (3 langkah)

### 1. Nyalakan robot
Colok daya ESP32 dan driver TB6600 + BTN7970.

### 2. Sambungkan HP ke WiFi robot

| | |
|---|---|
| **Nama WiFi (SSID)** | `SmartTrashbin` |
| **Password** | `12345678` |

> Android akan bilang **"WiFi tanpa internet — tetap sambungkan?"**
> Pilih **TETAP SAMBUNGKAN**. Ini normal: robot tidak punya internet, dan memang tidak perlu.

### 3. Buka browser, ketik:

```
http://192.168.4.1
```

Panel kendali langsung muncul. Putar HP ke **posisi mendatar**, lalu tap **BUKA PANEL**.

**Selesai.** Tidak ada langkah lain.

> **Kalau ada masalah atau gerakan tidak halus**, buka halaman debug:
> ```
> http://192.168.4.1/debug
> ```
> Halaman itu menampilkan log, statistik jitter, dan **diagnosa otomatis**
> yang memberi tahu penyebabnya. Bisa dibuka dari HP tanpa laptop.
> Panduan lengkap: **`DEBUG-DARI-HP.md`**

---

## Kenapa cara ini paling mudah

| | WiFi (ini) | BLE (sebelumnya) |
|---|---|---|
| Perlu WiFi rumah | ❌ tidak | ❌ tidak |
| Perlu laptop/server | ❌ tidak | ✅ **ya** (Web Bluetooth tolak `file://`) |
| Perlu file di HP | ❌ tidak | ✅ ya |
| Tap "connect" tiap buka | ❌ tidak | ✅ ya |
| Alamat | selalu `192.168.4.1` | IP bisa berubah |
| Jalan di HP non-Android | ✅ ya | ❌ iOS tidak didukung |

HTML **tertanam di dalam ESP32** — jadi tidak ada yang perlu disiapkan di HP.

---

## Kontrol

| Kontrol | Fungsi |
|---|---|
| ▲ | Roda maju |
| ▼ | Roda mundur |
| ◀ | Pivot kiri (kiri mundur, kanan maju) |
| ▶ | Pivot kanan (kiri maju, kanan mundur) |
| ■ | **STOP** semua roda |
| Slider | Kecepatan PWM roda (0–255) |
| TONG KIRI | Buka / tutup lid **kiri saja** |
| TONG KANAN | Buka / tutup lid **kanan saja** |
| Kalibrasi · 0° | Set posisi stepper sekarang = 0° |
| Tutup Kedua Lid | Tutup **kedua** lid sekaligus |

> Setiap tombol lid menggerakkan **hanya sisi yang kamu klik**. Lid sebelahnya
> tidak ikut bergerak.

### Perintah lid (untuk terminal / testing)

| Perintah | Arti |
|---|---|
| `C` | buka **kedua** lid |
| `S` | tutup **kedua** lid |
| `IL1` | buka lid **kiri** saja |
| `IL0` | tutup lid **kiri** saja |
| `IR1` | buka lid **kanan** saja |
| `IR0` | tutup lid **kanan** saja |
| `IL1R1` | kombinasi — keduanya dalam satu perintah |

Contoh:
```powershell
Invoke-WebRequest "http://192.168.4.1/cmd?val=IL1"   # buka lid kiri saja
Invoke-WebRequest "http://192.168.4.1/cmd?val=IR0"   # tutup lid kanan saja
```

### Tuning kecepatan stepper (tanpa upload ulang)

Perintah `K` mengubah kecepatan stepper saat itu juga:

```powershell
Invoke-WebRequest "http://192.168.4.1/cmd?val=K300"    # lambat, paling aman
Invoke-WebRequest "http://192.168.4.1/cmd?val=K800"    # sedang
Invoke-WebRequest "http://192.168.4.1/cmd?val=K1500"   # cepat
```

Default sekarang **600** step/detik. Motor sudah pakai **ramp akselerasi** otomatis,
jadi tidak lagi stall saat mulai bergerak.

---

## Fitur keselamatan

**1. Tekan-tahan.** Tombol gerak harus ditahan. Lepas jari → otomatis kirim STOP.

**2. Watchdog 2.5 detik (sudah diuji).** Kalau HP mati, keluar jangkauan, atau browser
ditutup saat roda jalan — robot **berhenti sendiri** setelah 2.5 detik tanpa perintah.

Terbukti dari uji:
```
Kirim 'F', lalu diam:
  t=1s : move:"run"     roda jalan
  t=2s : move:"run"     roda jalan
  t=3s : move:"stop"    <-- berhenti sendiri
```

**3. Tab disembunyikan → stop.** Pindah app atau matikan layar saat roda jalan, robot berhenti.

**4. Motor stepper free saat idle.** Tidak panas menahan posisi.

---

## Kalau ada masalah

**Halaman tidak terbuka / "tidak dapat terhubung"**
→ Pastikan HP tersambung ke WiFi `SmartTrashbin`, **bukan** WiFi lain.
→ Matikan **paket data** di HP — Android kadang memaksa lewat data seluler.
→ Pastikan alamatnya `http://192.168.4.1` (pakai **http**, bukan https).

**"WiFi tanpa internet" terus muncul**
→ Pilih "Tetap sambungkan". Kalau Android menolak, buka Setelan → WiFi →
`SmartTrashbin` → centang "Auto-connect" dan matikan "Private DNS".

**Robot tidak bergerak padahal halaman terbuka**
→ Cek catu daya BTN7970 (butuh daya motor terpisah dari USB ESP32).
→ Cek **common ground**: GND ESP32, GND BTN7970, GND TB6600 harus tersambung.
→ Naikkan slider PWM (default 140, coba 200).
→ Cek polaritas: kalau roda kebalik arahnya, tukar `D1`/`D2` motor itu di firmware.

**Stepper bergetar / berdengung tapi lid tidak bergerak**
→ **Ini masalah paling sering.** Lihat panduan lengkap: **`MENGATASI-STEPPER-BERGETAR.md`**
→ Cepat: coba `K300` dulu (kecepatan lambat):
   `Invoke-WebRequest "http://192.168.4.1/cmd?val=K300"`
→ Penyebab utama biasanya **level tegangan**: TB6600 butuh sinyal **5V**, ESP32 keluar **3.3V**.
→ Naikkan arus TB6600 ke **1.5–2.0 A**.
→ Pastikan microstep TB6600 = 16.

**Stepper kehilangan langkah (lid tidak sampai penuh)**
→ Turunkan kecepatan: `K400` atau `K300` (bisa langsung dari HP/terminal, tanpa upload ulang).

**ESP32 tidak terdeteksi setelah semua kabel dipasang**
→ Kemungkinan GPIO15 (strapping pin, dipakai PUL lid kanan).
→ Lepas kabel PUL lid kanan, boot/upload, lalu sambungkan kembali.

**Ingin ganti nama/password WiFi**
→ Ubah `AP_SSID` dan `AP_PASS` di firmware. Password minimal 8 karakter.

---

## Cara BLE (cadangan)

Firmware ini **juga** memancarkan Bluetooth. Kalau WiFi bermasalah:

1. Jalankan `JALANKAN-SERVER.cmd` di laptop
2. Buka `http://<IP-laptop>:8000/controller-ble.html` di Chrome Android
3. Tap **HUBUNGKAN ROBOT** → pilih `SmartTrashbin`

BLE tidak butuh WiFi sama sekali. Berguna kalau router/HP bermasalah dengan hotspot.

---

## Wiring

### Motor DC — driver BTN7970
| Roda | PWM | D1 | D2 |
|---|---|---|---|
| KIRI | GPIO 25 | GPIO 27 | GPIO 26 |
| KANAN | GPIO 14 | GPIO 13 | GPIO 12 |

### Stepper Lid — driver TB6600
| Lid | PUL | DIR | ENA | Konektor |
|---|---|---|---|---|
| KIRI | GPIO 18 | GPIO 19 | GPIO 23 | STPR_1 |
| KANAN | GPIO 15 | GPIO 4 | GPIO 5 | STPR_2 |

**TB6600:** microstep **16** → 3200 pulse/rev → 1° = 8.889 step. Arus **1.5–1.7 A**.
TB6600 butuh **12–24 V** terpisah. **Jangan** ambil daya motor dari USB ESP32.

---

## Tes tanpa HP (lewat kabel USB)

Serial monitor ke **COM6 · 115200**. Tulis perintah, akhiri dengan **Enter**:

```
F  maju          B  mundur        L  pivot kiri     R  pivot kanan
P  STOP          A  kalibrasi      ?  status
V<0-255>  kecepatan roda (mis. V200)
K<sps>    kecepatan stepper (mis. K300)
C  buka KEDUA lid                 S  tutup KEDUA lid
IL1 / IL0   buka / tutup lid KIRI saja
IR1 / IR0   buka / tutup lid KANAN saja
```

> Perintah harus diakhiri **Enter**. Parser serial membaca satu baris penuh,
> jadi `IL1` akan diuraikan utuh — bukan terpecah jadi `I`, `L`, `1`.

Atau lewat HTTP dari terminal:

```powershell
Invoke-WebRequest "http://192.168.4.1/cmd?val=C"     # buka lid
Invoke-WebRequest "http://192.168.4.1/status"        # lihat status
```

---

## Upload ulang firmware

```powershell
$cli = "$env:USERPROFILE\Downloads\esp32-setup\arduino-cli.exe"
& $cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app `
    --upload -p COM6 "$env:USERPROFILE\Downloads\esp32-setup\SmartTrashbin-WiFi"
```

> **`PartitionScheme=huge_app` wajib.** WiFi + BLE + HTML sekaligus memakai
> 1.707 KB. Dengan `min_spiffs` itu 86% (mepet); dengan `huge_app` turun ke **54%**.

### Kalau mengubah tampilan (HTML)

HTML tertanam di firmware lewat `web_ui.h`. Setelah mengedit `controller-wifi.html`:

```powershell
cd "$env:USERPROFILE\Downloads\esp32-setup\SmartTrashbin-WiFi"
python konversi-html.py     # controller-wifi.html -> web_ui.h
```

Lalu upload ulang. **Jangan edit `web_ui.h` langsung** — file itu dibuat otomatis.

---

## Spesifikasi teknis

| Item | Nilai |
|---|---|
| Chip | ESP32-D0WD-V3 rev 3.1, 240 MHz, dual core |
| Flash | 4 MB |
| Core | esp32:esp32 **3.3.12** |
| Partisi | `huge_app` (3 MB app) |
| WiFi | SoftAP, kanal 6, maks 4 klien |
| IP robot | `192.168.4.1` |
| Endpoint | `/` (UI), `/cmd?val=X`, `/status` (JSON) |
| BLE | tetap aktif, nama `SmartTrashbin` |
| PWM roda | 20 kHz, 8 bit (LEDC) |
| Stepper | 3200 step/rev, non-blocking |
| Ukuran firmware | 1.706.986 byte (**54%**) |
| RAM terpakai | 63.068 byte (19%) |

---

## Hasil pengujian (2026-10-12)

Semua diuji nyata dari laptop yang tersambung ke AP ESP32:

| Uji | Hasil |
|---|---|
| ARP `192.168.4.1` | `68-09-47-86-91-11` = MAC ESP32 (bukan host lain) |
| DHCP | laptop dapat `192.168.4.2` |
| Halaman `/` | HTTP 200, 19.491 byte |
| `/status` | JSON valid |
| Buka lid `C` | 0° → **90°** (stepper benar-benar bergerak) |
| Tutup lid `S` | 90° → **0°** |
| Kecepatan `V180` | `sp` 140 → 180 |
| Tombol gerak | `F`→mv1, `L`→mv3, `R`→mv4, `B`→mv2 |
| STOP `P` | `move:"stop"` |
| **Watchdog** | run → run → **stop** (berhenti sendiri di detik ke-3) |
| Serial idle | 0 karakter, 0 reboot (stabil) |

---

## Bug yang ditemukan dan diperbaiki saat pengujian

**1. Spam watchdog.** Versi awal mencetak `[SAFETY] ...` berulang kali, bukan sekali.
Penyebab: blok pesan berada di jalur yang tereksekusi tiap siklus setelah motor distop.
Sesudah perbaikan: tercetak **tepat 1x**.

**2. `return` di dalam `loop()`.** Perintah serial memakai `return` untuk keluar lebih awal,
yang bisa melompati sisa `loop()`. Diganti `while` + `break`.

**3. Sampah buffer serial saat boot.** Kalau host menumpuk perintah lama, perintah itu
langsung dieksekusi saat ESP32 boot sehingga robot tampak bergerak sendiri.
`setup()` sekarang membuang isi buffer lebih dulu.

---

## Catatan penting saat menguji lewat script

Kalau menguji pakai script yang membuka/menutup port serial berulang kali, **buffer
input bisa menumpuk perintah lama**. Gejalanya: log tampak spam dan muncul banyak
`ets ...` (reboot). Ini **bukan** ESP32 yang rusak — itu buffer di sisi host.

Cara memastikan: buka port, tunggu 2 detik, `DiscardInBuffer()`, lalu pantau tanpa
mengirim apa pun. ESP32 yang sehat akan **sunyi total** (0 karakter).

**5. Tombol lid per sisi.** Versi awal hanya punya `C`/`S` yang selalu menggerakkan
**kedua** lid, dan `toggleLid()` di HTML juga mewarnai kedua tombol — jadi klik satu
lid membuat dua-duanya bereaksi. Sekarang ada perintah `I` berargumen sisi
(`IL1`/`IL0`/`IR1`/`IR0`), dan UI hanya mengubah tampilan sisi yang benar-benar diklik.

**6. Parser serial per-karakter (paling berbahaya).** Perintah serial dulu dibaca satu
karakter per siklus loop, sehingga `IL1` terpecah jadi `I` → `L` → `1`. Huruf `L`
lalu **dieksekusi sebagai pivot kiri — roda ikut bergerak!** Sekarang parser membaca
satu baris penuh sampai Enter, jadi perintah diurai utuh. Ini terbukti dari log:
sebelumnya muncul `[WARN] Perintah tidak dikenal: '1'` dan roda jalan; sesudahnya
`[LID] Lid KIRI -> BUKA`.

---

## Struktur file

```
SmartTrashbin-WiFi\
├── SmartTrashbin-WiFi.ino   Firmware utama (WiFi AP + BLE + debug)
├── web_ui.h                 HTML panel (DIBUAT OTOMATIS)
├── controller-wifi.html     Sumber panel — edit file ini
├── konversi-html.py         panel HTML -> web_ui.h
├── controller-ble.html      UI versi BLE (cadangan)
├── CARA-PAKAI-WIFI.md       Dokumen ini
├── DEBUG-DARI-HP.md         Debug lewat HP (log + jitter + diagnosa)
└── MENGATASI-STEPPER-BERGETAR.md   Panduan masalah getar/stall
```

Versi BLE saja (tanpa WiFi) tetap ada di folder `..\SmartTrashbin\`.
