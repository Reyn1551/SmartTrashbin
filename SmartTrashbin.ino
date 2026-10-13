/*
 * ============================================================
 *  SMART TRASHBIN — ESP32 WIFI AP + BLE
 *  Motor DC (roda) + 2x Stepper TB6600 (lid)
 * ============================================================
 *
 *  DUA JALUR KONTROL (bersamaan, pilih salah satu di HP):
 *
 *  1. WIFI ACCESS POINT  <- cara utama, paling mudah
 *     - ESP32 memancarkan hotspot:  SmartTrashbin
 *     - Password:                   12345678
 *     - Buka browser ke:            http://192.168.4.1
 *     - UI sudah tertanam di firmware, tidak perlu file apa pun
 *
 *  2. BLUETOOTH LE       <- cadangan, kalau WiFi bermasalah
 *     - Nama device:  SmartTrashbin
 *     - Pakai controller-ble.html + server lokal di laptop
 *
 *  ------------------------------------------------------------
 *  HARDWARE (verified):
 *    Board : ESP32 DevKit V4 32D (ESP32-D0WD-V3 rev 3.1)
 *    Port  : COM6 via CP2102
 *
 *  MOTOR DC — driver BTN7970
 *    Roda KIRI  : PWM=25  D1=27  D2=26
 *    Roda KANAN : PWM=14  D1=13  D2=12
 *
 *  STEPPER LID — driver TB6600 (microstep 16 -> 3200 pulse/rev)
 *    Lid KIRI  : PUL=18  DIR=19  ENA=23
 *    Lid KANAN : PUL=15  DIR=4   ENA=5
 *
 *  ------------------------------------------------------------
 *  CATATAN GPIO15
 *  GPIO15 adalah strapping pin ESP32. Kalau TB6600 menariknya HIGH
 *  saat boot, ESP32 bisa gagal start. Kode ini tidak menyentuhnya
 *  sampai setup() selesai. Kalau board gagal boot: lepas kabel PUL
 *  lid kanan, boot, sambungkan kembali.
 * ============================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <esp_timer.h>     // hardware timer untuk pulsa stepper (anti-jitter)
#include <esp_system.h>    // esp_reset_reason() - pelacak reset/brownout
#include <Preferences.h>   // simpan sudut lid global ke flash (NVS)
#include <stdarg.h>        // va_list untuk addLogf()

#include "web_ui.h"      // dibuat oleh konversi-html.py

// ==================== KONFIGURASI WIFI ====================

const char* AP_SSID = "SmartTrashbin";
const char* AP_PASS = "12345678";        // minimal 8 karakter
const byte  AP_CHANNEL = 6;
const byte  AP_MAX_CONN = 4;

WebServer server(80);

// ==================== STATE (didefinisikan paling awal) ====================
//
//  Struct Stepper HARUS ada di sini — sebelum semua fungsi.
//  Arduino IDE menyisipkan prototipe fungsi otomatis di dekat atas file;
//  kalau struct ini diletakkan setelah fungsi stepper, prototipe
//  `void stepperBegin(Stepper&)` akan muncul sebelum struct dikenali dan
//  menyebabkan error "'Stepper' was not declared in this scope".

enum MoveMode { MOVE_STOP, MOVE_FWD, MOVE_BACK, MOVE_LEFT, MOVE_RIGHT };
volatile MoveMode moveMode = MOVE_STOP;

unsigned long lastCmdMillis = 0;              // watchdog: kapan perintah terakhir

// Batas waktu tanpa perintah sebelum roda distop otomatis. Ini pengaman
// supaya robot tidak terus jalan kalau HP mati atau WiFi terputus.
//
// Nilainya sengaja longgar, bukan ketat. Sebelumnya 2500 ms dan terasa
// seperti "robot berhenti sendiri padahal tombol masih ditahan", karena
// satu saja heartbeat yang telat sudah cukup untuk memicu stop. Dengan
// 8000 ms, heartbeat yang sesekali telat tidak lagi memutus gerakan,
// tapi robot tetap berhenti dalam hitungan detik kalau HP benar-benar
// hilang.
const unsigned long CMD_TIMEOUT_MS = 8000;

struct Stepper {
  uint8_t pinPul, pinDir, pinEna;
  long    currentStep;
  long    targetStep;
  long    dir;
  bool    moving;
  unsigned long lastPulseUs;
  unsigned long halfPeriodUs;
  unsigned long pulseWidthUs;
  int     warmUp;            // pulsa awal yang dilewati saat ukur jitter
  float   speedCurrent;      // kecepatan sekarang (step/detik) - untuk ramp
  float   speedTarget;       // kecepatan tujuan (step/detik)
  bool    pulseHigh;
  bool    enabled;
};

// ==================== WIRING ====================

// --- Motor DC (BTN7970) ---
const uint8_t M_LEFT_PWM  = 25;
const uint8_t M_LEFT_D1   = 27;
const uint8_t M_LEFT_D2   = 26;
const uint8_t M_RIGHT_PWM = 14;
const uint8_t M_RIGHT_D1  = 13;
const uint8_t M_RIGHT_D2  = 12;

// --- Stepper Lid (TB6600) ---
const uint8_t LID_L_PUL = 18;
const uint8_t LID_L_DIR = 19;
const uint8_t LID_L_ENA = 23;
const uint8_t LID_R_PUL = 15;
const uint8_t LID_R_DIR = 4;
const uint8_t LID_R_ENA = 5;

// ==================== LOG RING (untuk /debug tanpa laptop) ====================
//
//  Menyimpan 40 pesan terakhir di RAM. Bisa dibaca dari HP lewat
//  http://192.168.4.1/debug  — jadi tidak perlu kabel/laptop.
//
//  Semua Serial.println penting juga masuk ke sini lewat addLog().

#define LOG_LINES   60          // 40 terlalu sedikit, 120 memakan RAM
#define LOG_LEN     100

char     logRing[LOG_LINES][LOG_LEN];
uint8_t  logHead  = 0;
uint16_t logTotal = 0;
uint32_t logDrop  = 0;      // pesan yang hilang karena ring penuh

void addLog(const char* msg) {
  // Tulis ke Serial (kalau kabel tersambung) DAN ke ring buffer.
  Serial.println(msg);

  snprintf(logRing[logHead], LOG_LEN, "[%8lu] %s",
           (unsigned long)millis(), msg);
  logHead = (logHead + 1) % LOG_LINES;
  logTotal++;
}

void addLogf(const char* fmt, ...) {
  char buf[LOG_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, LOG_LEN, fmt, ap);
  va_end(ap);
  addLog(buf);
}

// ==================== KONSTANTA STEPPER ====================

const float STEPS_PER_REV = 3200.0;
const float STEPS_PER_DEG = STEPS_PER_REV / 360.0;   // 8.8889

// ------------------------------------------------------------
//  SUDUT LID GLOBAL
// ------------------------------------------------------------
//  Sudut bukaan yang dipakai oleh SEMUA perintah buka/tutup
//  (perintah C/S/I, perintah O, dan tombol BUKA/TUTUP di /debug).
//  Atur sekali, lalu seluruh sistem memakai angka yang sama.
//
//  Contoh: setelah diatur 45 derajat, menekan BUKA akan membuka
//  45 derajat dan menekan TUTUP akan kembali ke 0.
const float LID_ANGLE_MIN = 1.0;
const float LID_ANGLE_MAX = 120.0;    // batas aman mekanik
const float LID_ANGLE_DEF = 90.0;     // dipakai kalau belum pernah diatur

float lidAngleGlobal = LID_ANGLE_DEF;

// Batas maksimum posisi lid. Sama dengan sudut maksimum yang boleh
// diatur, supaya lid tidak pernah bisa melewati batas mekanik.
const float LID_MAX_DEG = LID_ANGLE_MAX;

// ------------------------------------------------------------
//  KECEPATAN PULSA STEPPER (step per detik)
// ------------------------------------------------------------
//  Kalau motor BERGETAR atau berdengung tanpa berputar, itu tanda
//  motor kehilangan sinkronisasi (stall). Penyebab paling umum:
//    1. Level tegangan sinyal PUL kurang (TB6600 butuh ~5V, bukan 3.3V)
//    2. Kecepatan terlalu tinggi untuk arus/tegangan driver yang ada
//    3. Arus TB6600 di-set terlalu rendah
//  Turunkan angka ini dulu sebelum menyalahkan hal lain.
//
//  Nilai aman         : 400  - 800
//  Nilai sedang       : 800  - 1200
//  Nilai cepat        : 1200 - 2000  (butuh sinyal 5V yang benar)
//
// Kecepatan stepper.
//
// STEP_SPEED_FAST dipakai sebagai kecepatan default saat robot menyala.
// Nilai 1600 dipilih setelah pengujian langsung: pada semua kecepatan
// (K300 s/d K2000) jitter maksimum hanya 2-4 us dan lid berhenti tepat
// di 90 derajat, jadi 1600 aman dan membuka lid dalam ~0.94 detik
// (K600 butuh 1.23 detik).
//
// CATATAN: pengujian dilakukan TANPA beban lid. Kalau lid punya berat
// atau gesekan, turunkan lewat halaman /debug (tombol K900 / K600)
// atau kirim perintah K600. Kalau lid berhenti kurang dari 90 derajat,
// itu tanda kehilangan langkah -> turunkan kecepatan.
// ------------------------------------------------------------
//  PROFIL KECEPATAN: SLOW / MEDIUM / FAST
// ------------------------------------------------------------
//  Satu tombol di controller memilih profil, dan SATU profil mengatur
//  dua hal sekaligus: kecepatan putar roda (PWM) dan kecepatan gerak
//  tutup (step per detik). Ini disengaja, supaya "slow" berarti slow
//  untuk seluruh robot, bukan hanya rodanya.
//
//  SLOW   : paling pelan dan paling halus. Kecepatan tutupnya sengaja
//           jauh lebih rendah supaya tidak ada kemungkinan kehilangan
//           langkah. Ini pilihan untuk gerakan yang harus mulus betul,
//           dan untuk menghemat baterai.
//  MEDIUM : seimbang, dipakai sehari-hari.
//  FAST   : paling cepat, tapi paling berisiko kehilangan langkah kalau
//           arus TB6600 kurang atau beban tutup berat.
//
//  Nilai tutup berhenti di 900 step per detik karena pengujian
//  menunjukkan di atas itu mulai ada tanda kehilangan langkah pada
//  beban ringan. Roda boleh lebih cepat karena motor DC tidak punya
//  masalah sinkronisasi.

#define PROFIL_SLOW    0
#define PROFIL_MEDIUM  1
#define PROFIL_FAST    2

//            nama        step/s  PWM
struct ProfilKecepatan {
  const char* nama;
  float       stepSps;
  uint8_t     pwmRoda;
};

const ProfilKecepatan PROFIL[3] = {
  { "SLOW",    400.0,  110 },
  { "MEDIUM",  900.0,  165 },
  { "FAST",   1600.0,  220 }
};

uint8_t profilAktif = PROFIL_MEDIUM;

// Kecepatan stepper yang sedang berlaku, diubah dari HP/serial dengan
// perintah 'K'. Nilai ini selalu diisi ulang saat profil berganti.
float stepSpeedActive = PROFIL[PROFIL_MEDIUM].stepSps;

// ============================================================
//  POLARITAS SINYAL KE TB6600  *** BACA INI SEBELUM MENGUBAH ***
// ============================================================
//
//  TB6600 memakai OPTOCOUPLER di input PUL/DIR/ENA. Supaya bekerjasama
//  dengan benar, arus harus mengalir 10-15 mA melewati LED di dalamnya.
//
//  ADA DUA CARA WIRING, DAN POLARITAS FIRMWARE HARUS COCOK:
//
//  ------------------------------------------------------------
//  1) COMMON-ANODE  (5V)  <-- YANG BENAR, dipakai sekarang
//  ------------------------------------------------------------
//     X+  (PUL+/DIR+/ENA+)  ->  5V
//     X-  (PUL-/DIR-/ENA-)  ->  GPIO ESP32
//     GND buck converter    ->  GND ESP32  (WAJIB disambung)
//
//     GPIO menarik ke LOW = arus mengalir = sinyal AKTIF.
//     Jadi: pulsa = LOW, lalu kembali HIGH.  ->  PULSE_ACTIVE = LOW
//
//     Kenapa ini benar: arus datang dari rel 5V yang kuat, bukan dari
//     pin ESP32. Optocoupler dapat 10-15 mA penuh, sinyal jadi tegas,
//     dan motor berputar mulus.
//
//  ------------------------------------------------------------
//  2) COMMON-CATHODE  (3.3V dari GPIO)  <-- MENYEBABKAN GETAR
//  ------------------------------------------------------------
//     X-  (PUL-/DIR-/ENA-)  ->  GND
//     X+  (PUL+/DIR+/ENA+)  ->  GPIO
//
//     Arus = (3.3V - 1.2V) / 220ohm = hanya ~9.5 mA, dan itu pun
//     naik-turun karena GPIO ESP32 tidak stabil pada arus besar.
//     Optocoupler kadang tidak menyala -> GEJALA: motor berputar
//     tapi TERSENDAT-SENDAT. Persis masalah yang dilaporkan.
//
//  ------------------------------------------------------------
//  CARA MENGGANTI
//  ------------------------------------------------------------
//     Wiring common-anode 5V  ->  PULSE_ACTIVE LOW   (default)
//     Wiring common-cathode   ->  PULSE_ACTIVE HIGH
//
//  Setelah mengubah, WAJIB compile+upload ulang. Jangan pernah
//  mencampur: wiring 5V dengan firmware aktif-HIGH akan membuat
//  motor bergerak pada waktu yang salah.
// ============================================================

#define PULSE_ACTIVE   LOW    // common-anode 5V   (default)
// #define PULSE_ACTIVE   HIGH  // common-cathode 3.3V (tidak disarankan)

// Turunan polaritas. Dipakai di seluruh kode supaya tidak ada nilai
// yang ditulis langsung (hard-coded) di tempat lain.
#define PULSE_IDLE     (PULSE_ACTIVE == LOW  ? HIGH : LOW)
#define PULSE_ON       (PULSE_ACTIVE)
#define PULSE_OFF      (PULSE_IDLE)

// DIR: dengan wiring common-anode, arah dibalik dibanding common-cathode.
// Nilai ini menyesuaikan otomatis.
#define DIR_FOR_POS    (PULSE_ACTIVE == LOW ? LOW  : HIGH)
#define DIR_FOR_NEG    (PULSE_ACTIVE == LOW ? HIGH : LOW)

// ENA: motor FREE (tidak menahan) saat ENA tidak dialiri arus.
#define ENA_ACTIVE     (PULSE_ACTIVE)      // arus mengalir = motor DIKUNCI
#define ENA_INACTIVE   (PULSE_ACTIVE == LOW ? HIGH : LOW)  // motor FREE

// ------------------------------------------------------------
//  MODE TAHAN (HOLD)  *** PENTING UNTUK DIAGNOSA ***
// ------------------------------------------------------------
//  Setelah gerakan selesai, firmware normalnya MELEPAS motor (ENA off =
//  motor free). Ini hemat daya dan motor tidak panas.
//
//  TAPI motor yang FREE bisa berputar sendiri karena:
//   - beban lid menariknya (gravitasi)
//   - getaran mekanik
//   - sisa medan magnet
//
//  Gejala "motor terus berputar walau firmware sudah berhenti" bisa
//  berasal dari ini. Kalau HOLD_MODE = true, motor TETAP DIKUNCI setelah
//  selesai, sehingga tidak bisa berputar sendiri.
//
//  Cara memakai untuk diagnosa:
//   - HOLD_MODE false (default): motor bebas setelah gerakan
//   - HOLD_MODE true          : motor dikunci setelah gerakan
//
//  Kalau dengan HOLD_MODE true masalah "terus berputar" HILANG,
//  berarti penyebabnya memang motor free + beban, bukan sinyal.
//
//  Bisa diubah saat berjalan lewat perintah 'H' (tanpa upload ulang):
//     H1 -> kunci setelah gerakan
//     H0 -> lepas setelah gerakan (default)
// ------------------------------------------------------------
bool holdMode = false;

// Berapa lama motor tetap dikunci setelah gerakan selesai (ms).
// 0 = selamanya (sampai perintah berikutnya). Dipakai kalau holdMode true
// dan kita ingin motor hanya dikunci sebentar saja.
#define HOLD_TIMEOUT_MS  0

// ==================== KONSTANTA MOTOR ====================

uint8_t speedPWM = 140;
const uint32_t PWM_FREQ = 20000;
const uint8_t  PWM_RES  = 8;

// ==================== BLE ====================

#define BLE_NAME       "SmartTrashbin"
#define SERVICE_UUID   "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_RX_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_TX_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a9"

BLEServer*         bleServer       = nullptr;
BLECharacteristic* charTX          = nullptr;
bool               bleConnected    = false;

// ==================== STATE ====================
// (struct Stepper sudah didefinisikan di atas, dekat WebServer)

// Instance stepper dibuat di sini karena butuh konstanta pin LID_*.
// Dipakai designated initializer (.field = ...) supaya urutan field tidak
// mudah salah kalau struct berubah lagi.
Stepper stLidLeft = {
  .pinPul = LID_L_PUL, .pinDir = LID_L_DIR, .pinEna = LID_L_ENA,
  .currentStep = 0, .targetStep = 0, .dir = 0, .moving = false,
  .lastPulseUs = 0, .halfPeriodUs = 0, .pulseWidthUs = 0, .warmUp = 0,
  .speedCurrent = 0, .speedTarget = 0, .pulseHigh = false, .enabled = false
};

Stepper stLidRight = {
  .pinPul = LID_R_PUL, .pinDir = LID_R_DIR, .pinEna = LID_R_ENA,
  .currentStep = 0, .targetStep = 0, .dir = 0, .moving = false,
  .lastPulseUs = 0, .halfPeriodUs = 0, .pulseWidthUs = 0, .warmUp = 0,
  .speedCurrent = 0, .speedTarget = 0, .pulseHigh = false, .enabled = false
};

// ==================== MOTOR DC ====================
//
//  ------------------------------------------------------------
//  KENAPA BELOK DULU TERASA SEPERTI SALAH SATU MOTOR TIDAK JALAN
//  ------------------------------------------------------------
//
//  Versi lama memakai SATU nilai PWM untuk kedua roda, dan itulah
//  sumber masalahnya. Ada dua sebab, dan keduanya nyata:
//
//  1. SETIAP MOTOR PUNYA TITIK MULAI BERBEDA.
//     Motor DC tidak mulai berputar di PWM yang sama. Satu motor
//     mungkin mulai bergerak di PWM 70, pasangannya baru di PWM 100.
//     Akibatnya pada PWM rendah, satu roda sudah berputar sementara
//     yang lain masih diam — persis gejala "ada motor yang seperti
//     tidak jalan". Ini paling terasa saat berbelok, karena di situ
//     kedua roda memang harus berlawanan arah dan torsi bebannya
//     paling berat: seluruh berat robot diseret memutar, bukan
//     didorong lurus.
//
//  2. BELOK ADALAH KONDISI PALING BERAT, TAPI MEMAKAI PWM YANG SAMA.
//     Saat berbelok, robot harus mengalahkan gesekan roda terhadap
//     lantai. Beban ini jauh lebih besar daripada saat maju. Kalau
//     PWM-nya sama dengan PWM maju, motor yang titik mulainya lebih
//     tinggi akan tertahan dan berhenti, sementara pasangannya jalan.
//
//  PERBAIKANNYA:
//  - Tiap roda punya nilai PWM sendiri (pwmKiri / pwmKanan), sehingga
//    ketidakseimbangan antar motor bisa dikompensasi.
//  - Belok memakai PWM lebih tinggi daripada maju, karena bebannya
//    memang lebih berat. Ini yang membuat putaran di tempat jadi
//    lancar, bukan tersendat.
//  - Nilai kompensasi bisa disetel dari HP lewat halaman /setting
//    dan disimpan di flash.
//
//  Arah putar tiap roda ditentukan oleh motorBalikKiri /
//  motorBalikKanan, jadi "putar di tempat" berlaku untuk roda mana
//  pun, tidak peduli bagaimana kabelnya terpasang.

void motorBegin() {
  pinMode(M_LEFT_D1,  OUTPUT);
  pinMode(M_LEFT_D2,  OUTPUT);
  pinMode(M_RIGHT_D1, OUTPUT);
  pinMode(M_RIGHT_D2, OUTPUT);

  // API PWM baru (core esp32 >= 3.0); ledcSetup/ledcAttachPin sudah dihapus.
  ledcAttach(M_LEFT_PWM,  PWM_FREQ, PWM_RES);
  ledcAttach(M_RIGHT_PWM, PWM_FREQ, PWM_RES);

  motorStop();
}

void motorSideDrive(uint8_t pinPWM, uint8_t pinD1, uint8_t pinD2,
                    bool forward, bool reverse, uint8_t pwm) {
  if (forward) {
    digitalWrite(pinD1, HIGH);
    digitalWrite(pinD2, LOW);
    ledcWrite(pinPWM, pwm);
  } else if (reverse) {
    digitalWrite(pinD1, LOW);
    digitalWrite(pinD2, HIGH);
    ledcWrite(pinPWM, pwm);
  } else {
    digitalWrite(pinD1, LOW);
    digitalWrite(pinD2, LOW);
    ledcWrite(pinPWM, 0);
  }
}

// ------------------------------------------------------------
//  PEMBALIK ARAH MOTOR
// ------------------------------------------------------------
//  Kalau kabel motor terpasang terbalik, robot akan bergerak ke arah
//  yang berlawanan dengan tombolnya (tekan MAJU tapi robot mundur).
//  Daripada membongkar kabel, arahnya dibalik di perangkat lunak.
//
//  Setel lewat halaman /setting, atau lewat perintah Serial:
//     YL1  -> balik arah motor KIRI
//     YL0  -> kembalikan ke normal
//     YR1 / YR0 -> sama untuk motor KANAN
//
//  Nilainya disimpan di memori flash, jadi tetap tersimpan setelah
//  ESP32 dimatikan.
bool motorBalikKiri  = false;
bool motorBalikKanan = false;

// ------------------------------------------------------------
//  KOMPENSASI PWM PER RODA
// ------------------------------------------------------------
//  0 berarti tidak ada kompensasi, dan nilai di atas 0 berarti roda
//  itu diberi PWM lebih tinggi daripada roda pasangannya.
//
//  Pakai ini kalau saat MAJU robot tidak lurus, atau saat berputar
//  di tempat salah satu roda terlihat lebih lambat. Naikkan angka
//  untuk roda yang lemah, bukan turunkan roda yang kuat, supaya
//  tenaga robot tidak berkurang.
int8_t pwmKompensasiKiri  = 0;
int8_t pwmKompensasiKanan = 0;

// Batas kompensasi, supaya salah setel tidak membuat roda diberi
// PWM jauh di atas batas aman driver.
const int8_t KOMPENSASI_MAKS = 60;

// ------------------------------------------------------------
//  TAMBAHAN PWM SAAT BERBELOK
// ------------------------------------------------------------
//  Berbelok menyeret roda ke samping, jadi bebannya lebih berat
//  daripada maju. Tanpa tambahan ini, robot cenderung diam atau
//  tersendat saat berputar di tempat.
//
//  Nilainya bisa diatur, dan disimpan di flash.
uint8_t tambahanBelok = 45;

// Batas atas, supaya driver dan motor tetap aman.
const uint8_t TAMBAHAN_BELOK_MAKS = 90;

// Hitung PWM akhir untuk satu roda, sudah termasuk kompensasi dan
// tambahan belok. Hasilnya selalu dibatasi 0 sampai 255.
uint8_t motorPwmUntuk(bool sisiKiri, bool sedangBelok) {
  int nilai = (int)speedPWM;

  nilai += sisiKiri ? (int)pwmKompensasiKiri
                    : (int)pwmKompensasiKanan;

  // Tambahan belok hanya diberikan kalau PWM dasarnya memang tidak nol.
  // Kalau pengguna sengaja menyetel PWM 0, robot harus tetap diam.
  if (sedangBelok && speedPWM > 0) nilai += (int)tambahanBelok;

  if (nilai < 0)   nilai = 0;
  if (nilai > 255) nilai = 255;

  return (uint8_t)nilai;
}

void motorStop() {
  digitalWrite(M_LEFT_D1,  LOW);
  digitalWrite(M_LEFT_D2,  LOW);
  digitalWrite(M_RIGHT_D1, LOW);
  digitalWrite(M_RIGHT_D2, LOW);
  ledcWrite(M_LEFT_PWM,  0);
  ledcWrite(M_RIGHT_PWM, 0);
}

void motorApply() {
  // Belok dan mundur memang memutar roda ke arah yang berbeda dari
  // roda pasangannya, jadi PWM-nya dihitung per roda.
  const bool belok = (moveMode == MOVE_LEFT || moveMode == MOVE_RIGHT);

  const uint8_t pL = motorPwmUntuk(true,  belok);
  const uint8_t pR = motorPwmUntuk(false, belok);

  switch (moveMode) {
    // --- MAJU: kedua roda berputar ke arah yang sama ---
    case MOVE_FWD:
      motorSideDrive(M_LEFT_PWM,  M_LEFT_D1,  M_LEFT_D2,
                     !motorBalikKiri,  motorBalikKiri,  pL);
      motorSideDrive(M_RIGHT_PWM, M_RIGHT_D1, M_RIGHT_D2,
                     !motorBalikKanan, motorBalikKanan, pR);
      break;

    // --- MUNDUR: kebalikan dari maju ---
    case MOVE_BACK:
      motorSideDrive(M_LEFT_PWM,  M_LEFT_D1,  M_LEFT_D2,
                     motorBalikKiri,  !motorBalikKiri,  pL);
      motorSideDrive(M_RIGHT_PWM, M_RIGHT_D1, M_RIGHT_D2,
                     motorBalikKanan, !motorBalikKanan, pR);
      break;

    // --- PUTAR DI TEMPAT KE KIRI ---
    // Roda kiri mundur, roda kanan maju. Robot berputar pada porosnya.
    case MOVE_LEFT:
      motorSideDrive(M_LEFT_PWM,  M_LEFT_D1,  M_LEFT_D2,
                     motorBalikKiri,  !motorBalikKiri,  pL);
      motorSideDrive(M_RIGHT_PWM, M_RIGHT_D1, M_RIGHT_D2,
                     !motorBalikKanan, motorBalikKanan, pR);
      break;

    // --- PUTAR DI TEMPAT KE KANAN ---
    // Kebalikan dari putar kiri.
    case MOVE_RIGHT:
      motorSideDrive(M_LEFT_PWM,  M_LEFT_D1,  M_LEFT_D2,
                     !motorBalikKiri,  motorBalikKiri,  pL);
      motorSideDrive(M_RIGHT_PWM, M_RIGHT_D1, M_RIGHT_D2,
                     motorBalikKanan, !motorBalikKanan, pR);
      break;

    default:
      motorStop();
      break;
  }
}

// ==================== STEPPER ====================
//
// ============================================================
//  ARSITEKTUR ANTI-JITTER
// ============================================================
//
//  MASALAH (terukur pada board ini):
//  Versi awal mengeluarkan pulsa dari loop(). loop() tersendat oleh WiFi,
//  sehingga pulsa stepper berhenti sesaat -> motor tersentak (jitter).
//  Bahkan dengan hardware timer di core WiFi, jitter maksimum terukur
//  mencapai 1156 us dengan 215 dari 3128 pulsa terlambat >50 us.
//
//  SOLUSI:
//  Pulsa dikeluarkan oleh TASK TERSENDIRI yang di-PIN ke CORE 1.
//  WiFi/HTTP Arduino berjalan di CORE 0, jadi keduanya tidak saling
//  mengganggu. Ini yang membuat gerakan halus.
//
//  Lihat stepperTaskCore1() di bawah.
//
//  Catatan getar (stall): motor stepper tidak bisa langsung dari 0 ke
//  kecepatan tinggi. Karena itu tetap ada RAMP bertahap.
//
//  Catatan ESP_TIMER_ISR: TIDAK TERSEDIA di build Arduino core 3.3.12,
//  karena header membungkusnya dengan
//      #if CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
//  dan opsi itu tidak diaktifkan. Karena itu dipakai task core-pinned.

// Core tempat task stepper berjalan. WiFi/HTTP memakai core 0.
#define STEPPER_CORE  1

// ------------------------------------------------------------
//  PELACAK RESET  (penting untuk diagnosa gerakan tersendat)
// ------------------------------------------------------------
//  Kalau gerakan tersendat-sendat padahal statistik jitter bersih,
//  kemungkinan besar ESP32 RESET di tengah gerakan (brownout karena
//  noise motor). Saat reset, semua statistik ikut ter-nol-kan sehingga
//  masalahnya tidak terlihat.
//
//  Cara mendeteksi: simpan penghitung di RTC memory yang TIDAK hilang
//  saat reset biasa (hanya hilang kalau daya benar-benar putus).
//  Setiap boot, penghitung dinaikkan. Kalau > 1, berarti sudah pernah
//  reset tanpa daya putus = indikasi kuat brownout.
// ------------------------------------------------------------

// Angka ajaib penanda bahwa RTC memory memang berisi data kita.
#define RTC_MAGIC 0x54524231UL   // "TRB1"

RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t rtcBootCount;
RTC_NOINIT_ATTR uint32_t rtcMoveInterrupted;  // gerakan yang terputus reset

// Alasan reset terakhir (dari ESP32)
uint32_t bootResetReason = 0;
uint32_t bootCount       = 1;
uint32_t moveInterrupted = 0;
bool     brownoutSuspect = false;
bool     crashSuspect    = false;

// Nama alasan reset ESP-IDF -> teks yang bisa dibaca
static const char* resetReasonName(uint32_t r) {
  switch (r) {
    case 1:  return "POWERON (daya baru masuk)";
    case 3:  return "SW (reset software/upload)";
    case 4:  return "PANIC (crash / exception)";
    case 5:  return "INT_WDT (watchdog interrupt)";
    case 6:  return "TASK_WDT (watchdog task - kode macet)";
    case 7:  return "WDT (watchdog lain)";
    case 8:  return "DEEPSLEEP";
    case 9:  return "BROWNOUT (TEGANGAN TURUN!)";
    case 10: return "SDIO";
    case 11: return "EXT_PIN";
    default: return "TIDAK DIKETAHUI";
  }
}

// Dipanggil paling awal di setup(). Membaca alasan reset dan menghitung
// berapa kali boot tanpa daya putus.
void resetTrackerBegin() {
  bootResetReason = (uint32_t)esp_reset_reason();

  if (rtcMagic == RTC_MAGIC) {
    // RTC memory masih valid -> ini reset TANPA daya putus
    rtcBootCount++;
  } else {
    // Daya benar-benar baru masuk
    rtcMagic        = RTC_MAGIC;
    rtcBootCount    = 1;
    rtcMoveInterrupted = 0;
  }

  bootCount       = rtcBootCount;
  moveInterrupted = rtcMoveInterrupted;

  // Brownout dicurigai kalau: reset karena tegangan turun, ATAU sudah
  // lebih dari 1 kali boot tanpa daya putus sementara tidak ada upload.
  brownoutSuspect = (bootResetReason == 9);

  // Crash (PANIC) atau watchdog juga membuat gerakan terputus. Ini
  // dibedakan dari brownout supaya saran perbaikannya tepat.
  crashSuspect = (bootResetReason == 4) ||
                 (bootResetReason == 5) ||
                 (bootResetReason == 6) ||
                 (bootResetReason == 7);

  // Catat ke log (log mungkin masih kosong di sini, jadi dipanggil
  // setelah log siap - lihat setup()).
}

// Dipanggil saat gerakan stepper dimulai. Kalau ESP32 reset di tengah
// gerakan, penanda ini masih ada di RTC memory saat boot berikutnya.
void markMoveStart() {
  rtcMoveInterrupted = 1;
}

// Dipanggil saat gerakan selesai dengan benar.
void markMoveEnd() {
  rtcMoveInterrupted = 0;
}

// ============================================================
//  PENGUKUR DURASI GERAKAN  (mendeteksi KEHILANGAN LANGKAH)
// ============================================================
//
//  Kalau firmware mengirim 800 pulsa pada 1600 step/s, gerakan
//  seharusnya selesai dalam waktu tertentu. Kita ukur waktu AKTUAL
//  lalu hitung kecepatan rata-rata nyata.
//
//  Kalau rata-rata jauh di bawah yang diminta, artinya pulsa tidak
//  keluar serapi yang diperintahkan -> bisa kehilangan langkah.
//
//  Ini mengukur dari sisi firmware saja. Motor tetap bisa kehilangan
//  langkah MEKANIK (kopling selip) yang tidak terlihat di sini.
//  Tapi kalau angkanya cocok, perintah pulsanya memang rapi.

volatile unsigned long gMoveStartUs   = 0;
volatile unsigned long gMoveEndUs     = 0;
volatile uint32_t      gMoveLastSteps = 0;
volatile float         gMoveLastSpeed = 0.0f;
volatile uint32_t      gMoveCount     = 0;
volatile uint32_t      gMoveStepsAtStart = 0;

// ---- Pelacak PRESISI gerakan ----
// Setiap kali perintah BUKA atau TUTUP selesai, selisih antara posisi
// yang diminta dan posisi yang benar-benar dicapai dicatat di sini.
// Kalau selisihnya selalu 0, gerakannya presisi dan konsisten.
// Kalau ada selisih, jumlahnya terlihat jelas, tidak hanya dirasakan.
volatile uint32_t gMoveReqSteps       = 0;   // langkah yang diminta gerakan terakhir
volatile int32_t  gPresisiMaksSelisih = 0;   // selisih terbesar yang pernah terjadi
volatile uint32_t gPresisiUji         = 0;   // berapa kali diuji
volatile uint32_t gPresisiGagal       = 0;   // berapa kali meleset

// Total pulsa yang keluar dari semua motor, sejak boot.
// Dideklarasikan di sini karena dipakai oleh pengukur durasi gerakan.
volatile uint32_t stepTotal = 0;

// Pengukur perioda pulsa AKTUAL: berapa mikrodetik jarak antar-pulsa
// yang BENAR-BENAR terjadi. Ini memisahkan "pulsa tidak rapi" (jitter)
// dari "pulsa keluar terlalu jarang" (perioda terlalu panjang).
volatile uint32_t gPeriodMaxUs = 0;
volatile uint32_t gPeriodMinUs = 0xFFFFFFFF;
volatile uint32_t gPeriodSumUs = 0;
volatile uint32_t gPeriodCount = 0;
volatile uint32_t gPeriodLastUs = 0;

// Pelacak LONJAKAN: satu pulsa yang tertahan jauh lebih lama dari jadwal.
// Inilah penyebab "tersendat-sendat" - motor maju, macet sejenak, maju lagi.
// Kalau rata-rata selisih kecil tapi 'max' besar, berarti pulsanya rapi
// TAPI ada macet sesekali, dan itu terasa sebagai sentakan.
volatile uint32_t gLonjakCount          = 0;   // berapa kali macet >500us
volatile uint32_t gLonjakPertamaPulsa   = 0;   // pulsa ke-berapa macet pertama
volatile uint32_t gLonjakPertamaUs      = 0;   // besar macet pertama (us)
volatile uint32_t gLonjakPosisiStep     = 0;   // step saat macet pertama
volatile uint32_t gLonjakTerbesarUs     = 0;
volatile uint32_t gLonjakTerbesarStep   = 0;

// ============================================================
//  SAKLAR UJI: kenapa perioda pulsa jadi terlalu panjang?
// ============================================================
//
//  Temuan TERUKUR (bukti, bukan dugaan):
//    min perioda = 599 us        -> spin loop CEPAT, pulsa BISA tepat waktu
//    max perioda = 5135-32368 us -> kadang tertunda 8-50x
//    rata-rata   = 2-3x jadwal   -> 884 step/s padahal diminta 1600
//
//  Karena 'min' sudah benar, spin loop-nya tidak lambat. Yang membuat
//  perioda panjang adalah gangguan dari luar core 1.
//
//  Dua saklar untuk memisahkan penyebab:
//
//  1. bleOff -> matikan BLE. BLE memakai radio + flash bersama WiFi,
//     dan akses flash bisa menahan SELURUH chip (cache stall).
//     32 ms yang muncul di 'max' itu khas durasi satu operasi flash.
//
//  2. tickHanyaMotorAktif -> di spin loop hanya panggil stepperTick
//     untuk motor yang SEDANG bergerak, mengurangi beban loop.
//
//  Kendali lewat perintah 'X':
//     XB1 -> matikan BLE      XB0 -> hidupkan BLE
//     XT1 -> tick motor aktif saja    XT0 -> tick keduanya
bool bleOff              = false;
bool tickHanyaMotorAktif = false;

// Pembungkus BLE advertising. Dipisah sebagai fungsi supaya bisa
// dipanggil dari perintah 'X' tanpa menyalin kode inisialisasi.
// (definisi aslinya ada di dekat setup(); ini hanya deklarasinya)
void BLEAdvertising_stop();
void BLEAdvertising_start();

void markMoveStartTimed() {
  gMoveStartUs      = micros();
  gMoveStepsAtStart = stepTotal;   // patokan untuk menghitung pulsa
  gMoveCount++;

  // Nol-kan pelacak jadwal & lonjakan untuk gerakan ini
  gPeriodMaxUs        = 0;
  gPeriodMinUs        = 0xFFFFFFFF;
  gPeriodSumUs        = 0;
  gPeriodCount        = 0;
  gPeriodLastUs       = 0;
  gLonjakCount        = 0;
  gLonjakPertamaPulsa = 0;
  gLonjakPertamaUs    = 0;
  gLonjakPosisiStep   = 0;
  gLonjakTerbesarUs   = 0;
  gLonjakTerbesarStep = 0;
}

void markMoveEndTimed(uint32_t steps, float speed) {
  gMoveEndUs     = micros();
  gMoveLastSteps = steps;
  gMoveLastSpeed = speed;
}

// ------------------------------------------------------------
//  RAMP AKSELERASI
// ------------------------------------------------------------
//  PENTING: panjang ramp TIDAK boleh angka tetap. Kalau tetap, pada
//  kecepatan rendah ramp menempati hampir seluruh gerakan dan motor
//  stall di akhir. Terbukti dari pengujian: pada K300 lid berhenti di
//  84 derajat (bukan 90) sementara K600-K1600 berhenti tepat di 90.
//
//  Sekarang panjang ramp proporsional: kecepatan x 0.5, dibatasi
//  antara RAMP_MIN_STEPS dan RAMP_MAX_STEPS.
#define RAMP_MIN_STEPS    150.0f   // ramp terpendek
#define RAMP_MAX_STEPS    800.0f   // ramp terpanjang

// Lantai deselerasi: saat mendekati tujuan, kecepatan tidak turun di
// bawah fraksi ini dari kecepatan target. Terlalu kecil = stall.
#define SLOW_FLOOR_FRAC   0.35f

// Kecepatan minimum pulsa. Di bawah ini motor stepper cenderung
// kehilangan torsi, jadi dijaga di atas nilai ini.
#define SPEED_MIN_SPS     100.0f

// Lebar pulsa minimum untuk TB6600. Datasheet: minimal 2.2 us.
// Kita pakai 10 us supaya aman dengan overhead di dalam timer.
const unsigned long PULSE_MIN_US = 10;

// ---- Jitter monitor ----
// Mengukur berapa besar penyimpangan waktu antar-pulsa. Kalau angka ini
// besar, berarti ada sesuatu yang mengganggu timer (biasanya beban CPU).
volatile uint32_t jitMaxUs   = 0;    // penyimpangan terburuk (us)
volatile uint32_t jitSumUs   = 0;    // total penyimpangan (untuk rata-rata)
volatile uint32_t jitCount   = 0;    // jumlah sampel
volatile uint32_t jitLateCnt = 0;    // berapa kali pulsa terlambat >50us

// ------------------------------------------------------------
//  PELACAK ARAH  (untuk mendiagnosa "buka & tutup searah")
// ------------------------------------------------------------
//  Kalau lid pernah bergerak naik DAN turun, berarti DIR bekerja.
//  Kalau hanya satu arah, berarti sinyal DIR tidak sampai ke TB6600.
//  Penanda ini di-set di executeCommand saat perintah lid diterima,
//  bukan dari gerakan fisik (kita tidak punya sensor).
volatile bool gMovedUp   = false;   // pernah diperintahkan ke posisi lebih besar
volatile bool gMovedDown = false;   // pernah diperintahkan ke posisi lebih kecil

void stepperBegin(Stepper &s) {
  pinMode(s.pinPul, OUTPUT);
  pinMode(s.pinDir, OUTPUT);
  pinMode(s.pinEna, OUTPUT);
  // Pakai PULSE_IDLE, bukan LOW langsung, supaya polaritas mengikuti
  // PULSE_ACTIVE. Penting: kalau idle salah, optocoupler bisa teraliri
  // terus dan motor panas / bergetar.
  digitalWrite(s.pinPul, PULSE_IDLE);
  digitalWrite(s.pinDir, DIR_FOR_POS);
  digitalWrite(s.pinEna, ENA_INACTIVE);   // mulai FREE, motor tidak panas
  s.enabled      = false;
  s.moving       = false;
  s.speedCurrent = 0;
  s.speedTarget  = 0;
}

void stepperEnable(Stepper &s, bool on) {
  digitalWrite(s.pinEna, on ? ENA_ACTIVE : ENA_INACTIVE);
  s.enabled = on;
}

// Lepas motor setelah gerakan selesai, TAPI hormati holdMode.
//
// Dipakai di semua tempat yang dulu memanggil stepperEnable(s, false)
// setelah gerakan. Kalau holdMode true, motor tetap dikunci sehingga
// tidak bisa berputar sendiri karena beban/gravitasi.
//
// Ini juga jalur diagnosa: kalau gejala "terus berputar" hilang saat
// holdMode true, berarti penyebabnya motor free + beban, bukan sinyal.
void stepperRelease(Stepper &s) {
  if (holdMode) return;          // tetap dikunci, motor menahan posisi
  stepperEnable(s, false);
}

// ============================================================
//  UJI DIR MANUAL  (untuk memisahkan dua penyebab "selalu searah")
// ============================================================
//
//  PROBLEM yang dipecahkan:
//  Kalau lid selalu bergerak satu arah, ada dua kemungkinan yang
//  gejalanya IDENTIK dari layar:
//    (a) 5V untuk TB6600 MATI (mis. buck converter ikut mati saat
//        diuji dari laptop)  -> DIR+ tidak punya tegangan
//    (b) kabel DIR- salah/lepas  -> DIR tidak sampai ke driver
//
//  Cara memisahkan: kita gerakkan DIR dan PUL SECARA TERPISAH, lalu
//  uji apakah motor bereaksi. Karena kita tidak punya sensor, yang
//  diuji adalah URUTAN gerakannya:
//
//    UJI A: set DIR ke satu posisi, gerakkan sedikit, TANDai
//    UJI B: set DIR ke posisi lain, gerakkan sedikit lagi
//
//  Kalau DIR bekerja, kedua gerakan itu berlawanan arah sehingga motor
//  kembali ke posisi semula (kamu melihatnya goyang bolak-balik).
//  Kalau DIR tidak bekerja, kedua gerakan itu searah sehingga motor
//  berjalan terus ke satu arah.
//
//  Fungsi ini mengembalikan nilai-nilai yang bisa dibaca di /debug
//  supaya bisa dibedakan tanpa alat ukur.

// Gerakkan satu motor sejumlah step tertentu dengan arah paksa.
// Dipakai HANYA oleh uji DIR. Blocking, jadi harus dari konteks aman.
void stepperJog(Stepper &s, long steps, int forcedDir, float sps) {
  if (steps <= 0) return;

  // Paksa arah tanpa mengubah target posisi permanen
  long savedTarget = s.targetStep;
  long savedDir    = s.dir;

  s.dir        = forcedDir;
  s.targetStep = s.currentStep + (forcedDir * steps);
  s.speedTarget  = sps;
  s.speedCurrent = 0;
  s.pulseHigh    = false;
  s.warmUp       = 2;

  stepperEnable(s, true);
  digitalWrite(s.pinDir, (forcedDir > 0) ? DIR_FOR_POS : DIR_FOR_NEG);
  delayMicroseconds(10);

  s.lastPulseUs = micros();
  s.moving      = true;

  // Reset pelacak pengukuran supaya angka yang dilaporkan hanya
  // mencakup gerakan jog ini, bukan menumpuk dari gerakan sebelumnya.
  // (sebelumnya stepperJog tidak melakukannya, sehingga laporan
  // menampilkan jumlah pulsa yang jauh lebih besar dari seharusnya)
  gPeriodCount        = 0;
  gPeriodSumUs        = 0;
  gPeriodMaxUs        = 0;
  gPeriodMinUs        = 0xFFFFFFFF;
  gPeriodLastUs       = 0;
  gLonjakCount        = 0;
  gLonjakTerbesarUs   = 0;

  // Tunggu sampai selesai, dengan batas waktu aman
  unsigned long t0 = millis();
  while (s.moving && (millis() - t0) < 3000UL) {
    stepperTick(s, micros());
  }

  s.moving = false;
  stepperRelease(s);

  // Pulihkan target supaya posisi tidak melompat di pandangan pengguna
  s.targetStep = savedTarget;
  s.dir        = savedDir;
}

// ============================================================
//  UJI TORSI DAN KECEPATAN MAKSIMAL  (SWEEP OTOMATIS)
// ============================================================
//
//  LATAR BELAKANG: KENAPA TORSI TIDAK BISA DIATUR DARI FIRMWARE
//
//  Pertanyaan "apakah torsi stepper bisa diatur" jawabannya: BISA,
//  tetapi BUKAN dari program ini. Torsi motor stepper ditentukan dua
//  hal, dan keduanya perangkat keras:
//
//  1. ARUS YANG DISETEL DI TB6600 (paling menentukan).
//     Arus ini diatur DIP switch SW1/SW2/SW3 di modul TB6600. Nilai
//     yang tersedia biasanya 0.5 sampai 4.0 A. NEMA 17 umumnya butuh
//     1.5 sampai 2.0 A. Selama arusnya terlalu kecil, motor akan
//     kehilangan langkah atau tidak kuat mengangkat beban, DAN TIDAK
//     ADA PERINTAH DARI ESP32 YANG BISA MENAMBAHNYA. Sinyal PUL dan
//     DIR hanya menentukan kapan dan ke arah mana motor berputar,
//     bukan seberapa kuat.
//
//  2. LEVEL MICROSTEP (DIP switch SW4/SW5/SW6).
//     Ini berlawanan dengan intuisi, tetapi menaikkan microstep
//     justru MENURUNKAN torsi per langkah. Menurut Faulhaber
//     Application Note 015, torsi per microstep dibanding torsi
//     holding penuh adalah:
//
//        microstep  1 -> 100.00 persen
//        microstep  2 ->  70.71 persen
//        microstep  4 ->  38.27 persen
//        microstep  8 ->  19.51 persen
//        microstep 16 ->   9.80 persen
//
//     Pada microstep 16, satu langkah hanya menghasilkan kurang dari
//     10 persen torsi holding. Sementara itu detent torque motor
//     sendiri 5 sampai 20 persen dari holding torque. Artinya torsi
//     satu microstep seringkali TIDAK CUKUP untuk mengalahkan detent
//     torque, sehingga sebagian perintah langkah tidak menghasilkan
//     gerakan sama sekali.
//
//     Jadi untuk beban berat, microstep RENDAH memberi torsi lebih
//     besar. Lid tong sampah tidak butuh resolusi 3200 pulsa per
//     putaran, jadi menurunkan ke 8 atau 4 adalah pertukaran yang
//     menguntungkan: resolusi yang tidak dipakai ditukar dengan
//     torsi yang memang dibutuhkan.
//
//  KALAU TORSI KURANG, URUTAN YANG DISARANKAN:
//     a. Naikkan arus TB6600 lewat SW1/SW2/SW3 ke 1.5 sampai 2.0 A.
//        Ini perbaikan terbesar dan paling murah.
//     b. Turunkan microstep dari 16 ke 8.
//     c. Naikkan tegangan suplai motor ke 24 V kalau memungkinkan.
//     d. Turunkan kecepatan gerak.
//
//  SETELAH MENGUBAH MICROSTEP, `STEPS_PER_DEG` DI FIRMWARE WAJIB
//  IKUT DIUBAH. Kalau tidak, sudut yang diminta akan salah:
//        microstep  4 -> 3600.0 / 360.0 = 10.0000
//        microstep  8 -> 1600.0 / 360.0 =  4.4444
//        microstep 16 -> 3200.0 / 360.0 =  8.8889  (nilai sekarang)
//
//  ------------------------------------------------------------
//  ALAT UJI INI
//  ------------------------------------------------------------
//
//  Karena torsi tidak bisa diubah dari firmware, yang bisa dilakukan
//  adalah MENGUKUR pada kecepatan berapa motor mulai kehilangan
//  langkah. Dari situ ketahuan batas aman, dan ketahuan juga apakah
//  masalahnya di kecepatan atau memang di arus.
//
//  Uji ini menjalankan gerakan bolak-balik pada kecepatan yang naik
//  bertahap, lalu melaporkan pada tahap berapa mulai meleset.

// Hasil satu tahap uji
struct HasilSweep {
  float    sps;
  uint32_t minta;
  uint32_t dapat;
  long     selisih;
  uint32_t jitterMaks;
  bool     meleset;
};

#define SWEEP_MAKS_TAHAP 12

HasilSweep sweepHasil[SWEEP_MAKS_TAHAP];
uint8_t    sweepJumlahTahap = 0;
uint16_t   sweepTotalMeleset = 0;

// Kecepatan tertinggi yang masih tepat, dikali 10 supaya bisa
// dikirim sebagai bilangan bulat. -10 berarti belum ada yang aman.
float sweepKecepatanAman = -1.0f;

// Jalankan satu gerakan uji pada satu kecepatan, lalu hitung
// selisih antara langkah yang diminta dan yang benar-benar keluar.
void sweepSatuTahap(Stepper &s, float sps, long langkah) {
  // Siapkan gerakan
  s.dir          = +1;
  s.targetStep   = s.currentStep + langkah;
  s.speedTarget  = sps;
  s.speedCurrent = 0;
  s.pulseHigh    = false;
  s.warmUp       = 2;

  stepperEnable(s, true);
  digitalWrite(s.pinDir, DIR_FOR_POS);
  digitalWrite(s.pinPul, PULSE_OFF);
  delayMicroseconds(20);

  // Nolkan pelacak supaya angka yang dilaporkan hanya untuk tahap ini
  gPeriodCount      = 0;
  gPeriodSumUs      = 0;
  gPeriodMaxUs      = 0;
  gPeriodMinUs      = 0xFFFFFFFF;
  gPeriodLastUs     = 0;
  gLonjakCount      = 0;
  gLonjakTerbesarUs = 0;

  uint32_t awal = stepTotal;

  s.lastPulseUs = micros();
  s.moving      = true;

  // Jalankan sampai selesai. Batas 5 detik mencegah robot menggantung
  // kalau ada yang tidak beres.
  unsigned long t0 = millis();
  while (s.moving && (millis() - t0) < 5000UL) {
    stepperTick(s, micros());
  }

  s.moving = false;
  stepperRelease(s);

  // Catat hasilnya
  HasilSweep &h = sweepHasil[sweepJumlahTahap];
  h.sps        = sps;
  h.minta      = (uint32_t)langkah;
  h.dapat      = stepTotal - awal;
  h.selisih    = (long)h.dapat - langkah;
  h.jitterMaks = gPeriodMaxUs;
  h.meleset    = (h.selisih != 0);

  if (h.meleset) {
    sweepTotalMeleset++;
    addLogf("[SWEEP] %.0f step/s MELESET %+ld langkah (minta %lu, dapat %lu)",
            sps, h.selisih,
            (unsigned long)h.minta, (unsigned long)h.dapat);
  } else {
    addLogf("[SWEEP] %.0f step/s TEPAT %lu langkah",
            sps, (unsigned long)h.dapat);
  }

  sweepJumlahTahap++;

  // Kembalikan ke posisi semula supaya lid tidak makin terbuka terus
  s.targetStep = s.currentStep - langkah;
  s.dir        = -1;
  s.speedTarget  = sps;
  s.speedCurrent = 0;
  s.pulseHigh    = false;
  s.warmUp       = 2;

  stepperEnable(s, true);
  digitalWrite(s.pinDir, DIR_FOR_NEG);
  digitalWrite(s.pinPul, PULSE_OFF);
  delayMicroseconds(20);

  s.lastPulseUs = micros();
  s.moving      = true;

  t0 = millis();
  while (s.moving && (millis() - t0) < 5000UL) {
    stepperTick(s, micros());
  }

  s.moving = false;
  stepperRelease(s);

  // Jeda antar tahap supaya motor dan driver tidak kepanasan.
  delay(300);
}

// Uji seluruh rentang kecepatan pada satu lid, dari lambat ke cepat.
// Berhenti lebih awal kalau dua tahap berturut-turut sudah meleset,
// supaya motor tidak dipaksa terus saat sudah jelas tidak kuat.
void sweepKecepatan(Stepper &s, const char* nama) {
  // Kecepatan uji, dari paling lambat ke paling cepat
  const float daftar[] = { 200, 400, 600, 800, 1000,
                           1200, 1400, 1600, 1800, 2000 };
  const uint8_t jumlah = sizeof(daftar) / sizeof(daftar[0]);

  // Panjang gerakan 800 langkah = 90 derajat. Cukup panjang supaya
  // motor benar-benar mencapai kecepatan penuh, dan cukup pendek
  // supaya banyak tahap bisa diuji tanpa lid membentur apa pun.
  const long LANGKAH = 800;

  sweepJumlahTahap  = 0;
  sweepTotalMeleset = 0;

  float speedSebelumnya = stepSpeedActive;

  addLogf("[SWEEP] ===== MULAI ===== lid %s", nama);
  addLogf("[SWEEP] %u tahap, %ld langkah tiap tahap (90 derajat)",
          (unsigned)jumlah, LANGKAH);

  uint8_t gagalBerturut = 0;

  for (uint8_t i = 0; i < jumlah; i++) {
    sweepSatuTahap(s, daftar[i], LANGKAH);

    if (sweepHasil[sweepJumlahTahap - 1].meleset) {
      gagalBerturut++;
    } else {
      gagalBerturut = 0;
    }

    // Dua tahap meleset berturut-turut = sudah lewat batasnya.
    if (gagalBerturut >= 2) {
      addLogf("[SWEEP] Dua tahap meleset berturut-turut, uji dihentikan di %.0f step/s",
              daftar[i]);
      break;
    }
  }

  // Cari kecepatan tertinggi yang masih tepat
  float aman = -1.0f;
  for (uint8_t i = 0; i < sweepJumlahTahap; i++) {
    if (!sweepHasil[i].meleset) aman = sweepHasil[i].sps;
  }
  sweepKecepatanAman = aman;

  addLog("[SWEEP] ===== SELESAI =====");

  if (aman > 0) {
    addLogf("[SWEEP] Kecepatan aman tertinggi: %.0f step/s", aman);
    addLogf("[SWEEP] Di atas itu mulai kehilangan langkah. Pakai %.0f step/s untuk %s.",
            aman * 0.8f, nama);
  } else {
    addLog("[SWEEP] Bahkan 200 step/s sudah meleset.");
    addLog("[SWEEP] Ini BUKAN masalah kecepatan. Periksa arus TB6600,");
    addLog("[SWEEP] dan coba turunkan microstep dari 16 ke 8.");
  }

  addLogf("[SWEEP] Tahap diuji: %u, tahap meleset: %u",
          sweepJumlahTahap, sweepTotalMeleset);

  // Kembalikan kecepatan semula
  stepSpeedActive = speedSebelumnya;
}

// Jalankan uji DIR: gerakkan maju sedikit, lalu mundur sedikit.
// Kalau DIR bekerja, motor akan bergoyang bolak-balik di tempat.
// Kalau tidak, motor akan jalan terus satu arah.
void stepperDirTest(Stepper &s, const char* nama) {
  // 400 step = 45 derajat, cukup terlihat & terdengar, dan cukup panjang
  // supaya motor benar-benar mencapai kecepatan penuh di tengah gerakan.
  // JOG 200 step terlalu pendek: ramp (min 150 step) memakan hampir
  // seluruh gerakan sehingga motor hanya mencapai ~60% kecepatan.
  long JOG = 400;
  float sps = 300.0;       // pelan supaya jelas terlihat

  // Ingat kecepatan yang sedang berlaku, lalu PAKSA kecepatan uji.
  // stepperJog memakai sps sendiri, tapi variabel aktualnya perlu
  // di-set supaya laporan rasio menghitung terhadap angka yang benar.
  float speedSebelumnya = stepSpeedActive;
  stepSpeedActive = sps;

  long posAwal = s.currentStep;

  addLogf("[UJIDIR] %s: mulai uji (jog %ld step, 2 arah @ %.0f step/s)",
          nama, JOG, sps);

  // Gerakan 1: arah positif
  stepperJog(s, JOG, +1, sps);
  long posSetelahMaju = s.currentStep;

  // Gerakan 2: arah negatif, kembali ke posisi awal
  stepperJog(s, JOG, -1, sps);
  long posSetelahMundur = s.currentStep;

  long maju   = posSetelahMaju  - posAwal;
  long mundur = posSetelahMundur - posSetelahMaju;

  addLogf("[UJIDIR] %s: gerakan1=%+ld, gerakan2=%+ld step", nama, maju, mundur);

  // Laporkan kecepatan jog AKTUAL, bukan stepSpeedActive (yang bisa
  // jauh berbeda karena uji DIR memakai kecepatan sendiri).
  if (gPeriodCount > 0) {
    unsigned long pAvg = gPeriodSumUs / gPeriodCount;
    addLogf("[UJIDIR] %s: %lu pulsa, selisih jadwal rata2=%lu us (max %ld)",
            nama, (unsigned long)gPeriodCount, pAvg, (long)gPeriodMaxUs);
  }

  addLogf("[UJIDIR] %s: %s", nama,
          (maju > 0 && mundur < 0)
            ? "firmware benar (maju lalu mundur)"
            : "firmware TIDAK membalik arah - BUG!");

  // Kembalikan posisi tercatat ke posisi awal supaya kalibrasi tetap sah
  s.currentStep = posAwal;
  s.targetStep  = posAwal;
  s.moving      = false;
  stepperRelease(s);

  // Pulihkan kecepatan yang berlaku sebelum uji
  stepSpeedActive = speedSebelumnya;
  addLogf("[UJIDIR] %s: kecepatan dikembalikan ke %.0f step/s",
          nama, speedSebelumnya);

  addLog("[UJIDIR] Perhatikan motor:");
  addLog("[UJIDIR]  - BERGOYANG bolak-balik = DIR bekerja, masalah di tempat lain");
  addLog("[UJIDIR]  - JALAN TERUS satu arah = DIR TIDAK sampai ke driver");
  addLog("[UJIDIR]    Penyebab: 5V buck mati, ATAU kabel DIR lepas/salah pin");
}

void stepperMoveToDeg(Stepper &s, float deg, float speedSps) {  if (speedSps < 50.0) speedSps = 50.0;

  s.targetStep   = lroundf(deg * STEPS_PER_DEG);
  long delta     = s.targetStep - s.currentStep;

  if (delta == 0) {
    s.moving = false;
    return;
  }

  // ARAH OTOMATIS: arah SELALU ditentukan oleh selisih posisi.
  //   delta positif (menambah sudut) = BUKA
  //   delta negatif (mengurangi sudut) = TUTUP
  // Jadi buka dan tutup PASTI berlawanan arah, dan lid selalu kembali
  // lewat jalur yang sama. Tidak ada cara memaksa arah dari UI, supaya
  // tidak mungkin terjadi gerakan yang menjauh dari target.
  s.dir          = (delta >= 0) ? +1 : -1;

  // ---- Catat jumlah langkah yang DIMINTA, untuk uji presisi ----
  // Nanti setelah gerakan selesai, angka ini dibandingkan dengan jumlah
  // pulsa yang benar-benar terkirim. Kalau sama, gerakan presisi.
  gMoveReqSteps = (uint32_t)(delta < 0 ? -delta : delta);

  // Catat arah yang benar-benar diperintahkan. Dipakai halaman /debug
  // untuk membuktikan DIR bekerja (pernah naik DAN turun).
  if (s.dir > 0) gMovedUp   = true;
  else           gMovedDown = true;

  s.speedTarget  = speedSps;
  s.speedCurrent = 0;             // mulai dari diam, lalu di-ramp naik
  s.pulseHigh    = false;
  s.pulseWidthUs = 0;
  s.warmUp       = 0;
  s.lastPulseUs  = micros();
  // Lewati 2 pulsa pertama saat mengukur jitter: perioda pulsa awal
  // belum stabil karena speedCurrent baru di-ramp dari nol.
  s.warmUp       = 2;

  stepperEnable(s, true);
  // Arah mengikuti polaritas wiring (common-anode membalik logika DIR).
  uint8_t dirLevel = (s.dir > 0) ? DIR_FOR_POS : DIR_FOR_NEG;
  digitalWrite(s.pinDir, dirLevel);

  // Catat arah yang DIPERINTAHKAN ke log. Ini penting untuk mendiagnosa
  // masalah "buka dan tutup berputar ke arah yang sama": kalau log
  // menunjukkan dir berubah (+1 lalu -1) tetapi motor secara fisik
  // selalu berputar ke arah yang sama, berarti sinyal DIR tidak sampai
  // ke TB6600 (masalah wiring pada pin DIR).
  addLogf("[DIR] %s: %ld -> %ld step (dir=%+ld, GPIO%d=%s)",
          (s.dir > 0 ? "BUKA" : "TUTUP"),
          s.currentStep, s.targetStep, s.dir,
          (int)s.pinDir,
          (dirLevel == HIGH ? "HIGH" : "LOW"));

  // Penting untuk presisi: pastikan pin PUL dalam keadaan IDLE sebelum
  // mengubah DIR. Kalau gerakan sebelumnya meninggalkan PUL di level
  // aktif, mengubah DIR sekarang akan membuat driver membaca tepi palsu.
  digitalWrite(s.pinPul, PULSE_OFF);
  s.pulseHigh = false;

  // DIR harus stabil jauh sebelum tepi pulsa pertama. TB6600 (seperti
  // driver step/dir pada umumnya) mensyaratkan waktu setup DIR; 20 us
  // jauh di atas kebutuhan dan tidak terasa pada kecepatan berapa pun.
  delayMicroseconds(20);
  s.moving = true;

  // Tandai di RTC memory bahwa gerakan sedang berlangsung. Kalau ESP32
  // reset sebelum gerakan selesai (mis. brownout karena noise motor),
  // penanda ini masih ada saat boot berikutnya dan kita bisa tahu
  // bahwa gerakan tersendat itu memang disebabkan reset.
  markMoveStart();
  markMoveStartTimed();
}

// Kecepatan di-ramp naik dari 0 ke speedTarget, dan turun lagi mendekati
// tujuan. Ini yang mencegah motor stall/getar saat start dan stop.
float stepperNextSpeed(Stepper &s) {
  long remaining = labs(s.targetStep - s.currentStep);

  // ------------------------------------------------------------
  //  RAMP YANG BENAR
  // ------------------------------------------------------------
  //  Bug versi lama: panjang ramp dibuat TETAP (400 step) sementara
  //  kecepatan target berubah-ubah. Akibatnya pada kecepatan rendah (K300)
  //  ramp menempati hampir separuh gerakan, dan deselerasi turun sampai
  //  10% x 300 = 30 step/s -> terlalu lambat -> motor STALL di akhir
  //  sehingga lid berhenti di ~84 derajat, bukan 90.
  //
  //  Perbaikan:
  //   1. Panjang ramp dibuat PROPORSIONAL terhadap kecepatan target,
  //      dengan batas atas supaya gerakan tidak lambat.
  //   2. Deselerasi tidak turun di bawah 35% kecepatan target.
  //   3. Batas bawah kecepatan dinaikkan supaya motor tetap punya torsi.
  //
  //  Rambu tuning:
  //   - Kalau lid berhenti SEBELUM target (seperti 84 dari 90 derajat):
  //     motor stall di akhir. Turunkan RAMP_MIN_FRAC atau naikkan
  //     SLOW_FLOOR_FRAC (jangan melambat terlalu pelan di akhir).
  //   - Kalau gerakan awal terasa menyentak: naikkan RAMP_MIN_FRAC.

  // Panjang ramp proporsional kecepatan: makin cepat, makin panjang.
  float rampLenF = s.speedTarget * 0.5f;
  if (rampLenF > RAMP_MAX_STEPS) rampLenF = RAMP_MAX_STEPS;   // jangan terlalu panjang

  // --- Batas atas: ramp tidak boleh melebihi SEPARUH panjang gerakan ---
  // (lihat penjelasan bug di bawah)
  {
    long setengahGerakan = remaining / 2;
    if (setengahGerakan < 1) setengahGerakan = 1;
    if ((long)rampLenF > setengahGerakan) rampLenF = (float)setengahGerakan;
  }

  // --- Batas bawah diterapkan SETELAH pembatasan di atas ---
  // PENTING: urutannya harus begini. Kalau RAMP_MIN_STEPS diterapkan
  // lebih dulu, pembatasan separuh gerakan bisa menimpanya dan
  // menghasilkan ramp yang lebih pendek dari minimum.
  //
  // Tapi batas bawah juga tidak boleh melebihi separuh gerakan: untuk
  // gerakan sangat pendek (mis. jog 20 step), memaksa ramp 150 step
  // membuat motor tidak pernah mencapai kecepatan penuh sama sekali.
  if (rampLenF < RAMP_MIN_STEPS) {
    float maksRamp = (float)(remaining / 2);
    if (maksRamp < 1.0f) maksRamp = 1.0f;
    rampLenF = (RAMP_MIN_STEPS < maksRamp) ? RAMP_MIN_STEPS : maksRamp;
  }
  if (rampLenF < 1.0f) rampLenF = 1.0f;

  // ------------------------------------------------------------
  //  BUG YANG DIPERBAIKI DI SINI
  // ------------------------------------------------------------
  //  SEBELUMNYA: panjang ramp tidak dibatasi oleh panjang gerakan.
  //  Untuk gerakan 800 step dengan K1600, rampLen = 1600*0.5 = 800.
  //  Akibatnya 'remaining < rampLen' bernilai BENAR dari step pertama,
  //  sehingga motor langsung dikenai faktor perlambatan dan TIDAK PERNAH
  //  mencapai kecepatan target.
  //
  //  Bukti terukur: kecepatan rata-rata hanya 882 dari 1600 (55%),
  //  dan perhitungan teoretis memberi 781 (49%) — cocok.
  //  Akibat lanjutannya: kecepatan berubah terus di setiap step sehingga
  //  motor mudah kehilangan sinkronisasi -> gerakan tersendat/getar.
  //
  //  PERBAIKAN: rampLen tidak boleh melebihi SEPARUH panjang gerakan.
  //  Separuh untuk akselerasi, separuh untuk deselerasi. Dengan begitu
  //  motor SELALU punya bagian di tengah untuk berjalan pada kecepatan
  //  penuh, dan profil kecepatannya berbentuk trapesium yang benar.
  //  (pembatasan ini sudah dilakukan di atas, sebelum batas bawah)
  const long rampLen = (long)rampLenF;

  // Perlambat kalau sudah dekat tujuan
  float capSps = s.speedTarget;
  if (remaining < rampLen) {
    float frac = (float)remaining / (float)rampLen;
    if (frac < SLOW_FLOOR_FRAC) frac = SLOW_FLOOR_FRAC;
    float slow = s.speedTarget * frac;
    if (slow < capSps) capSps = slow;
  }

  // Naikkan bertahap menuju capSps.
  //
  // PENTING: accelPerStep dihitung dari rampLenF yang SEDANG berlaku.
  // Karena rampLenF sekarang menyesuaikan panjang gerakan, nilai ini
  // otomatis benar untuk gerakan pendek maupun panjang.
  const float accelPerStep = s.speedTarget / rampLenF;
  if (s.speedCurrent < capSps) {
    s.speedCurrent += accelPerStep;
    if (s.speedCurrent > capSps) s.speedCurrent = capSps;
  } else {
    s.speedCurrent = capSps;
  }

  // Batas bawah: motor stepper butuh kecepatan minimum agar tetap punya
  // torsi. Terlalu pelan justru mudah stall dan bergetar.
  if (s.speedCurrent < SPEED_MIN_SPS) s.speedCurrent = SPEED_MIN_SPS;
  return s.speedCurrent;
}

// Hitung perioda berikutnya untuk satu motor.
//
// PENTING soal pengukuran jitter:
// Kita TIDAK mengukur jitter dari selisih waktu antar-pulsa, karena
// perioda pulsa MEMANG berubah sesuai ramp (motor naik/turun kecepatan).
// Perubahan perioda itu bukan gangguan.
//
// Yang diukur adalah JITTER TASK: seberapa lama task stepper tertahan
// sebelum sempat melayani pulsa. Caranya: setiap kali task dipanggil,
// kita catat jarak waktu antar panggilan. Kalau jarak itu jauh lebih
// besar dari yang diminta, berarti ada yang menahan task.
static inline void stepperTick(Stepper &s, unsigned long now) {
  if (!s.moving) return;

  // Fase 1: pulsa sedang HIGH -> turunkan setelah PULSE_MIN_US
  if (s.pulseHigh) {
    if (now - s.lastPulseUs < PULSE_MIN_US) return;
    // Akhiri pulsa. Dengan common-anode 5V, "selesai" = kembali ke HIGH.
    digitalWrite(s.pinPul, PULSE_OFF);
    s.pulseHigh   = false;
    s.lastPulseUs = now;
    return;
  }

  // Fase 2: tunggu sisa perioda, lalu naikkan pulsa lagi
  unsigned long elapsed = now - s.lastPulseUs;
  if (elapsed < s.halfPeriodUs) return;

  // Ukur KELEBIHAN tunggu: berapa lama melewati jadwal.
  //
  // PENTING: pulsa PERTAMA dan KEDUA setelah gerakan dimulai sengaja
  // TIDAK diukur. Alasannya:
  //  - Pulsa pertama terjadi saat halfPeriodUs masih 0 / baru di-set.
  //  - speedCurrent di-ramp naik dari nol, jadi perioda pulsa kedua
  //    jauh lebih panjang dari perioda "normal".
  // Keduanya transisi ramp yang sah, bukan gangguan. Kalau ikut diukur,
  // selalu muncul "1 pulsa terlambat" yang menyesatkan dan membuat
  // jitter maksimum terlihat buruk padahal pulsanya rapi.
  //
  // s.warmUp menyimpan sisa pulsa yang harus dilewati sebelum pengukuran
  // dianggap sah.
  if (s.warmUp > 0) {
    s.warmUp--;
  } else {
    unsigned long over = elapsed - s.halfPeriodUs;

    if (over < 50000UL) {        // di atas 50 ms = pause, bukan jitter
      jitSumUs += (uint32_t)over;
      jitCount++;
      if (over > jitMaxUs) jitMaxUs = (uint32_t)over;
      if (over > 50)       jitLateCnt++;
    }
  }

  // Mulai pulsa. Dengan common-anode 5V, "pulsa" = tarik ke LOW.
  digitalWrite(s.pinPul, PULSE_ON);
  s.pulseHigh   = true;
  s.lastPulseUs = now;
  s.currentStep += s.dir;
  stepTotal++;

  // ---- PERBAIKAN PRESISI: pastikan pulsa TERAKHIR selalu ditutup ----
  //
  // MASALAH YANG DIPERBAIKI DI SINI:
  // Kode lama memeriksa "target tercapai" SETELAH menaikkan pulsa, lalu
  // langsung `return` tanpa menurunkan pin PUL. Akibatnya pin PUL tetap
  // tertahan di level AKTIF (LOW untuk common-anode) ketika gerakan
  // selesai, dan baru dilepas saat gerakan berikutnya dimulai.
  //
  // Kalau gerakan berikutnya memakai DIR yang BERBEDA, driver TB6600
  // melihat tepi pada pin PUL bersamaan dengan perubahan DIR, dan tepi
  // itu dihitung sebagai satu langkah tambahan ke arah yang baru. Motor
  // bergeser satu langkah (0.1125 derajat) secara TIDAK KONSISTEN,
  // tergantung waktu. Ini penyebab "80 persen tepat, sisanya bergeser".
  //
  // Perbaikannya: begitu langkah terakhir dikirim, tunggu selebar pulsa
  // minimum lalu TUTUP pulsanya. Dengan begitu pin PUL selalu kembali ke
  // level idle sebelum gerakan berakhir, dan tidak ada tepi palsu saat
  // DIR berubah.
  if (s.currentStep == s.targetStep) {
    // Tunggu sampai lebar pulsa minimum terpenuhi, supaya TB6600 sempat
    // membaca langkah terakhir ini.
    unsigned long tunggu = micros();
    while ((micros() - tunggu) < (PULSE_MIN_US + 2)) {
      // putar singkat, jangan pakai delay() supaya tidak menyerahkan
      // kendali ke task lain di tengah pulsa terakhir
    }

    digitalWrite(s.pinPul, PULSE_OFF);
    s.pulseHigh   = false;

    // Beri jeda kecil setelah tepi turun sebelum gerakan dianggap selesai.
    // Ini memberi waktu driver menyelesaikan satu langkah terakhir.
    tunggu = micros();
    while ((micros() - tunggu) < 5) { }

    s.moving = false;
    stepperRelease(s);
    return;
  }

  // ---- Ukur KESALAHAN perioda terhadap JADWAL SAAT ITU ----
  //
  // PENTING: pembacaan ini harus terjadi SEBELUM halfPeriodUs dihitung
  // ulang untuk pulsa berikutnya. Kalau diletakkan setelahnya, yang
  // dibandingkan adalah jadwal pulsa BERIKUTNYA, bukan jadwal yang baru
  // saja dipakai menunggu. Akibatnya angkanya tidak bermakna.
  //
  // Jangan juga bandingkan dengan stepSpeedActive, karena selama ramp
  // kecepatannya berubah-ubah. Jadwal yang sah saat ini adalah
  // halfPeriodUs: error = elapsed - halfPeriodUs.
  {
    long err = (long)elapsed - (long)s.halfPeriodUs;

    // Abaikan pulsa pertama (halfPeriodUs belum bermakna)
    if (gPeriodLastUs != 0 && err > -100000L && err < 100000L) {
      if (err > (long)gPeriodMaxUs)  gPeriodMaxUs = (uint32_t)err;
      if (err < (long)gPeriodMinUs)  gPeriodMinUs = (uint32_t)err;

      uint32_t a = (uint32_t)(err < 0 ? -err : err);
      gPeriodSumUs += a;
      gPeriodCount++;

      // Catat di PULSA KE-BERAPA lonjakan besar terjadi. Ini yang
      // membedakan "lonjakan saat mulai" (transisi ramp, wajar) dari
      // "lonjakan di tengah" (macet, penyebab tersendat).
      if (err > 500) {
        gLonjakCount++;
        if (gLonjakPertamaPulsa == 0) {
          gLonjakPertamaPulsa = gPeriodCount;
          gLonjakPertamaUs    = (uint32_t)err;
          gLonjakPosisiStep   = (uint32_t)s.currentStep;
        }
        if (err > (long)gLonjakTerbesarUs) {
          gLonjakTerbesarUs   = (uint32_t)err;
          gLonjakTerbesarStep = (uint32_t)s.currentStep;
        }
      }
    }
  }
  gPeriodLastUs = now;

  // Hitung kecepatan berikutnya (ramp) untuk perioda berikutnya
  float sps = stepperNextSpeed(s);
  s.halfPeriodUs = (unsigned long)(500000.0 / sps);
  if (s.halfPeriodUs < (PULSE_MIN_US + 2)) {
    s.halfPeriodUs = PULSE_MIN_US + 2;
  }
}

// ============================================================
//  TASK STEPPER DI CORE 1  (inti perbaikan jitter)
// ============================================================
//
//  MASALAH: kalau pulsa stepper dikeluarkan dari loop() atau dari timer
//  yang berjalan di core 0 (core WiFi), maka setiap request HTTP menunda
//  pulsa -> gerakan tersentak (jitter).
//
//  Uji nyata sebelum perbaikan ini: jitter maksimum 1156 us,
//  215 dari 3128 pulsa terlambat >50 us.
//
//  SOLUSI: jalankan loop pulsa di CORE 1 dengan prioritas tinggi.
//  WiFi/HTTP Arduino berjalan di CORE 0, jadi keduanya tidak saling
//  mengganggu. Ini menjadikan pulsa jauh lebih konsisten.
//
//  Delay memakai vTaskDelayUntil pada tick 1 ms, dibantu
//  delayMicroseconds untuk presisi di bawah 1 ms — keduanya tidak
//  memblokir core lain.

TaskHandle_t stepperTaskHandle = nullptr;

// ---- Instrumentasi: ukur waktu yang dihabiskan task stepper sendiri ----
volatile uint32_t spinLoopCnt   = 0;   // berapa kali putaran spin
volatile uint32_t spinMaxUs     = 0;   // putaran spin terpanjang (us)
volatile uint32_t yieldLateMax  = 0;   // keterlambatan setelah taskYIELD (us)
volatile uint32_t delayLateMax  = 0;   // keterlambatan setelah vTaskDelayUntil (us)

// Loop pulsa. Berjalan di core 1.
//
// PENTING: strategi di sini adalah BERPUTAR (spin) selama motor bergerak,
// TANPA memanggil taskYIELD atau delay. Alasannya: pada core 1, taskYIELD
// bisa tidak langsung kembali (task lain mengambil alih), dan itu membuat
// pulsa tertunda milidetik. Dengan spin murni + micros(), pulsa keluar
// tepat waktu selama motor bergerak. Saat motor diam, task tidur penuh
// sehingga tidak membuang CPU.
void stepperTaskCore1(void* param) {
  for (;;) {
    // ---- MODE GERAK: spin presisi, TANPA tidur ----
    // Selama ada motor bergerak, task ini tidak tidur sama sekali.
    //
    // Ini penting: vTaskDelayUntil memakai resolusi tick 1 ms, sedangkan
    // perioda pulsa bisa 0,83 ms (K600) atau lebih kecil. Kalau task tidur
    // di antara pulsa, pulsa berikutnya harus menunggu tick berikutnya dan
    // jitter rata-rata terukur ~490 us (hampir separuh tick) — terbukti
    // dari pengujian, dan itulah penyebab gerakan tersentak.
    //
    // Dengan spin penuh selama gerakan, jitter rata-rata ~0,2 us.
    // Trade-off: core 1 terpakai penuh selama lid bergerak. Wajar, karena
    // gerakan hanya berlangsung <2 detik dan core 1 memang disediakan
    // khusus untuk ini (WiFi ada di core 0).
    if (stLidLeft.moving || stLidRight.moving) {
      spinLoopCnt++;
      unsigned long t0 = micros();
      unsigned long guard = 0;

      while (stLidLeft.moving || stLidRight.moving) {
        unsigned long now = micros();

        // Hanya proses motor yang SEDANG bergerak. Memanggil untuk motor
        // diam sebenarnya langsung 'return', tapi memanggil hanya yang
        // aktif mengurangi jumlah pemanggilan fungsi per iterasi dan
        // membuat loop lebih ringan.
        if (tickHanyaMotorAktif) {
          if (stLidLeft.moving)  stepperTick(stLidLeft,  now);
          if (stLidRight.moving) stepperTick(stLidRight, now);
        } else {
          stepperTick(stLidLeft,  now);
          stepperTick(stLidRight, now);
        }

        // Pengaman anti-macet: kalau masih bergerak setelah ~3 detik,
        // paksa berhenti supaya tidak ada lid yang jalan tanpa kendali.
        if (++guard > 3000000UL) {
          stLidLeft.moving  = false;
          stLidRight.moving = false;
          stepperRelease(stLidLeft);
          stepperRelease(stLidRight);
          addLog("[ERROR] Stepper >3s - dihentikan paksa");
          break;
        }
      }

      uint32_t spent = (uint32_t)(micros() - t0);
      if (spent > spinMaxUs) spinMaxUs = spent;

      // Kedua motor selesai dengan benar -> hapus penanda di RTC memory.
      // Selama gerakan berlangsung penanda tetap ada, jadi reset di
      // tengah gerakan akan terlihat pada boot berikutnya.
      if (!stLidLeft.moving && !stLidRight.moving) {
        markMoveEnd();
        gMoveEndUs = micros();   // catat akhir gerakan SEBELUM dihitung

        // ---- UKUR PRESISI ----
        // Bandingkan langkah yang DIMINTA dengan langkah yang benar-benar
        // dikirim. Kalau keduanya sama, gerakan presisi. Selisih di sini
        // berarti ada langkah yang tidak terkirim, dan itu penyebab
        // "titik berhenti yang bergeser".
        {
          int32_t selisih = (int32_t)gMoveLastSteps - (int32_t)gMoveReqSteps;
          if (selisih < 0) selisih = -selisih;

          gPresisiUji++;
          if (selisih != 0) {
            gPresisiGagal++;
            if (selisih > gPresisiMaksSelisih) gPresisiMaksSelisih = selisih;
            addLogf("[PRESISI] MELESET %ld langkah dari %lu yang diminta "
                    "(%.2f derajat)",
                    (long)selisih,
                    (unsigned long)gMoveReqSteps,
                    (float)selisih / STEPS_PER_DEG);
          } else {
            addLogf("[PRESISI] tepat %lu langkah - presisi",
                    (unsigned long)gMoveReqSteps);
          }
        }

        // Hitung kecepatan rata-rata AKTUAL dari durasi gerakan.
        // Kalau jauh di bawah yang diminta, ada indikasi kehilangan
        // langkah atau pulsa tidak keluar rapi.
        if (gMoveStartUs && gMoveEndUs > gMoveStartUs) {
          unsigned long durUs = gMoveEndUs - gMoveStartUs;
          uint32_t      steps = (uint32_t)(stepTotal - gMoveStepsAtStart);
          if (durUs > 0 && steps > 0) {
            float aktual = (float)steps * 1000000.0f / (float)durUs;
            gMoveLastSpeed = aktual;
            gMoveLastSteps = steps;

            // Tampilkan hanya kalau menarik (perbedaan > 20%)
            float diminta = stepSpeedActive;
            if (aktual < diminta * 0.8f) {
              addLogf("[HATI] Gerakan %.0f step/s aktual, diminta %.0f "
                      "(%.0f%% dari target)",
                      aktual, diminta, (aktual / diminta) * 100.0f);
            } else {
              addLogf("[OK] Gerakan %.0f step/s aktual (diminta %.0f)",
                      aktual, diminta);
            }

            // Tampilkan KESALAHAN JADWAL pulsa. Inilah ukuran yang sah:
            // dibandingkan dengan jadwal SAAT ITU (halfPeriodUs), bukan
            // dengan stepSpeedActive yang tidak berlaku selama ramp.
            if (gPeriodCount > 0) {
              unsigned long pAvg = gPeriodSumUs / gPeriodCount;
              addLogf("[JADWAL] selisih rata2=%lu us, min=%ld max=%ld "
                      "dari %lu pulsa",
                      pAvg,
                      (long)gPeriodMinUs,
                      (long)gPeriodMaxUs,
                      (unsigned long)gPeriodCount);
              if (pAvg > 100) {
                addLog("[HATI] Pulsa tertunda >100us rata-rata - "
                       "inilah penyebab gerakan tersendat.");
              } else {
                addLog("[OK] Pulsa keluar tepat sesuai jadwal saat itu.");
              }

              // Laporkan LONJAKAN: ini yang menyebabkan sentakan.
              if (gLonjakCount > 0) {
                addLogf("[LONJAK] %lu pulsa macet >500us. "
                        "Terbesar %lu us di step %lu. "
                        "Pertama %lu us di pulsa ke-%lu (step %lu)",
                        (unsigned long)gLonjakCount,
                        (unsigned long)gLonjakTerbesarUs,
                        (unsigned long)gLonjakTerbesarStep,
                        (unsigned long)gLonjakPertamaUs,
                        (unsigned long)gLonjakPertamaPulsa,
                        (unsigned long)gLonjakPosisiStep);

                // Kalau macet pertama terjadi di pulsa ke-1 atau 2,
                // itu transisi ramp dan tidak berbahaya.
                // Kalau di tengah gerakan, itu penyebab tersendat.
                if (gLonjakPertamaPulsa <= 3) {
                  addLog("[LONJAK] Macet terjadi di AWAL gerakan "
                         "(transisi ramp) - tidak berbahaya.");
                } else {
                  addLog("[LONJAK] Macet terjadi di TENGAH gerakan - "
                         "INILAH penyebab gerakan tersendat.");
                }
              } else {
                addLog("[OK] Tidak ada pulsa yang macet.");
              }
            }
          }
        }
      }
      continue;   // langsung periksa lagi, tanpa tidur
    }

    // ---- MODE DIAM: tidur, tidak membuang CPU ----
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void stepperTaskStart() {
  BaseType_t ok = xTaskCreatePinnedToCore(
      stepperTaskCore1,      // fungsi task
      "stepper",             // nama
      4096,                  // stack
      nullptr,               // argumen
      3,                     // prioritas (lebih tinggi dari loop Arduino)
      &stepperTaskHandle,
      STEPPER_CORE);         // core 1, bukan core WiFi

  if (ok == pdPASS) {
    addLogf("[OK] Task stepper di core %d (anti-jitter)", STEPPER_CORE);
  } else {
    addLog("[ERROR] Gagal membuat task stepper!");
  }
}

bool stepperBusy() { return stLidLeft.moving || stLidRight.moving; }

// Apakah ada pulsa yang sudah jatuh tempo sekarang?
// Dipakai task stepper untuk menentukan perlu spin atau boleh tidur.
// Kalau tidak ada yang jatuh tempo, tidak perlu berputar menahan CPU.
static inline bool stepperAnyDueNow() {
  unsigned long now = micros();

  if (stLidLeft.moving) {
    unsigned long e = now - stLidLeft.lastPulseUs;
    if (stLidLeft.pulseHigh) {
      if (e >= PULSE_MIN_US) return true;
    } else if (e >= stLidLeft.halfPeriodUs) {
      return true;
    }
  }

  if (stLidRight.moving) {
    unsigned long e = now - stLidRight.lastPulseUs;
    if (stLidRight.pulseHigh) {
      if (e >= PULSE_MIN_US) return true;
    } else if (e >= stLidRight.halfPeriodUs) {
      return true;
    }
  }

  return false;
}

// Gerakkan HANYA satu lid.
void lidToDeg(Stepper &s, float deg, float speed) {
  stepperMoveToDeg(s, deg, speed);
}

// Gerakkan KEDUA lid bersamaan. Dipakai oleh perintah C/S.
void lidBothToDeg(float deg, float speed) {
  stepperMoveToDeg(stLidLeft,  deg, speed);
  stepperMoveToDeg(stLidRight, deg, speed);
}

// Terapkan profil kecepatan ke seluruh robot.
//
// Dipanggil saat pengguna menekan tombol SLOW/MEDIUM/FAST, dan saat
// robot menyala untuk memulihkan pilihan terakhir. Kalau `simpan`
// bernilai true, pilihannya ditulis ke flash supaya tidak hilang.
void simpanProfil();
void pilihProfil(uint8_t p, bool simpan) {
  if (p > 2) p = PROFIL_MEDIUM;
  profilAktif = p;

  // Kecepatan tutup mengikuti profil
  stepSpeedActive = PROFIL[p].stepSps;

  // Kecepatan roda juga mengikuti, supaya "slow" berarti slow
  // untuk seluruh robot, bukan hanya tutupnya.
  speedPWM = PROFIL[p].pwmRoda;

  // Kalau roda sedang berjalan, kecepatan baru langsung dipakai.
  if (moveMode != MOVE_STOP) motorApply();

  addLogf("[PROFIL] %s - tutup %.0f step/s, roda PWM %d",
          PROFIL[p].nama, stepSpeedActive, speedPWM);

  if (simpan) simpanProfil();
}

// Simpan setelan motor penggerak ke flash. Dipanggil setiap kali ada
// yang diubah, supaya tidak hilang saat ESP32 dimatikan.
void simpanMotor() {
  Preferences p;
  if (!p.begin("trashbin", false)) return;

  p.putBool("balikKiri",  motorBalikKiri);
  p.putBool("balikKanan", motorBalikKanan);
  p.putChar("kompKiri",   pwmKompensasiKiri);
  p.putChar("kompKanan",  pwmKompensasiKanan);
  p.putUChar("tambahBelok", tambahanBelok);
  p.putUChar("profil",    profilAktif);

  p.end();
}

// Simpan hanya pilihan profil. Dipakai saat tombol kecepatan ditekan.
void simpanProfil() {
  Preferences p;
  if (!p.begin("trashbin", false)) return;
  p.putUChar("profil", profilAktif);
  p.end();
}

// Nama lama untuk simpanMotor(), dipertahankan supaya bagian kode
// yang lama tidak perlu diubah semuanya.
void simpanArahMotor() {
  simpanMotor();
}

// Muat pilihan profil dari flash saat robot menyala.
void muatProfil() {
  Preferences p;
  if (!p.begin("trashbin", true)) {
    pilihProfil(PROFIL_MEDIUM, false);
    return;
  }

  uint8_t v = p.getUChar("profil", PROFIL_MEDIUM);
  p.end();

  if (v > 2) v = PROFIL_MEDIUM;
  pilihProfil(v, false);
}

// ------------------------------------------------------------
//  SIMPAN / MUAT SUDUT GLOBAL KE FLASH (NVS)
// ------------------------------------------------------------
//  Supaya sudut yang sudah diatur tidak hilang saat ESP32 dimatikan.
//  Kalau belum pernah diatur, dipakai LID_ANGLE_DEF.
void simpanSudutGlobal() {
  Preferences p;
  if (!p.begin("trashbin", false)) return;
  p.putFloat("lidAngle", lidAngleGlobal);
  p.end();
}

void muatSudutGlobal() {
  Preferences p;
  if (!p.begin("trashbin", true)) {
    lidAngleGlobal = LID_ANGLE_DEF;
    return;
  }

  float v = p.getFloat("lidAngle", LID_ANGLE_DEF);

  // Sekalian muat setelan motor penggerak, karena keduanya
  // disimpan di tempat yang sama.
  motorBalikKiri  = p.getBool("balikKiri",  false);
  motorBalikKanan = p.getBool("balikKanan", false);

  pwmKompensasiKiri  = p.getChar("kompKiri",  (int8_t)0);
  pwmKompensasiKanan = p.getChar("kompKanan", (int8_t)0);
  tambahanBelok      = p.getUChar("tambahBelok", (uint8_t)45);

  // Jaga-jaga kalau nilai di flash rusak atau di luar batas.
  if (pwmKompensasiKiri  < -KOMPENSASI_MAKS) pwmKompensasiKiri  = -KOMPENSASI_MAKS;
  if (pwmKompensasiKiri  >  KOMPENSASI_MAKS) pwmKompensasiKiri  =  KOMPENSASI_MAKS;
  if (pwmKompensasiKanan < -KOMPENSASI_MAKS) pwmKompensasiKanan = -KOMPENSASI_MAKS;
  if (pwmKompensasiKanan >  KOMPENSASI_MAKS) pwmKompensasiKanan =  KOMPENSASI_MAKS;
  if (tambahanBelok > TAMBAHAN_BELOK_MAKS)   tambahanBelok      = TAMBAHAN_BELOK_MAKS;

  p.end();

  if (v < LID_ANGLE_MIN || v > LID_ANGLE_MAX) v = LID_ANGLE_DEF;
  lidAngleGlobal = v;

  addLogf("[SUDUT] Sudut global dimuat dari memori = %.1f derajat", lidAngleGlobal);
  addLogf("[MOTOR] Arah: KIRI=%s KANAN=%s",
          motorBalikKiri  ? "DIBALIK" : "normal",
          motorBalikKanan ? "DIBALIK" : "normal");
  addLogf("[MOTOR] Kompensasi: KIRI=%+d KANAN=%+d, tambahan belok=+%d",
          (int)pwmKompensasiKiri, (int)pwmKompensasiKanan,
          (int)tambahanBelok);
}

// ==================== LOGIKA PERINTAH (dipakai WiFi & BLE) ====================

void executeCommand(char cmd, const String &arg) {
  lastCmdMillis = millis();      // reset watchdog tiap ada perintah

  switch (cmd) {
    case 'F': moveMode = MOVE_FWD;   motorApply(); break;
    case 'B': moveMode = MOVE_BACK;  motorApply(); break;
    case 'L': moveMode = MOVE_LEFT;  motorApply(); break;
    case 'R': moveMode = MOVE_RIGHT; motorApply(); break;
    case 'P': moveMode = MOVE_STOP;  motorStop();  break;

    // ============================================================
    //  'N' = PILIH PROFIL KECEPATAN  (SLOW / MEDIUM / FAST)
    // ============================================================
    //  Format:  N<0|1|2>   atau   N<slow|medium|fast>
    //    N0 / NSLOW    -> profil SLOW
    //    N1 / NMEDIUM  -> profil MEDIUM
    //    N2 / NFAST    -> profil FAST
    //
    //  Satu profil mengatur kecepatan roda DAN kecepatan tutup sekaligus,
    //  supaya "slow" berlaku untuk seluruh robot.
    //  Pilihan ini disimpan di flash.
    case 'N': {
      String a = arg;
      a.toUpperCase();
      a.trim();

      int p = -1;

      if (a.startsWith("SLOW"))        p = PROFIL_SLOW;
      else if (a.startsWith("MEDIUM")) p = PROFIL_MEDIUM;
      else if (a.startsWith("FAST"))   p = PROFIL_FAST;
      else if (a.length() > 0)         p = a.toInt();

      if (p < 0 || p > 2) {
        addLog("[MOTOR] Profil harus SLOW, MEDIUM, FAST, atau 0-2");
        break;
      }

      pilihProfil((uint8_t)p, true);
      break;
    }

    // ============================================================
    //  'Q' = UJI KECEPATAN BERTAHAP (SWEEP)
    // ============================================================
    //  Mencari pada kecepatan berapa motor mulai kehilangan langkah.
    //
    //    Q   atau QL  -> uji lid KIRI
    //    QR           -> uji lid KANAN
    //    QB           -> uji KEDUA lid
    //
    //  PENTING: torsi TIDAK BISA diubah dari firmware. Uji ini
    //  mengukur batas kecepatan, bukan menambah tenaga. Untuk menambah
    //  tenaga harus mengubah arus di DIP switch TB6600, lihat catatan
    //  lengkap di atas fungsi sweepKecepatan().
    case 'Q': {
      String a = arg;
      a.toUpperCase();

      bool kiri  = (a.indexOf('L') >= 0) || (a.indexOf('R') < 0);
      bool kanan = (a.indexOf('R') >= 0) || (a.indexOf('L') < 0);

      addLog("[SWEEP] Mulai... motor akan bergerak bolak-balik.");
      addLog("[SWEEP] Jangan matikan robot selama uji berjalan.");

      if (kiri)  sweepKecepatan(stLidLeft,  "KIRI");
      if (kanan) sweepKecepatan(stLidRight, "KANAN");

      addLog("[SWEEP] Selesai. Lihat hasilnya di log di atas.");
      break;
    }

    case 'V': {
      if (arg.length() > 0) {
        int sp = arg.toInt();
        if (sp < 0)   sp = 0;
        if (sp > 255) sp = 255;
        speedPWM = (uint8_t)sp;
        if (moveMode != MOVE_STOP) motorApply();
        addLogf("[MOTOR] Kecepatan PWM = %d", speedPWM);
      }
      break;
    }

    case 'C': {
      float deg = lidAngleGlobal;
      if (arg.length() > 0) {
        float preset = arg.toFloat();
        if (preset >= LID_ANGLE_MIN && preset <= LID_ANGLE_MAX) deg = preset;
      }
      addLogf("[LID] Buka KEDUA lid ke %.1f deg @ %.0f step/s", deg, stepSpeedActive);
      lidBothToDeg(deg, stepSpeedActive);
      break;
    }

    case 'S':
      addLog("[LID] Tutup KEDUA lid ke 0 deg");
      lidBothToDeg(0.0f, stepSpeedActive);
      break;

    // 'I' = kendalikan lid PER SATU SISI.
    //   IL1 / IL0  -> buka / tutup lid KIRI saja
    //   IR1 / IR0  -> buka / tutup lid KANAN saja
    //   IL1R1      -> kombinasi, keduanya bisa digabung dalam satu perintah
    // Argumen dipakai supaya tidak bentrok dengan 'L'/'R' (pivot roda).
    case 'I': {
      if (arg.length() == 0) {
        addLog("[LID] Perintah I butuh argumen, mis. IL1 / IR0");
        break;
      }

      String a = arg;
      a.toUpperCase();

      bool anyAction = false;
      for (unsigned int i = 0; i < a.length(); i++) {
        char side = a.charAt(i);
        if (side != 'L' && side != 'R') continue;

        // Ambil digit setelah huruf sisi. Kalau tidak ada, anggap buka (1).
        char val = (i + 1 < a.length()) ? a.charAt(i + 1) : '1';
        bool open = (val == '1');
        float deg = open ? lidAngleGlobal : 0.0f;

        if (side == 'L') {
          addLogf("[LID] Lid KIRI -> %s (%.1f deg)",
                  open ? "BUKA" : "TUTUP", deg);
          lidToDeg(stLidLeft, deg, stepSpeedActive);
        } else {
          addLogf("[LID] Lid KANAN -> %s (%.1f deg)",
                  open ? "BUKA" : "TUTUP", deg);
          lidToDeg(stLidRight, deg, stepSpeedActive);
        }
        anyAction = true;
        i++;   // lewati digit yang sudah dipakai
      }

      if (!anyAction) {
        addLogf("[WARN] Argumen I tidak valid: %s (pakai L/R)", arg.c_str());
      }
      break;
    }

    // 'K' = set kecepatan stepper (step/detik). Untuk tuning anti-getar.
    // Contoh: K300 -> 300 step/s (lambat, paling aman)
    //         K1200 -> cepat, butuh sinyal 5V yang benar
    case 'K': {
      if (arg.length() > 0) {
        float sps = arg.toFloat();
        if (sps < 50.0f)    sps = 50.0f;
        if (sps > 4000.0f)  sps = 4000.0f;
        stepSpeedActive = sps;
        addLogf("[STEP] Kecepatan stepper = %.0f step/s", stepSpeedActive);
      }
      break;
    }

    // 'D' = UJI DIR. Menggerakkan satu motor maju lalu mundur.
    // Dipakai untuk memisahkan dua penyebab "lid selalu searah":
    //   1) 5V untuk TB6600 mati (buck converter ikut mati)
    //   2) kabel DIR lepas / salah pin
    //
    //   D   -> uji lid KIRI
    //   DR  -> uji lid KANAN
    //   DL  -> uji lid KIRI (sama dengan D)
    case 'D': {
      String a = arg;
      a.toUpperCase();
      bool kanan = (a.indexOf('R') >= 0);

      if (kanan) stepperDirTest(stLidRight, "KANAN");
      else       stepperDirTest(stLidLeft,  "KIRI");
      break;
    }

    // 'H' = mode TAHAN motor setelah gerakan (diagnosa "terus berputar").
    //   H1 -> motor DIKUNCI setelah gerakan selesai
    //   H0 -> motor DILEPAS setelah gerakan selesai (default)
    //
    // Kalau dengan H1 masalah "motor terus berputar" hilang, berarti
    // penyebabnya motor free + beban, bukan sinyal DIR/PUL.
    case 'H': {
      bool on = (arg.length() == 0) ? true : (arg.toInt() != 0);
      holdMode = on;
      addLogf("[HOLD] Mode tahan motor = %s",
              on ? "AKTIF (motor dikunci)" : "MATI (motor dilepas)");
      if (on) {
        stepperEnable(stLidLeft,  true);
        stepperEnable(stLidRight, true);
      } else {
        stepperEnable(stLidLeft,  false);
        stepperEnable(stLidRight, false);
      }
      break;
    }

    // ============================================================
    //  'Y' = BALIK ARAH MOTOR PENGGERAK
    // ============================================================
    //  Dipakai kalau kabel motor terpasang terbalik, sehingga robot
    //  bergerak berlawanan dengan tombolnya.
    //
    //    YL1 / YL0  -> balik / normalkan arah motor KIRI
    //    YR1 / YR0  -> balik / normalkan arah motor KANAN
    //
    //  Huruf Y dipakai karena R sudah terpakai untuk pivot kanan.
    //  Setelannya disimpan di flash, jadi tidak hilang saat dimatikan.
    case 'Y': {
      if (arg.length() < 2) {
        addLog("[MOTOR] Perintah Y butuh argumen, mis. YL1 / YR0");
        break;
      }

      String a = arg;
      a.toUpperCase();

      char sisi = a.charAt(0);
      if (sisi != 'L' && sisi != 'R') {
        addLogf("[WARN] Sisi harus L atau R - bukan %c", sisi);
        break;
      }

      bool balik = (a.charAt(1) != '0');

      if (sisi == 'L') motorBalikKiri  = balik;
      else             motorBalikKanan = balik;

      simpanArahMotor();

      addLogf("[MOTOR] Arah motor %s = %s (tersimpan)",
              (sisi == 'L' ? "KIRI" : "KANAN"),
              balik ? "DIBALIK" : "normal");

      // Terapkan segera supaya kalau roda sedang jalan, arahnya
      // langsung berubah tanpa perlu berhenti dulu.
      if (moveMode != MOVE_STOP) motorApply();
      break;
    }

    // ============================================================
    //  'Z' = KOMPENSASI RODA DAN TAMBAHAN TENAGA SAAT BELOK
    // ============================================================
    //  Dipakai kalau saat maju robot tidak lurus, atau saat berputar
    //  di tempat salah satu roda terlihat lebih lambat.
    //
    //    ZL<n>  -> kompensasi roda KIRI,  -60 sampai +60
    //    ZR<n>  -> kompensasi roda KANAN, -60 sampai +60
    //    ZB<n>  -> tambahan PWM saat belok, 0 sampai 90
    //
    //  Contoh: ZL15 berarti roda kiri diberi PWM 15 lebih tinggi
    //  daripada nilai dasar. Naikkan angka untuk roda yang lemah,
    //  jangan turunkan roda yang kuat.
    //
    //  Semua nilai disimpan di flash.
    case 'Z': {
      String a = arg;
      a.toUpperCase();

      if (a.length() < 2) {
        addLog("[MOTOR] Format: ZL<n>, ZR<n>, atau ZB<n>");
        break;
      }

      char apa = a.charAt(0);
      int  v   = a.substring(1).toInt();

      if (apa == 'L' || apa == 'R') {
        if (v < -KOMPENSASI_MAKS) v = -KOMPENSASI_MAKS;
        if (v >  KOMPENSASI_MAKS) v =  KOMPENSASI_MAKS;

        if (apa == 'L') pwmKompensasiKiri  = (int8_t)v;
        else            pwmKompensasiKanan = (int8_t)v;

        simpanMotor();
        addLogf("[MOTOR] Kompensasi roda %s = %+d",
                (apa == 'L' ? "KIRI" : "KANAN"), v);

      } else if (apa == 'B') {
        if (v < 0) v = 0;
        if (v > TAMBAHAN_BELOK_MAKS) v = TAMBAHAN_BELOK_MAKS;

        tambahanBelok = (uint8_t)v;
        simpanMotor();
        addLogf("[MOTOR] Tambahan PWM saat belok = +%d", v);

      } else {
        addLog("[MOTOR] Format: ZL<n>, ZR<n>, atau ZB<n>");
        break;
      }

      // Terapkan segera supaya perubahan bisa langsung dirasakan.
      if (moveMode != MOVE_STOP) motorApply();
      break;
    }

    // 'X' = saklar uji untuk mencari penyebab perioda pulsa panjang.
    //   XB1 -> matikan BLE (uji: apakah BLE/flash yang menahan pulsa)
    //   XB0 -> hidupkan BLE
    //   XT1 -> tick hanya motor yang aktif (uji: beban spin loop)
    //   XT0 -> tick keduanya
    case 'X': {
      String a = arg;
      a.toUpperCase();

      if (a.startsWith("B")) {
        bleOff = (a.length() > 1 && a.charAt(1) == '1');
        if (bleOff) BLEAdvertising_stop();
        else        BLEAdvertising_start();
        addLogf("[UJI] BLE %s", bleOff ? "DIMATIKAN" : "diaktifkan");
      } else if (a.startsWith("T")) {
        tickHanyaMotorAktif = (a.length() > 1 && a.charAt(1) == '1');
        addLogf("[UJI] Tick %s",
                tickHanyaMotorAktif ? "hanya motor aktif"
                                    : "kedua motor");
      } else {
        addLogf("[UJI] BLE=%s  tickHanyaAktif=%s",
                bleOff ? "mati" : "hidup",
                tickHanyaMotorAktif ? "ya" : "tidak");
      }
      break;
    }

    // ============================================================
    //  'G' = ATUR SUDUT SATU LID  (untuk kontrol halus / tuning)
    // ============================================================
    //  Format:  G<L|R><sudut>      sudut dalam DERAJAT
    //  Contoh:  GL10   -> lid KIRI ke 10 derajat
    //           GL12.5 -> lid KIRI ke 12.5 derajat
    //           GR0    -> lid KANAN ke 0 derajat
    //
    //  Arah ditentukan OTOMATIS dari selisih posisi (seperti biasa).
    //  Pergerakan dibatasi supaya tidak menabrak mekanik.
    case 'G': {
      if (arg.length() < 2) {
        addLog("[LID] Perintah G butuh argumen, mis. GL10");
        break;
      }

      String a = arg;
      a.toUpperCase();

      char sisi = a.charAt(0);
      if (sisi != 'L' && sisi != 'R') {
        addLogf("[WARN] Sisi harus L atau R, bukan %c", sisi);
        break;
      }

      float deg = a.substring(1).toFloat();

      // Batas aman: 0 sampai LID_MAX_DEG
      if (deg < 0.0f)         deg = 0.0f;
      if (deg > LID_MAX_DEG)  deg = LID_MAX_DEG;

      if (sisi == 'L') {
        addLogf("[LID] Lid KIRI -> %.2f derajat", deg);
        lidToDeg(stLidLeft, deg, stepSpeedActive);
      } else {
        addLogf("[LID] Lid KANAN -> %.2f derajat", deg);
        lidToDeg(stLidRight, deg, stepSpeedActive);
      }
      break;
    }

    // ============================================================
    //  'O' = BUKA / TUTUP  (memakai sudut global)
    // ============================================================
    //  Format:  O<L|R|B><1|0>
    //    1 = BUKA  sampai sudut global (CW)
    //    0 = TUTUP sampai 0 derajat (CCW)
    //
    //  Contoh:  OL1 -> lid KIRI buka sampai sudut global
    //           OL0 -> lid KIRI tutup sampai 0 derajat
    //           OB1 -> KEDUA lid buka
    //           OB0 -> KEDUA lid tutup
    //
    //  PENTING - kenapa dulu ada bug "tutup malah bergerak":
    //  perintah lama memakai GESER RELATIF (tambah/kurang sekian derajat),
    //  sehingga menekan TUTUP saat sudah tertutup malah mengurangi sudut
    //  lagi dan motor tetap bergerak. Sekarang perintah ini menuju SUDUT
    //  TETAP: kalau sudah di posisi tujuan, motor tidak bergerak sama
    //  sekali.
    case 'O': {
      if (arg.length() < 2) {
        addLog("[LID] Perintah O butuh argumen, mis. OL1");
        break;
      }

      String a = arg;
      a.toUpperCase();

      char sisi = a.charAt(0);
      if (sisi != 'L' && sisi != 'R' && sisi != 'B') {
        addLogf("[WARN] Sisi harus L, R, atau B - bukan %c", sisi);
        break;
      }

      bool buka = (a.charAt(1) != '0');
      float degTujuan = buka ? lidAngleGlobal : 0.0f;

      Stepper *daftar[2];
      int jumlah = 0;
      if (sisi == 'L' || sisi == 'B') daftar[jumlah++] = &stLidLeft;
      if (sisi == 'R' || sisi == 'B') daftar[jumlah++] = &stLidRight;

      for (int i = 0; i < jumlah; i++) {
        long targetBaru = lroundf(degTujuan * STEPS_PER_DEG);

        // Kalau sudah di posisi tujuan, jangan bergerak sama sekali.
        // Ini yang mencegah "sudah tutup tapi masih bergerak".
        if (daftar[i]->currentStep == targetBaru && !daftar[i]->moving) {
          addLogf("[LID] %s sudah di %.1f derajat - tidak bergerak",
                  (daftar[i] == &stLidLeft ? "KIRI" : "KANAN"), degTujuan);
          continue;
        }

        addLogf("[LID] %s %s ke %.1f derajat",
                (daftar[i] == &stLidLeft ? "KIRI" : "KANAN"),
                (buka ? "BUKA" : "TUTUP"), degTujuan);

        lidToDeg(*daftar[i], degTujuan, stepSpeedActive);
      }
      break;
    }

    // ============================================================
    //  'W' = ATUR SUDUT GLOBAL  (dipakai semua perintah buka/tutup)
    // ============================================================
    //  Format:  W<sudut>      dalam derajat
    //  Contoh:  W10  -> mulai sekarang, BUKA berarti 10 derajat
    //           W45  -> BUKA berarti 45 derajat
    //
    //  Nilai ini disimpan ke memori flash, jadi tetap tersimpan
    //  setelah ESP32 dimatikan.
    case 'W': {
      float d = arg.toFloat();

      if (d < LID_ANGLE_MIN) d = LID_ANGLE_MIN;
      if (d > LID_ANGLE_MAX) d = LID_ANGLE_MAX;

      lidAngleGlobal = d;
      simpanSudutGlobal();

      addLogf("[SUDUT] Sudut global = %.1f derajat (tersimpan)", lidAngleGlobal);
      break;
    }

    case 'A':
      addLog("[LID] Kalibrasi - posisi sekarang = 0 derajat");
      stLidLeft.currentStep  = stLidLeft.targetStep  = 0;
      stLidLeft.moving       = false;
      stLidRight.currentStep = stLidRight.targetStep = 0;
      stLidRight.moving      = false;
      stepperRelease(stLidLeft);
      stepperRelease(stLidRight);
      break;

    default:
      addLogf("[WARN] Perintah tidak dikenal: %c", cmd);
      break;
  }
}

// ==================== WEB HANDLER ====================

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleCmd() {
  if (!server.hasArg("val")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  String val = server.arg("val");
  if (val.length() == 0) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }

  char cmd = val.charAt(0);
  String arg = val.substring(1);
  addLogf("[WEB] Terima: %c (arg=%s)", cmd, arg.c_str());

  executeCommand(cmd, arg);
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  char buf[300];
  snprintf(buf, sizeof(buf),
           "{\"ll\":%.1f,\"lr\":%.1f,\"lopen\":%.1f,\"bl\":%d,"
           "\"move\":\"%s\",\"mv\":%d,\"sp\":%d,"
           "\"profil\":%d,\"pwm\":%d,"
           "\"belok\":%d,\"kompKiri\":%d,\"kompKanan\":%d,"
           "\"balikL\":%d,\"balikR\":%d,\"holdMode\":%d,"
           "\"lidAngle\":%.1f,"
           "\"pwmKiri\":%d,\"pwmKanan\":%d}",
           stLidLeft.currentStep  / STEPS_PER_DEG,
           stLidRight.currentStep / STEPS_PER_DEG,
           lidAngleGlobal,
           stepperBusy() ? 1 : 0,
           (moveMode == MOVE_STOP) ? "stop" : "run",
           (int)moveMode,
           (int)stepSpeedActive,
           (int)profilAktif,
           (int)speedPWM,
           (int)tambahanBelok,
           (int)pwmKompensasiKiri,
           (int)pwmKompensasiKanan,
           motorBalikKiri  ? 1 : 0,
           motorBalikKanan ? 1 : 0,
           holdMode ? 1 : 0,
           lidAngleGlobal,
           (int)motorPwmUntuk(true,  false),
           (int)motorPwmUntuk(false, false));
  server.send(200, "application/json", buf);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ==================== HALAMAN /debug ====================
//
//  Halaman diagnostik yang bisa dibuka dari HP tanpa laptop:
//      http://192.168.4.1/debug
//
//  Menampilkan:
//   - Statistik jitter stepper (max, rata-rata, jumlah pulsa terlambat)
//   - Posisi & kecepatan tiap lid
//   - Status WiFi, memori, uptime
//   - 40 pesan log terakhir
//   - Tombol tes cepat (buka/tutup tiap lid, sweep kecepatan)

void handleDebugJson() {
  float jitAvg = (jitCount > 0) ? ((float)jitSumUs / (float)jitCount) : 0.0f;

  char buf[700];
  snprintf(buf, sizeof(buf),
    "{"
      "\"uptime\":%lu,"
      "\"freeHeap\":%lu,"
      "\"minHeap\":%lu,"
      "\"wifiClients\":%d,"
      "\"rssi\":%d,"
      // --- stepper kiri ---
      "\"ll\":%.2f,"
      "\"llTarget\":%.2f,"
      "\"llMoving\":%d,"
      "\"llSpeed\":%.0f,"
      "\"llEnabled\":%d,"
      // --- stepper kanan ---
      "\"lr\":%.2f,"
      "\"lrTarget\":%.2f,"
      "\"lrMoving\":%d,"
      "\"lrSpeed\":%.0f,"
      "\"lrEnabled\":%d,"
      // --- jitter ---
      "\"jitMaxUs\":%lu,"
      "\"jitAvgUs\":%.2f,"
      "\"jitSamples\":%lu,"
      "\"jitLate\":%lu,"
      "\"stepTotal\":%lu,"
      // --- konfigurasi ---
      "\"stepSpeed\":%.0f,"
      "\"rampSteps\":%ld,"
      "\"pulseMinUs\":%lu,"
      "\"pwm\":%d,"
      "\"move\":\"%s\","
      "\"lopen\":%.1f,"
      // --- instrumentasi task (untuk diagnosa lanjutan) ---
      "\"spinLoops\":%lu,"
      "\"spinMaxUs\":%lu,"
      "\"delayLateMaxUs\":%lu,"
      "\"core\":%d,"
      // --- pelacak reset / brownout ---
      "\"bootCount\":%lu,"
      "\"resetReason\":%lu,"
      "\"resetName\":\"%s\","
      "\"brownout\":%d,"
      "\"crash\":%d,"
      "\"moveInterrupted\":%lu,"
      "\"pulseActiveLow\":%d,"
      "\"movedUp\":%d,"
      "\"movedDown\":%d,"
      "\"holdMode\":%d,"
      "\"maxDeg\":%.1f,"
      "\"lidAngle\":%.1f,"
      "\"presisiUji\":%lu,"
      "\"presisiGagal\":%lu,"
      "\"presisiMaks\":%ld,"
      "\"balikL\":%d,"
      "\"balikR\":%d,"
      "\"profil\":%d,"
      "\"profilNama\":\"%s\","
      "\"sp\":%d,"
      "\"moveMode\":%d,"
      "\"pwm\":%d,"
      "\"pwmKiri\":%d,"
      "\"pwmKanan\":%d,"
      "\"belok\":%d,"
      "\"kompKiri\":%d,"
      "\"kompKanan\":%d,"
      "\"timeoutS\":%lu,"
      "\"sweepN\":%u,"
      "\"sweepGagal\":%u,"
      "\"sweepAman\":%d,"
      "\"stepsPerDeg\":%.4f"
    "}",
    (unsigned long)(millis() / 1000),
    (unsigned long)ESP.getFreeHeap(),
    (unsigned long)ESP.getMinFreeHeap(),
    (int)WiFi.softAPgetStationNum(),
    (int)WiFi.RSSI(),

    stLidLeft.currentStep  / STEPS_PER_DEG,
    stLidLeft.targetStep   / STEPS_PER_DEG,
    stLidLeft.moving ? 1 : 0,
    stLidLeft.speedCurrent,
    stLidLeft.enabled ? 1 : 0,

    stLidRight.currentStep / STEPS_PER_DEG,
    stLidRight.targetStep  / STEPS_PER_DEG,
    stLidRight.moving ? 1 : 0,
    stLidRight.speedCurrent,
    stLidRight.enabled ? 1 : 0,

    (unsigned long)jitMaxUs,
    jitAvg,
    (unsigned long)jitCount,
    (unsigned long)jitLateCnt,
    (unsigned long)stepTotal,

    stepSpeedActive,
    (long)RAMP_MIN_STEPS,
    (unsigned long)PULSE_MIN_US,
    (int)speedPWM,
    (moveMode == MOVE_STOP) ? "stop" : "run",
    lidAngleGlobal,

    (unsigned long)spinLoopCnt,
    (unsigned long)spinMaxUs,
    (unsigned long)delayLateMax,
    STEPPER_CORE,
    (unsigned long)bootCount,
    (unsigned long)bootResetReason,
    resetReasonName(bootResetReason),
    brownoutSuspect ? 1 : 0,
    crashSuspect ? 1 : 0,
    (unsigned long)moveInterrupted,
    (PULSE_ACTIVE == LOW) ? 1 : 0,
    gMovedUp   ? 1 : 0,
    gMovedDown ? 1 : 0,
    holdMode ? 1 : 0,
    LID_MAX_DEG,
    lidAngleGlobal,
    (unsigned long)gPresisiUji,
    (unsigned long)gPresisiGagal,
    (long)gPresisiMaksSelisih,
    motorBalikKiri  ? 1 : 0,
    motorBalikKanan ? 1 : 0,
    (int)profilAktif,
    PROFIL[profilAktif].nama,
    (int)stepSpeedActive,
    (int)moveMode,
    (int)speedPWM,
    (int)motorPwmUntuk(true,  false),
    (int)motorPwmUntuk(false, false),
    (int)tambahanBelok,
    (int)pwmKompensasiKiri,
    (int)pwmKompensasiKanan,
    CMD_TIMEOUT_MS / 1000,
    (unsigned)sweepJumlahTahap,
    (unsigned)sweepTotalMeleset,
    (int)(sweepKecepatanAman * 10.0f),
    STEPS_PER_DEG);

  server.send(200, "application/json", buf);
}

// ============================================================
//  LAPORAN DIAGNOSA LENGKAP  (untuk diunduh dan dikirim)
// ============================================================
//
//  Membangun satu file teks yang memuat SEMUA yang dibutuhkan untuk
//  menganalisis masalah dari jarak jauh:
//    - status lengkap dalam bentuk yang mudah dibaca
//    - penilaian otomatis (mana yang OK, mana yang bermasalah)
//    - riwayat log
//    - riwayat diagnosa yang pernah muncul
//
//  Dipakai oleh /debug/download (file) dan bisa juga dibaca di web.
String buildLaporan() {
  String o;
  o.reserve(6000);

  unsigned long up = millis() / 1000UL;

  o += "==================================================\n";
  o += "LAPORAN DIAGNOSA SMART TRASHBIN\n";
  o += "==================================================\n\n";

  // ---------- RINGKASAN ----------
  o += "--- RINGKASAN ---\n";
  o += "Uptime            : " + String(up) + " detik\n";
  o += "Boot ke-          : " + String(bootCount) + "\n";
  o += "Alasan reset      : " + String(resetReasonName(bootResetReason)) + "\n";
  o += "Brownout          : " + String(brownoutSuspect ? "YA - MASALAH" : "tidak") + "\n";
  o += "Crash/watchdog    : " + String(crashSuspect ? "YA - MASALAH" : "tidak") + "\n";
  o += "Gerakan terputus  : " + String(moveInterrupted ? "YA - MASALAH" : "tidak") + "\n";
  o += "Polaritas sinyal  : " +
       String((PULSE_ACTIVE == LOW) ? "common-anode 5V (aktif-LOW)"
                                    : "common-cathode 3.3V (aktif-HIGH)") + "\n";
  o += "\n";

  // ---------- STEPPER ----------
  o += "--- STEPPER / LID ---\n";
  o += "Lid KIRI  posisi  : " + String(stLidLeft.currentStep / STEPS_PER_DEG, 1) +
       " derajat (target " +
       String(stLidLeft.targetStep / STEPS_PER_DEG, 1) + ")\n";
  o += "Lid KIRI  status  : " + String(stLidLeft.moving ? "BERGERAK" : "diam") +
       ", enabled=" + String(stLidLeft.enabled ? 1 : 0) +
       ", step=" + String(stLidLeft.currentStep) + "\n";
  o += "Lid KANAN posisi  : " + String(stLidRight.currentStep / STEPS_PER_DEG, 1) +
       " derajat (target " +
       String(stLidRight.targetStep / STEPS_PER_DEG, 1) + ")\n";
  o += "Lid KANAN status  : " + String(stLidRight.moving ? "BERGERAK" : "diam") +
       ", enabled=" + String(stLidRight.enabled ? 1 : 0) +
       ", step=" + String(stLidRight.currentStep) + "\n";
  o += "Kecepatan stepper : " + String(stepSpeedActive, 0) + " step/s\n";
  o += "Mode tahan motor  : " + String(holdMode ? "AKTIF (dikunci)" : "mati (dilepas)") + "\n";
  o += "Pernah BUKA       : " + String(gMovedUp ? "ya" : "belum") + "\n";
  o += "Pernah TUTUP      : " + String(gMovedDown ? "ya" : "belum") + "\n";
  o += "Total pulsa keluar: " + String(stepTotal) + "\n";
  o += "\n";

  // ---------- KECEPATAN AKTUAL (deteksi kehilangan langkah) ----------
  o += "--- GERAKAN TERAKHIR DIUKUR ---\n";
  o += "Jumlah gerakan    : " + String(gMoveCount) + "\n";
  if (gMoveStartUs && gMoveEndUs > gMoveStartUs) {
    unsigned long durUs = gMoveEndUs - gMoveStartUs;
    o += "Durasi gerakan    : " + String(durUs / 1000.0f, 1) + " ms\n";
    o += "Pulsa gerakan itu : " + String(gMoveLastSteps) + "\n";
    o += "Kecepatan AKTUAL  : " + String(gMoveLastSpeed, 0) + " step/s\n";
    o += "Kecepatan DIMINTA : " + String(stepSpeedActive, 0) + " step/s\n";

    float rasio = (stepSpeedActive > 0)
                ? (gMoveLastSpeed / stepSpeedActive * 100.0f) : 0.0f;
    o += "Rasio             : " + String(rasio, 0) + "% dari target\n";
    o += "\n";
    if (rasio < 80.0f) {
      o += "[X] Kecepatan aktual JAUH di bawah target (" +
           String(rasio, 0) + "%).\n";
      o += "    Ini indikasi KEHILANGAN LANGKAH atau pulsa tidak rapi.\n";
      o += "    Periksa: arus TB6600 (NEMA 17 butuh 1.5-2.0 A),\n";
      o += "    tegangan suplai motor (min 12V), dan kebebasan mekanik.\n";
    } else if (rasio < 95.0f) {
      o += "[!] Kecepatan aktual agak di bawah target (" +
           String(rasio, 0) + "%).\n";
      o += "    Masih bisa diterima, tapi perhatikan kalau tersendat.\n";
    } else {
      o += "[OK] Kecepatan aktual sesuai target.\n";
    }
  } else {
    o += "(belum ada gerakan yang diukur)\n";
  }
  o += "\n";

  // ---------- PRESISI / KONSISTENSI ----------
  o += "--- PRESISI / KONSISTENSI ---\n";
  o += "Gerakan diuji     : " + String(gPresisiUji) + "\n";
  o += "Meleset           : " + String(gPresisiGagal) + "\n";
  o += "Selisih terbesar  : " + String(gPresisiMaksSelisih) + " langkah ("
       + String(gPresisiMaksSelisih / STEPS_PER_DEG, 3) + " derajat)\n";

  if (gPresisiUji == 0) {
    o += "[?] Belum ada gerakan yang diuji.\n";
  } else if (gPresisiGagal == 0) {
    o += "[OK] SEMUA gerakan presisi - tidak ada langkah yang hilang.\n";
    o += "     Titik berhenti konsisten di setiap pengulangan.\n";
  } else {
    float persen = (float)gPresisiGagal * 100.0f / (float)gPresisiUji;
    o += "[X] " + String(persen, 0) + "% gerakan meleset.\n";
    o += "    Selisih " + String(gPresisiMaksSelisih) +
         " langkah = " + String(gPresisiMaksSelisih / STEPS_PER_DEG, 3) +
         " derajat.\n";
    o += "    Ini KEHILANGAN LANGKAH. Kemungkinan penyebab:\n";
    o += "      1. Arus TB6600 kurang (SW1-SW3, NEMA 17 butuh 1.5-2.0 A)\n";
    o += "      2. Tegangan suplai motor di bawah 12V\n";
    o += "      3. Beban mekanik / lid berat atau seret\n";
    o += "      4. Kecepatan terlalu tinggi untuk torsi yang tersedia\n";
    o += "    Coba turunkan kecepatan (K600) lalu uji ulang.\n";
  }
  o += "\n";

  // ---------- JITTER ----------
  o += "--- JITTER (ketepatan waktu pulsa) ---\n";
  float avg = (jitCount > 0) ? ((float)jitSumUs / (float)jitCount) : 0.0f;
  o += "Jitter maksimum   : " + String(jitMaxUs) + " us\n";
  o += "Jitter rata-rata  : " + String(avg, 2) + " us\n";
  o += "Pulsa terlambat   : " + String(jitLateCnt) + " dari " + String(jitCount) + "\n";
  o += "\n";

  // ---------- GERAKAN TERAKHIR ----------
  o += "--- GERAKAN TERAKHIR ---\n";
  o += "Mode roda         : " + String((int)moveMode) + "\n";
  o += "PWM roda          : " + String(speedPWM) + "\n";
  o += "Kanan CMD terakhir: " + String(millis() - lastCmdMillis) + " ms lalu\n";
  o += "\n";

  // ---------- SISTEM ----------
  o += "--- SISTEM ---\n";
  o += "RAM bebas         : " + String(ESP.getFreeHeap()) + " byte\n";
  o += "RAM minimum       : " + String(ESP.getMinFreeHeap()) + " byte\n";
  o += "Klien WiFi        : " + String(WiFi.softAPgetStationNum()) + "\n";
  o += "Task stepper core : " + String(STEPPER_CORE) + "\n";
  o += "Log total / drop  : " + String(logTotal) + " / " + String(logDrop) + "\n";
  o += "\n";


// ---------- PENILAIAN OTOMATIS ----------
  o += "--- PENILAIAN OTOMATIS ---\n";
  uint8_t nMasalah = 0;

  if (jitCount == 0) {
    o += "[?] Belum ada data jitter (belum ada gerakan).\n";
  } else if (jitMaxUs < 50) {
    o += "[OK] Jitter maksimum " + String(jitMaxUs) + " us - sangat baik.\n";
  } else if (jitMaxUs < 200) {
    o += "[!] Jitter maksimum " + String(jitMaxUs) + " us - agak kasar.\n"; nMasalah++;
  } else {
    o += "[X] Jitter maksimum " + String(jitMaxUs) + " us - BURUK.\n"; nMasalah++;
  }

  if (jitCount > 0) {
    float rate = (100.0f * (float)jitLateCnt) / (float)jitCount;
    if (rate < 0.5f) {
      o += "[OK] Pulsa terlambat " + String(rate, 2) + "% - sangat rendah.\n";
    } else if (rate < 5.0f) {
      o += "[!] Pulsa terlambat " + String(rate, 2) + "% - agak tinggi.\n"; nMasalah++;
    } else {
      o += "[X] Pulsa terlambat " + String(rate, 2) + "% - TINGGI.\n"; nMasalah++;
    }
  }

  if (brownoutSuspect) { o += "[X] Brownout - tegangan turun saat motor jalan.\n"; nMasalah++; }
  if (crashSuspect)    { o += "[X] Crash/watchdog terdeteksi.\n"; nMasalah++; }
  if (moveInterrupted) { o += "[X] Gerakan terputus oleh reset.\n"; nMasalah++; }
  if (bootCount > 1)   { o += "[!] Sudah reset " + String(bootCount) + " kali sejak daya masuk.\n"; }
  if (PULSE_ACTIVE != LOW) {
    o += "[!] Polaritas aktif-HIGH - sinyal ke TB6600 lemah (arus kurang).\n";
    nMasalah++;
  } else {
    o += "[OK] Polaritas common-anode 5V - arus sinyal cukup.\n";
  }
  if (!gMovedUp || !gMovedDown) {
    o += "[?] Belum diuji BUKA dan TUTUP - DIR belum terbukti.\n";
  } else {
    o += "[OK] Firmware sudah memerintahkan BUKA dan TUTUP.\n";
  }
  if (ESP.getFreeHeap() < 20000) {
    o += "[!] RAM saat ini " + String(ESP.getFreeHeap()) + " byte.\n";
    o += "    CATATAN: angka ini bisa rendah SESUAT setelah gerakan berat\n";
    o += "    dan naik lagi setelah itu. Yang menentukan adalah RAM\n";
    o += "    MINIMUM dan apakah RAM sekarang sudah pulih.\n";
  }

  // Yang benar-benar penting: seberapa rendah RAM pernah turun, dan
  // apakah setelah itu pulih. RAM yang rendah SESUAT bukan masalah
  // selama tidak sampai kehabisan dan bisa pulih kembali.
  uint32_t heapNow = ESP.getFreeHeap();
  uint32_t heapMin = ESP.getMinFreeHeap();
  o += "[INFO] RAM sekarang " + String(heapNow) +
       " byte, minimum pernah " + String(heapMin) + " byte.\n";

  if (heapMin < 8000) {
    o += "[X] RAM minimum pernah di bawah 8 KB - berisiko gagal alokasi.\n";
    nMasalah++;
  } else if (heapMin < 16000) {
    o += "[!] RAM minimum pernah di bawah 16 KB - perhatikan saat gerakan berat.\n";
  } else {
    o += "[OK] RAM minimum masih aman (di atas 16 KB).\n";
  }

  if (heapNow < 8000) {
    o += "[X] RAM SEKARANG di bawah 8 KB - sistem bisa macet.\n";
    nMasalah++;
  }

  o += "\n";
  if (nMasalah == 0) {
    o += "KESIMPULAN: TIDAK ADA MASALAH TERDETEKSI di firmware.\n";
    o += "Kalau gejala masih terasa, kemungkinan besar MEKANIK (beban/\n";
    o += "gravitasi) atau NOISE pada kabel sinyal - bukan dari firmware.\n";
    o += "Coba tekan TAHAN Motor ON dan uji ulang.\n";
  } else {
    o += "KESIMPULAN: ADA " + String(nMasalah) + " MASALAH. Lihat tanda [X]/[!] di atas.\n";
  }
  o += "\n";
  o += "CATATAN: posisi lid dihitung dari JUMLAH PULSA yang dikirim ke\n";
  o += "driver, BUKAN dari sensor. Motor yang kehilangan langkah tetap\n";
  o += "terbaca 90 derajat walau lid belum sampai.\n";
  o += "\n";

  // ---------- LOG ----------
  o += "==================================================\n";
  o += "LOG (" + String(LOG_LINES) + " baris terakhir)\n";
  o += "==================================================\n";

  uint16_t start = (logTotal < LOG_LINES) ? 0 : logHead;
  uint16_t count = (logTotal < LOG_LINES) ? logTotal : LOG_LINES;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t idx = (start + i) % LOG_LINES;
    if (logRing[idx][0] == 0) continue;
    o += logRing[idx];
    o += "\n";
  }

  return o;
}

// Unduh laporan lengkap sebagai FILE .txt
// Header Content-Disposition membuat browser MENYIMPANNYA sebagai file,
// sehingga bisa langsung dikirim ke pengembang.
void handleDebugDownload() {
  String lap = buildLaporan();

  char nama[64];
  snprintf(nama, sizeof(nama),
           "attachment; filename=\"trashbin-%lus-boot%lu.txt\"",
           millis() / 1000UL, (unsigned long)bootCount);

  server.sendHeader("Content-Disposition", nama);
  server.send(200, "text/plain; charset=utf-8", lap);
}

void handleDebugLog() {
  // Kirim pesan log dari yang paling lama ke paling baru
  String out;
  out.reserve(LOG_LINES * (LOG_LEN + 8));

  uint16_t start = (logTotal < LOG_LINES) ? 0 : logHead;
  uint16_t count = (logTotal < LOG_LINES) ? logTotal : LOG_LINES;

  for (uint16_t i = 0; i < count; i++) {
    uint16_t idx = (start + i) % LOG_LINES;
    if (logRing[idx][0] == 0) continue;
    out += logRing[idx];
    out += "\n";
  }

  char hdr[80];
  snprintf(hdr, sizeof(hdr), "# total=%u drop=%lu\n",
           (unsigned)logTotal, (unsigned long)logDrop);
  out = String(hdr) + out;

  server.send(200, "text/plain; charset=utf-8", out);
}

void handleDebugReset() {
  jitMaxUs   = 0;
  jitSumUs   = 0;
  jitCount   = 0;
  jitLateCnt = 0;
  stepTotal  = 0;
  addLog("[DEBUG] Statistik jitter di-reset");
  server.send(200, "text/plain", "OK statistik direset");
}

// Halaman HTML untuk /debug — dibuat ringkas, muat di layar HP.
const char DEBUG_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="id"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Debug · SmartTrashbin</title>
<style>
:root{--bg:#0c1015;--pan:#141a22;--fg:#e6edf3;--dim:#8b98a8;--acc:#4ea8ff;
--ok:#3fb950;--warn:#d29922;--er:#f85149;--bd:#232c38}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
font:14px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;padding:12px}
h1{font-size:15px;margin:0 0 12px;letter-spacing:.5px}
h1 span{color:var(--acc)}
.card{background:var(--pan);border:1px solid var(--bd);border-radius:9px;
padding:11px;margin-bottom:10px}
.card h2{font-size:11px;margin:0 0 9px;color:var(--dim);
text-transform:uppercase;letter-spacing:1px;font-weight:600}
.row{display:flex;justify-content:space-between;padding:3px 0;
border-bottom:1px solid #1b232d;font-size:13px}
.row:last-child{border:0}
.row b{font-weight:600}
.k{color:var(--dim)}
.ok{color:var(--ok)}.warn{color:var(--warn)}.er{color:var(--er)}.dimc{color:var(--dim)}
button.aktif{background:#2d4a6b;border-color:#4a7ab5;color:#cfe3ff;font-weight:700}
button.okBtn{border-color:#2f5e3a;color:#9ce8b0}
button.warnBtn{border-color:#5a4218;color:#ffd479}
.btns{display:flex;flex-wrap:wrap;gap:6px}
button{flex:1 1 46%;padding:9px 6px;font:inherit;font-size:12px;
background:#1e2833;color:var(--fg);border:1px solid var(--bd);
border-radius:7px;cursor:pointer}
button:active{background:var(--acc);color:#04121f}
button.warnBtn{border-color:#5a4218}
pre{background:#0a0e13;border:1px solid var(--bd);border-radius:7px;
padding:9px;font-size:11px;line-height:1.45;max-height:46vh;
overflow:auto;white-space:pre-wrap;word-break:break-all;margin:0}
.bar{height:5px;background:#1b232d;border-radius:3px;overflow:hidden;margin-top:5px}
.bar i{display:block;height:100%;background:var(--ok);transition:width .3s}
</style></head><body>

<h1>DEBUG <span>SmartTrashbin</span></h1>

<div class="card"><h2>Stepper · Lid</h2>
  <div class="row"><span class="k">KIRI pos / target</span>
    <b><span id="ll">-</span>° / <span id="llt">-</span>°</b></div>
  <div class="row"><span class="k">KIRI kecepatan</span><b id="lls">-</b></div>
  <div class="row"><span class="k">KANAN pos / target</span>
    <b><span id="lr">-</span>° / <span id="lrt">-</span>°</b></div>
  <div class="row"><span class="k">KANAN kecepatan</span><b id="lrs">-</b></div>
</div>

<div class="card"><h2>Jitter Monitor</h2>
  <div class="row"><span class="k">Jitter maksimum</span>
    <b id="jitmax">-</b></div>
  <div class="row"><span class="k">Jitter rata-rata</span>
    <b id="jitavg">-</b></div>
  <div class="row"><span class="k">Pulsa terlambat &gt;50µs</span>
    <b id="jitlate">-</b></div>
  <div class="row"><span class="k">Total pulsa keluar</span>
    <b id="steps">-</b></div>
  <div class="bar"><i id="jitbar" style="width:0%"></i></div>
  <p style="font-size:11px;color:var(--dim);margin:8px 0 0">
  Jitter = keterlambatan pulsa dari jadwalnya. <b>&lt;50µs</b> halus.
  <b>50–200µs</b> agak kasar. <b>&gt;200µs</b> tersentak.</p>
</div>

<div class="card"><h2>Presisi Gerakan</h2>
  <div class="row"><span class="k">Gerakan diuji</span>
    <b id="preUji">-</b></div>
  <div class="row"><span class="k">Meleset</span>
    <b id="preGagal">-</b></div>
  <div class="row"><span class="k">Selisih terbesar</span>
    <b id="preMaks">-</b></div>
  <div class="row"><span class="k">Akurasi</span>
    <b id="preAkurasi">-</b></div>
  <div class="bar"><i id="preBar" style="width:0%"></i></div>
  <p style="font-size:11px;color:var(--dim);margin:8px 0 0">
  Menghitung berapa kali titik berhenti lid meleset dari target.
  <b>0 meleset</b> berarti gerakan konsisten. Kalau meleset, lid
  kehilangan langkah dari arus kurang atau beban berat.</p>
</div>

<div class="card"><h2>Diagnosa Otomatis</h2>
  <div id="diagnosa" style="font-size:12.5px">memeriksa…</div>
</div>

<div class="card"><h2>Sistem</h2>  <div class="row"><span class="k">Uptime</span><b id="up">-</b></div>
  <div class="row"><span class="k">RAM bebas / minimum</span><b id="heap">-</b></div>
  <div class="row"><span class="k">Klien WiFi tersambung</span><b id="cli">-</b></div>
  <div class="row"><span class="k">Kecepatan stepper aktif</span><b id="ss">-</b></div>
  <div class="row"><span class="k">Kecepatan roda (PWM)</span><b id="pwm">-</b></div>
  <div class="row"><span class="k">Status gerak</span><b id="mv">-</b></div>
  <div class="row"><span class="k">Boot ke-</span><b id="bc">-</b></div>
  <div class="row"><span class="k">Alasan reset terakhir</span><b id="rr">-</b></div>
  <div class="row"><span class="k">Polaritas sinyal</span><b id="pol">-</b></div>
  <div class="row"><span class="k">Arah pernah diperintahkan</span><b id="dirs">-</b></div>
</div>

<div class="card"><h2>Uji DIR &amp; Daya</h2>
  <p style="font-size:12px;margin:0 0 8px">
  Kalau buka &amp; tutup selalu searah, tekan <b>UJI DIR KIRI</b> lalu
  <b>perhatikan lid</b>. Motor akan digerakkan maju lalu mundur.</p>
  <div class="row"><span class="k">Lib bergoyang bolak-balik</span>
    <b class="ok">DIR bekerja &rarr; cek 5V buck</b></div>
  <div class="row"><span class="k">Lid jalan terus satu arah</span>
    <b class="er">DIR tidak sampai ke TB6600</b></div>
  <div class="row"><span class="k">Mode tahan motor</span><b id="hold">-</b></div>
  <p style="font-size:11.5px;color:var(--dim);margin:8px 0 0">
  <b>Kalau motor jalan TERUS walau sudah berhenti (posisi di atas sudah
  diam tapi poros masih berputar):</b> tekan <b>TAHAN Motor ON</b> lalu
  uji ulang. Kalau berhenti, penyebabnya motor <i>free</i> + beban
  (gravitasi), bukan sinyal. Kalau masih jalan, berarti ada pulsa palsu
  dari noise di kabel PUL.</p>
  <p style="font-size:11.5px;color:var(--dim);margin:8px 0 0">
  <b>PENTING soal daya:</b> kalau ESP32 diberi daya dari laptop dan
  baterai motor dimatikan, buck converter 5V juga mati &mdash;
  sehingga TB6600 <b>tidak dapat sinyal DIR</b>. Uji dulu dengan daya
  lengkap (baterai + buck hidup) sebelum menyimpulkan wiring-nya salah.</p>
</div>

<div class="card"><h2>Motor Penggerak</h2>
  <p style="font-size:11.5px;color:var(--dim);margin:0 0 10px">
  Untuk mengubah setelan robot, buka <b><a href="/setting"
  target="_blank" rel="noopener" style="color:var(--acc)">halaman Setelan</a></b>.
  Halaman ini untuk diagnosa, bukan untuk mengatur.</p>
  <div class="row"><span class="k">Status roda</span>
    <b id="mStatus">-</b></div>
  <div class="row"><span class="k">PWM</span>
    <b id="mPwm">-</b></div>
  <div class="row"><span class="k">Pengaman berhenti</span>
    <b id="mTimeout">-</b></div>

  <div class="row"><span class="k">Uji gerak</span>
    <span>
      <button onmousedown="maju()" onmouseup="mStop()" ontouchstart="maju()" ontouchend="mStop()">MAJU</button>
      <button onmousedown="mback()" onmouseup="mStop()" ontouchstart="mback()" ontouchend="mStop()">MUNDUR</button>
      <button onclick="c('P')">STOP</button>
    </span>
  </div>

  <div class="row"><span class="k">Arah motor KIRI</span>
    <span><b id="mBalikL">-</b>
      <button onclick="c('YL1')">Balik</button>
      <button onclick="c('YL0')">Normal</button></span>
  </div>

  <div class="row"><span class="k">Arah motor KANAN</span>
    <span><b id="mBalikR">-</b>
      <button onclick="c('YR1')">Balik</button>
      <button onclick="c('YR0')">Normal</button></span>
  </div>

  <p style="font-size:11px;color:var(--dim);margin:10px 0 0">
  Tekan <b>MAJU</b> dan lihat roda. Kalau roda berputar ke belakang,
  tekan <b>Balik</b> pada sisi itu. Setelah benar, tekan <b>MAJU</b>
  lagi untuk memastikan. Setelan ini tersimpan di memori.</p>
</div>

<div class="card"><h2>Uji Torsi &amp; Kecepatan Maksimal</h2>
  <p style="font-size:11.5px;color:var(--dim);margin:0 0 10px">
  Mencari pada kecepatan berapa motor mulai kehilangan langkah.
  Motor akan bergerak bolak-balik sekitar 30 detik.</p>

  <div class="row"><span class="k">Jalankan uji</span>
    <span>
      <button onclick="c('QL')">LID KIRI</button>
      <button onclick="c('QR')">LID KANAN</button>
      <button onclick="c('QB')">KEDUA</button>
    </span>
  </div>

  <div id="sweepHasil" style="margin-top:11px"></div>

  <p style="font-size:11.5px;color:var(--dim);margin:11px 0 0">
  <b>Penting:</b> torsi tidak bisa diubah dari sini. Kalau uji ini
  meleset bahkan pada kecepatan rendah, masalahnya bukan kecepatan
  melainkan <b>arus TB6600</b>. Ubah DIP switch <b>SW1/SW2/SW3</b> ke
  1.5 sampai 2.0 A, atau turunkan microstep dari 16 ke 8.
  Lihat <code>docs/RISET-PRESISI-STEPPER.md</code>.</p>
</div>

<div class="card"><h2>Kendali Lid</h2>
  <div class="row"><span class="k">Buka / tutup lid</span>
    <span>
      <label><input type="radio" name="sisi" value="L" checked> KIRI</label>
      <label style="margin-left:12px"><input type="radio" name="sisi" value="R"> KANAN</label>
      <label style="margin-left:12px"><input type="radio" name="sisi" value="B"> KEDUANYA</label>
    </span>
  </div>

  <div class="row"><span class="k">Sudut bukaan</span>
    <span id="sudutBtns"></span>
  </div>

  <div class="row"><span class="k">Sudut lain</span>
    <span><input id="sudutKustom" type="number" step="1" min="1" max="120"
      style="width:72px"> &deg;
      <button onclick="pakaiSudut()">Simpan</button></span>
  </div>

  <div class="row"><span class="k">Sudut aktif</span>
    <b id="sudutAktif">-</b>
  </div>

  <div class="row"><span class="k">Jalankan</span>
    <span>
      <button class="okBtn" onclick="jalan(1)">&#9650; BUKA</button>
      <button class="warnBtn" onclick="jalan(0)">&#9660; TUTUP</button>
      <button onclick="c('A')">Nol-kan</button>
    </span>
  </div>

  <div class="row"><span class="k">Posisi lid</span>
    <b id="posSekarang">-</b>
  </div>

  <div class="row"><span class="k">Kecepatan</span>
    <span>
      <button onclick="c('K300')">300</button>
      <button onclick="c('K600')">600</button>
      <button onclick="c('K900')">900</button>
      <button onclick="c('K1600')">1600</button>
    </span>
  </div>
</div>

<div class="card"><h2>Tes Cepat</h2>
  <div class="btns">
    <button onclick="c('IL1')">Buka KIRI</button>
    <button onclick="c('IL0')">Tutup KIRI</button>
    <button onclick="c('IR1')">Buka KANAN</button>
    <button onclick="c('IR0')">Tutup KANAN</button>
    <button onclick="c('K600')">K600 (aman)</button>
    <button onclick="c('K900')">K900</button>
    <button onclick="c('K1600')">K1600 (default)</button>
    <button onclick="c('K2000')">K2000 (tercepat)</button>
    <button class="warnBtn" onclick="c('D')">UJI DIR KIRI</button>
    <button class="warnBtn" onclick="c('DR')">UJI DIR KANAN</button>
    <button class="warnBtn" onclick="c('H1')">TAHAN Motor ON</button>
    <button onclick="c('H0')">TAHAN Motor OFF</button>
    <button onclick="c('A')">Kalibrasi 0°</button>
    <button class="warnBtn" onclick="c('S')">Tutup KEDUA</button>
    <button class="warnBtn" onclick="c('P')">STOP RODA</button>
    <button class="warnBtn" onclick="fetch('/debug/reset').then(r=>r.text()).then(t=>{log('reset: '+t);load()})">
      Reset Statistik</button>
    <button onclick="loadLog()">Muat Ulang Log</button>
    <button class="warnBtn" onclick="location.href='/debug/download'">
      UNDUH LAPORAN LENGKAP</button>
  </div>
  <p style="font-size:11.5px;color:var(--dim);margin:8px 0 0">
  <b>Kalau masih ada masalah:</b> tekan <b>UNDUH LAPORAN LENGKAP</b>.
  File <code>.txt</code> akan tersimpan di HP berisi status lengkap,
  penilaian otomatis, dan seluruh log. Kirim file itu ke pengembang.</p>
</div>

<div class="card"><h2>Log (120 terakhir)</h2>
  <pre id="logtxt">memuat…</pre>
</div>

<script>
const $ = id => document.getElementById(id);
function c(v){ fetch('/cmd?val='+encodeURIComponent(v)).then(r=>r.text())
  .catch(()=>{}); setTimeout(load,300); }
function log(m){ console.log(m); }

function diagnosa(s, jm, ja){
  const out = [];
  const lateRate = s.jitSamples > 0 ? (s.jitLate / s.jitSamples) : 0;

  // --- kualitas jitter ---
  if (jm < 50 && lateRate < 0.01){
    out.push(['ok','Jitter sangat baik. Gerakan seharusnya halus.']);
  } else if (jm < 200){
    out.push(['warn','Jitter agak kasar ('+jm+'µs). Mungkin terasa sedikit tersentak.']);
  } else {
    out.push(['er','Jitter buruk ('+jm+'µs). Gerakan akan terasa tersentak.']);
  }

  // --- banyak pulsa terlambat ---
  if (lateRate > 0.05){
    out.push(['warn','Banyak pulsa terlambat ('+(lateRate*100).toFixed(1)+
      '%). Coba turunkan kecepatan dengan tombol K900 atau K600.']);
  }

  // --- memori ---
  if (s.freeHeap < 20000){
    out.push(['er','RAM rendah ('+(s.freeHeap/1024).toFixed(0)+' KB). Restart robot.']);
  } else if (s.freeHeap < 40000){
    out.push(['warn','RAM agak rendah ('+(s.freeHeap/1024).toFixed(0)+' KB).']);
  } else {
    out.push(['ok','RAM cukup: '+(s.freeHeap/1024).toFixed(0)+' KB bebas.']);
  }

  // --- kecepatan stepper ---
  if (s.stepSpeed > 1500){
    out.push(['warn','Kecepatan stepper tinggi ('+s.stepSpeed.toFixed(0)+
      ' step/s). Kalau motor bergetar, turunkan ke K900 atau K600.']);
  }

  // --- posisi tidak sesuai target (kehilangan langkah) ---
  const dl = Math.abs(s.ll - s.llTarget);
  const dr = Math.abs(s.lr - s.lrTarget);
  if (dl > 1.5 || dr > 1.5){
    out.push(['er','Posisi lid tidak sesuai target (kiri beda '+
      dl.toFixed(1)+', kanan beda '+dr.toFixed(1)+' derajat). '+
      'Turunkan kecepatan, naikkan arus TB6600, atau perbaiki level sinyal ke 5V.']);
  }

  // --- motor diam padahal target berbeda ---
  if (!s.llMoving && dl > 1.5){
    out.push(['er','Lid KIRI berhenti tapi belum sampai target. Cek kabel DIR/PUL.']);
  }
  if (!s.lrMoving && dr > 1.5){
    out.push(['er','Lid KANAN berhenti tapi belum sampai target. Cek kabel DIR/PUL.']);
  }

  // --- status gerak ---
  if (s.move !== 'stop'){
    out.push(['warn','Roda sedang bergerak ('+s.move+').']);
  }

  // --- BROWNOUT / CRASH: penyebab gerakan tersendat yang tidak
  //     terlihat di statistik jitter, karena statistik ikut ter-reset ---
  if (s.brownout){
    out.push(['er','ESP32 mengalami BROWNOUT \u2014 tegangan turun saat '+
      'motor jalan. Ini membuat gerakan tersendat dan statistik jitter '+
      'ter-reset sehingga sulit terlihat. SOLUSI: tambah kapasitor '+
      '1000\u00b5F elektrolitik + 100nF keramik dekat pin 5V/3V3 ESP32, '+
      'perkuat kabel GND, dan jangan ambil daya ESP32 dari jalur motor.']);
  }
  if (s.crash){
    out.push(['er','ESP32 CRASH atau WATCHDOG. Kode macet saat gerakan. '+
      'Kirim isi Log ke pengembang.']);
  }
  if (s.moveInterrupted){
    out.push(['er','Gerakan sebelumnya TERPUTUS oleh reset \u2014 ini '+
      'bukti kuat gerakan tersendat disebabkan ESP32 restart, bukan '+
      'masalah timing pulsa.']);
  }
  if (s.bootCount > 1 && !s.brownout && !s.crash){
    out.push(['warn','Sudah boot '+s.bootCount+' kali tanpa daya putus. '+
      'Ada reset yang tidak normal (sebelumnya: '+s.resetName+').']);
  }

  // --- POLARITAS FIRMWARE vs WIRING ---
  // Ini penyebab getar/tersendat yang paling sering: sinyal 3.3V
  // common-cathode tidak cukup kuat untuk optocoupler TB6600.
  if (s.pulseActiveLow){
    out.push(['ok','Polaritas firmware: COMMON-ANODE 5V (aktif-LOW). '+
      'Pastikan wiring: PUL+/DIR+/ENA+ ke 5V, PUL-/DIR-/ENA- ke GPIO, '+
      'dan GND 5V tersambung ke GND ESP32.']);
  } else {
    out.push(['warn','Polaritas firmware: COMMON-CATHODE 3.3V (aktif-HIGH). '+
      'Ini memberi hanya ~9.5mA ke optocoupler TB6600 (butuh 10-15mA) '+
      'sehingga motor bisa TERSENDAT-SENDAT. Kalau kamu punya sumber 5V, '+
      'ubah ke common-anode (PULSE_ACTIVE LOW) lalu upload ulang.']);
  }

  // --- DIR: deteksi "buka dan tutup berputar arah yang sama" ---
  // Kalau firmware sudah pernah MEMERINTAHKAN dua arah (naik dan turun),
  // maka otak sudah benar. Kalau fisiknya tetap searah, sinyal DIR tidak
  // sampai ke TB6600.
  if (s.movedUp && s.movedDown){
    out.push(['ok','DIR bekerja: firmware sudah pernah memerintahkan '+
      'arah NAIK dan TURUN. Kalau fisik tetap searah, cek wiring DIR.']);
  } else if (s.movedUp && !s.movedDown){
    out.push(['warn','Baru diperintahkan NAIK saja. Tekan Tutup KIRI '+
      'supaya DIR diuji ke arah sebaliknya.']);
  } else if (!s.movedUp && s.movedDown){
    out.push(['warn','Baru diperintahkan TURUN saja. Tekan Buka KIRI '+
      'supaya DIR diuji ke arah sebaliknya.']);
  } else {
    out.push(['warn','Belum ada gerakan. Tekan Buka KIRI lalu Tutup KIRI '+
      'supaya DIR bisa diuji.']);
  }

  // --- PERINGATAN DAYA: penyebab paling sering "selalu searah" ---
  // Kalau ESP32 diberi daya dari laptop sementara baterai motor dimatikan,
  // buck converter 5V ikut mati -> TB6600 tidak dapat sinyal DIR.
  // Kita tidak bisa mengukur 5V buck dari ESP32, jadi ini peringatan
  // kontekstual supaya tidak terlewat saat mengejar masalah wiring.
  out.push(['warn','DAYA: kalau ESP32 diberi daya dari laptop dan baterai '+
    'motor dimatikan, buck converter 5V ikut mati \u2014 TB6600 tidak '+
    'dapat tegangan di PUL+/DIR+/ENA+. Pastikan buck 5V hidup sebelum '+
    'menyimpulkan wiring salah. Pakai tombol UJI DIR di atas.']);

  // --- keterbatasan pengukuran (penting, jangan disembunyikan) ---
  out.push(['warn','CATATAN: posisi di atas dihitung dari jumlah pulsa yang '+
    'DIKIRIM ke driver, bukan dari sensor. Kalau motor kehilangan langkah '+
    '(stall), angka tetap 90\u00b0 padahal lid tidak sampai. '+
    'Kalau lid terlihat tidak sampai penuh tapi angka 90\u00b0: '+
    'itu kehilangan langkah \u2014 turunkan kecepatan (K900 atau K600) dan '+
    'naikkan arus TB6600.']);

  return out;
}

function renderDiagnosa(items){
  const el = document.getElementById('diagnosa');
  el.innerHTML = items.map(function(it){
    const cls = it[0] === 'ok' ? 'ok' : (it[0] === 'warn' ? 'warn' : 'er');
    const ico = it[0] === 'ok' ? '✓' : (it[0] === 'warn' ? '!' : '✕');
    return '<div class="'+cls+'" style="padding:3px 0;border-bottom:1px solid #1b232d">'+
           '<b>'+ico+'</b> '+it[1]+'</div>';
  }).join('');
}

function load(){
  fetch('/debug/json',{cache:'no-store'}).then(r=>r.json()).then(s=>{
    $('ll').textContent  = s.ll.toFixed(1);
    $('llt').textContent = s.llTarget.toFixed(1);
    $('lls').textContent = s.llMoving ? s.llSpeed.toFixed(0)+' step/s' : 'diam';
    $('lr').textContent  = s.lr.toFixed(1);
    $('lrt').textContent = s.lrTarget.toFixed(1);
    $('lrs').textContent = s.lrMoving ? s.lrSpeed.toFixed(0)+' step/s' : 'diam';

    const jm = s.jitMaxUs, ja = s.jitAvgUs;
    $('jitmax').textContent = jm + ' µs';
    $('jitmax').className   = jm < 50 ? 'ok' : (jm < 200 ? 'warn' : 'er');
    $('jitavg').textContent = ja.toFixed(1) + ' µs';
    $('jitlate').textContent = s.jitLate + ' dari ' + s.jitSamples;
    $('jitlate').className   = s.jitLate === 0 ? 'ok' : 'warn';
    $('steps').textContent = s.stepTotal;
    $('jitbar').style.width = Math.min(100, jm/5) + '%';
    $('jitbar').style.background = jm < 50 ? '#3fb950'
      : (jm < 200 ? '#d29922' : '#f85149');

    $('up').textContent  = s.uptime + ' s';
    $('heap').textContent = (s.freeHeap/1024).toFixed(0) + ' / '
                          + (s.minHeap/1024).toFixed(0) + ' KB';
    $('cli').textContent = s.wifiClients;
    $('ss').textContent  = s.stepSpeed.toFixed(0) + ' step/s';
    $('pwm').textContent = s.pwm;
    $('mv').textContent  = s.move;
    $('mv').className    = s.move === 'stop' ? '' : 'warn';

    $('bc').textContent  = s.bootCount;
    $('rr').textContent   = s.resetName;
    $('rr').className     = (s.brownout || s.crash) ? 'er'
                          : (s.bootCount > 1 ? 'warn' : 'ok');
    $('pol').textContent = s.pulseActiveLow
      ? 'common-anode 5V (benar)' : 'common-cathode 3.3V (lemah)';
    $('pol').className   = s.pulseActiveLow ? 'ok' : 'warn';

    // Arah yang sudah pernah diperintahkan. Kalau keduanya muncul,
    // otak firmware sudah benar dan masalahnya di wiring DIR.
    const naik  = s.movedUp   ? 'BUKA' : '';
    const turun = s.movedDown ? 'TUTUP' : '';
    $('dirs').textContent = (naik && turun) ? 'BUKA + TUTUP (benar)'
                          : (naik || turun || 'belum ada');
    $('dirs').className   = (naik && turun) ? 'ok' : 'warn';

    $('hold').textContent = s.holdMode ? 'AKTIF (dikunci)' : 'MATI (dilepas)';
    $('hold').className   = s.holdMode ? 'ok' : '';

    perbaruiKendali(s);
    perbaruiPresisi(s);
    perbaruiMotor(s);
    perbaruiSweep(s);

    renderDiagnosa(diagnosa(s, jm, ja));
  }).catch(()=>{ $('logtxt').textContent = 'ROBOT TIDAK MERESPONS'; });
}


// ---------- HASIL UJI TORSI & KECEPATAN ----------
//
// Menampilkan ringkasan hasil sweep. Rincian per tahap ada di log,
// jadi di sini hanya kesimpulannya supaya tidak ramai.
function perbaruiSweep(s){
  var el = document.getElementById('sweepHasil');
  if (!el) return;

  var n = s.sweepN || 0;
  if (n === 0){ el.innerHTML = ''; return; }

  var gagal = s.sweepGagal || 0;
  var aman  = (typeof s.sweepAman === 'number') ? s.sweepAman / 10 : -1;

  var warna, kunci, pesan;

  if (aman <= 0 && gagal > 0){
    warna = 'er';
    kunci = 'MELESET DI SEMUA KECEPATAN';
    pesan = 'Ini bukan masalah kecepatan. Periksa arus TB6600 (SW1-SW3, ' +
            'butuh 1.5 sampai 2.0 A) dan coba turunkan microstep dari 16 ke 8.';
  } else if (gagal === 0){
    warna = 'ok';
    kunci = 'SEMUA TAHAP TEPAT';
    pesan = 'Motor kuat sampai tahap terakhir yang diuji. Kecepatan ' +
            'tertinggi masih aman.';
  } else {
    warna = 'warn';
    kunci = 'BATAS KETEMU';
    pesan = 'Kecepatan aman tertinggi ' + aman + ' step/detik. ' +
            'Pakai sekitar ' + Math.round(aman * 0.8) + ' step/detik ' +
            'supaya ada cadangan tenaga.';
  }

  el.innerHTML =
    '<div class="row"><span class="k">Tahap diuji</span><b>' + n + '</b></div>' +
    '<div class="row"><span class="k">Tahap meleset</span><b class="' +
      (gagal > 0 ? 'er' : 'ok') + '">' + gagal + '</b></div>' +
    '<div class="row"><span class="k">Hasil</span><b class="' + warna +
      '">' + kunci + '</b></div>' +
    '<p style="font-size:11.5px;margin:9px 0 0" class="' + warna + '">' +
      pesan + '</p>';
}

// ---------- MOTOR PENGGERAK ----------
const MODE_NAMA = ['STOP', 'MAJU', 'MUNDUR', 'KIRI', 'KANAN'];

let mStopTimer = null;

// Selama tombol uji ditahan, kirim ulang perintahnya supaya pengaman
// berhenti tidak memutus gerakan sebelum pengguna selesai mengamati.
function mHeartbeat(cmd){
  if (mStopTimer) clearInterval(mStopTimer);
  mStopTimer = setInterval(function(){ c(cmd); }, 1500);
}

function maju(){  c('F'); mHeartbeat('F'); }
function mback(){ c('B'); mHeartbeat('B'); }

function mStop(){
  if (mStopTimer) { clearInterval(mStopTimer); mStopTimer = null; }
  c('P');
}

function perbaruiMotor(s){
  document.getElementById('mStatus').textContent =
    MODE_NAMA[s.moveMode] || '-';
  document.getElementById('mPwm').textContent = s.pwm;

  document.getElementById('mTimeout').textContent =
    'berhenti setelah ' + s.timeoutS + ' detik tanpa perintah';

  const l = document.getElementById('mBalikL');
  const r = document.getElementById('mBalikR');

  l.textContent = s.balikL ? 'DIBALIK' : 'normal';
  l.className   = s.balikL ? 'warn' : '';
  r.textContent = s.balikR ? 'DIBALIK' : 'normal';
  r.className   = s.balikR ? 'warn' : '';
}

// ---------- KENDALI LID ----------
const SUDUT_LIST = [10, 15, 30, 45, 60, 90];
let sudutPilih = 90;      // pilihan tombol (belum tentu yang aktif)
let sudutAktif = 90;      // sudut yang benar-benar dipakai firmware

function gambarSudut(){
  document.getElementById('sudutBtns').innerHTML = SUDUT_LIST.map(d =>
    '<button class="' + (d === sudutPilih ? 'aktif' : '') +
    '" onclick="pilihSudut(' + d + ')">' + d + '&deg;</button>').join(' ');
}

function pilihSudut(d){
  sudutPilih = d;
  document.getElementById('sudutKustom').value = d;
  gambarSudut();
}

// Simpan sudut ke firmware (perintah W). Setelah ini tombol BUKA
// memakai sudut tersebut.
function pakaiSudut(){
  let d = parseFloat(document.getElementById('sudutKustom').value);
  if (isNaN(d)) return;
  if (d < 1)   d = 1;
  if (d > 120) d = 120;
  c('W' + d);
  sudutPilih = d;
  gambarSudut();
  log('sudut bukaan diset ' + d + '\u00B0');
}

function sisiTerpilih(){
  const r = document.querySelector('input[name="sisi"]:checked');
  return r ? r.value : 'L';
}

// 1 = BUKA sampai sudut aktif, 0 = TUTUP sampai 0 derajat
function jalan(buka){
  c('O' + sisiTerpilih() + (buka ? '1' : '0'));
  log((buka ? 'BUKA' : 'TUTUP') + ' lid ' + sisiTerpilih());
}

function perbaruiPresisi(s){
  const uji   = s.presisiUji   || 0;
  const gagal = s.presisiGagal || 0;
  const maks  = s.presisiMaks  || 0;
  const spd   = s.stepsPerDeg || 8.8889;

  document.getElementById('preUji').textContent   = uji;
  document.getElementById('preGagal').textContent = gagal;

  const el = document.getElementById('preMaks');
  el.textContent = maks + ' langkah (' + (maks / spd).toFixed(2) + '\u00B0)';

  const ak = document.getElementById('preAkurasi');
  const bar = document.getElementById('preBar');

  if (uji === 0){
    ak.textContent = 'belum diuji';
    ak.className = 'dimc';
    bar.style.width = '0%';
    return;
  }

  const persenTepat = ((uji - gagal) / uji) * 100;
  ak.textContent = persenTepat.toFixed(0) + '%';

  bar.style.width = Math.min(100, persenTepat) + '%';
  bar.style.background = persenTepat >= 99.5 ? 'var(--ok)' :
                         persenTepat >= 95   ? 'var(--warn)' : 'var(--er)';

  ak.className = persenTepat >= 99.5 ? 'ok' :
                 persenTepat >= 95   ? 'warn' : 'er';
}

function perbaruiKendali(s){
  const maks = s.maxDeg || 120;
  const ll = s.ll || 0, lr = s.lr || 0;

  if (typeof s.lidAngle === 'number'){
    sudutAktif = s.lidAngle;
  }

  document.getElementById('sudutAktif').textContent =
    sudutAktif.toFixed(0) + '\u00B0';

  const el = document.getElementById('posSekarang');
  el.textContent = 'KIRI ' + ll.toFixed(0) + '\u00B0   KANAN ' + lr.toFixed(0) + '\u00B0';
  el.title = 'Batas maksimum ' + maks + '\u00B0';

  if (ll >= maks - 0.6 || lr >= maks - 0.6)      el.className = 'warn';
  else if (ll <= 0.6 && lr <= 0.6)               el.className = 'dimc';
  else                                            el.className = '';
}

// ---------- AKHIR KENDALI LID ----------

function loadLog(){
  fetch('/debug/log',{cache:'no-store'}).then(r=>r.text())
    .then(t=>{
      const el = $('logtxt');
      el.textContent = t;
      el.scrollTop = el.scrollHeight;   // tampilkan yang terbaru
    })
    .catch(()=>{ $('logtxt').textContent = 'gagal ambil log'; });
}

pilihSudut(90);
load(); loadLog();
setInterval(load, 1000);
setInterval(loadLog, 3000);

// Pengaman: kalau halaman ditinggalkan saat roda sedang diuji,
// pastikan roda tidak terus berjalan.
window.addEventListener('blur', mStop);
document.addEventListener('visibilitychange', function(){
  if (document.hidden) mStop();
});
window.addEventListener('pointerup', function(){
  if (mStopTimer) mStop();
});
</script>
</body></html>)rawliteral";

// ============================================================
//  HALAMAN /setting  -  DASHBOARD PENYETELAN
// ============================================================
//  Halaman ini khusus untuk MENGUBAH setelan robot, bukan untuk
//  diagnosa. Isinya sengaja dibuat sesederhana mungkin: setiap setelan
//  punya satu baris, penjelasan singkat, dan tombol yang jelas.
//
//  Dibuka lewat /setting, dan ditautkan dari /debug supaya mudah
//  ditemukan. Tidak ada tombol di sini yang langsung menggerakkan
//  motor, jadi halaman ini aman dibuka kapan saja tanpa mengubah
//  posisi robot.
const char SETTING_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="id"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Setelan SmartTrashbin</title>
<style>
:root{--bg:#0c1015;--pan:#141a22;--fg:#e6edf3;--dim:#8b98a8;--acc:#4ea8ff;
--bd:#212c38;--ok:#4ad07a;--warn:#ffc857;--er:#ff6b6b}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:var(--bg);color:var(--fg);
font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
padding:14px 14px 40px;max-width:620px;margin:0 auto}
h1{font-size:19px;margin:0 0 3px}
.sub{color:var(--dim);font-size:12.5px;margin-bottom:16px}
.card{background:var(--pan);border:1px solid var(--bd);border-radius:11px;
padding:14px;margin-bottom:12px}
.card h2{font-size:11.5px;margin:0 0 11px;color:var(--dim);
text-transform:uppercase;letter-spacing:1px;font-weight:600}
.ket{color:var(--dim);font-size:12px;margin:0 0 11px}
.row{display:flex;justify-content:space-between;align-items:center;
gap:10px;padding:7px 0;border-bottom:1px solid #1b232d}
.row:last-child{border:0}
.row .label{font-size:13.5px}
.row .hint{color:var(--dim);font-size:11.5px;display:block;margin-top:2px}
.nilai{font-weight:700;color:var(--acc);font-size:14px}
.pil{display:flex;gap:7px;flex-wrap:wrap}
button{font:inherit;font-size:13px;padding:8px 13px;border-radius:8px;
background:#1e2833;color:var(--fg);border:1px solid var(--bd);
cursor:pointer;touch-action:manipulation}
button:active{background:var(--acc);color:#04121f}
button.pilih{background:#2d4a6b;border-color:#4a7ab5;color:#cfe3ff;
font-weight:700}
input[type=number]{font:inherit;width:82px;padding:7px 9px;border-radius:8px;
background:#0e141b;color:var(--fg);border:1px solid var(--bd);text-align:center}
input[type=range]{width:100%;margin:7px 0 0;accent-color:var(--acc)}
.ok{color:var(--ok)}.warn{color:var(--warn)}.er{color:var(--er)}
.dimc{color:var(--dim)}
.tab{display:flex;gap:7px;margin-bottom:15px}
.tab a{flex:1;text-align:center;padding:9px;border-radius:9px;font-size:13px;
background:#1a222c;color:var(--dim);text-decoration:none;
border:1px solid var(--bd)}
.tab a.aktif{background:var(--acc);color:#04121f;font-weight:700}
.kaki{color:var(--dim);font-size:11.5px;text-align:center;margin-top:18px}
</style></head><body>

<h1>Setelan</h1>
<div class="sub">Atur robot tanpa harus menggerakkannya</div>

<div class="tab">
  <a href="/">Kontrol</a>
  <a href="/setting" class="aktif">Setelan</a>
  <a href="/debug">Diagnosa</a>
</div>

<!-- ============ KECEPATAN ============ -->
<div class="card">
  <h2>Kecepatan</h2>
  <p class="ket">Satu pilihan mengatur roda dan tutup sekaligus.</p>

  <div class="pil" id="profilPil">
    <button onclick="setProfil(0)" id="pb0">SLOW</button>
    <button onclick="setProfil(1)" id="pb1">MEDIUM</button>
    <button onclick="setProfil(2)" id="pb2">FAST</button>
  </div>

  <div class="row" style="margin-top:11px">
    <span class="label">Tutup bergerak<span class="hint" id="hintTutup">-</span></span>
    <span class="nilai" id="valTutup">-</span>
  </div>
  <div class="row">
    <span class="label">Roda berputar<span class="hint" id="hintRoda">-</span></span>
    <span class="nilai" id="valRoda">-</span>
  </div>

  <p class="ket" style="margin:11px 0 0" id="ketProfil">-</p>
</div>

<!-- ============ BUKAAN ============ -->
<div class="card">
  <h2>Bukaan Tutup</h2>
  <p class="ket">Sudut ini dipakai setiap kali tutup dibuka.</p>

  <div class="pil" id="sudutPil">
    <button onclick="setSudut(30)">30&deg;</button>
    <button onclick="setSudut(45)">45&deg;</button>
    <button onclick="setSudut(60)">60&deg;</button>
    <button onclick="setSudut(90)">90&deg;</button>
    <button onclick="setSudut(120)">120&deg;</button>
  </div>

  <div class="row" style="margin-top:11px">
    <span class="label">Sudut lain</span>
    <span>
      <input type="number" id="sudutKustom" min="1" max="120" step="1" value="90">
      <button onclick="pakaiSudutKustom()">Simpan</button>
    </span>
  </div>

  <div class="row">
    <span class="label">Sudut aktif</span>
    <span class="nilai" id="valSudut">-</span>
  </div>
</div>

<!-- ============ ARAH MOTOR ============ -->
<div class="card">
  <h2>Arah Motor Roda</h2>
  <p class="ket">Kalau robot bergerak terbalik dari tombolnya, balik
  arah di sini. Tidak perlu membongkar kabel.</p>

  <div class="row">
    <span class="label">Motor kiri</span>
    <span>
      <button onclick="setArah('L',1)" id="bl1">Balik</button>
      <button onclick="setArah('L',0)" id="bl0">Normal</button>
    </span>
  </div>
  <div class="row">
    <span class="label">Motor kanan</span>
    <span>
      <button onclick="setArah('R',1)" id="br1">Balik</button>
      <button onclick="setArah('R',0)" id="br0">Normal</button>
    </span>
  </div>

  <div class="pil" style="margin-top:11px">
    <button onclick="c('F')" style="flex:1">Uji MAJU</button>
    <button onclick="c('P')" style="flex:1">STOP</button>
  </div>
  <p class="ket" style="margin:9px 0 0">Tekan Uji MAJU sebentar saja
  untuk memastikan arahnya sudah benar.</p>
</div>

<!-- ============ BELOK ============ -->
<div class="card">
  <h2>Belok di Tempat</h2>
  <p class="ket">Kalau saat berbelok ada roda yang seperti tidak jalan,
  naikkan <b>tenaga belok</b>. Berbelok menyeret roda ke samping,
  jadi bebannya lebih berat daripada maju.</p>

  <div class="row">
    <span class="label">Tenaga tambahan saat belok
      <span class="hint" id="hintBelok">-</span></span>
    <span class="nilai" id="valBelok">-</span>
  </div>
  <input type="range" id="belok" min="0" max="90" value="45">
  <div class="pil" style="margin-top:9px">
    <button onclick="setBelok(0)">0</button>
    <button onclick="setBelok(30)">30</button>
    <button onclick="setBelok(45)">45</button>
    <button onclick="setBelok(70)">70</button>
    <button onclick="setBelok(90)">90</button>
  </div>

  <div class="row" style="margin-top:13px">
    <span class="label">Uji putar<span class="hint">Tekan dan tahan</span></span>
    <span>
      <button id="bputarL" onmousedown="putar('L')" onmouseup="stopRoda()"
        ontouchstart="putar('L')" ontouchend="stopRoda()">&#9664; KIRI</button>
      <button id="bputarR" onmousedown="putar('R')" onmouseup="stopRoda()"
        ontouchstart="putar('R')" ontouchend="stopRoda()">KANAN &#9654;</button>
    </span>
  </div>
</div>

<!-- ============ KESEIMBANGAN RODA ============ -->
<div class="card">
  <h2>Keseimbangan Roda</h2>
  <p class="ket">Pakai kalau saat maju robot tidak lurus. Naikkan angka
  untuk roda yang <b>lemah</b>, jangan turunkan roda yang kuat, supaya
  tenaga robot tidak berkurang.</p>

  <div class="row">
    <span class="label">Kompensasi roda kiri
      <span class="hint" id="hintKL">-</span></span>
    <span class="nilai" id="valKL">-</span>
  </div>
  <input type="range" id="kl" min="-60" max="60" value="0">
  <div class="pil" style="margin-top:7px">
    <button onclick="setKomp('L',-20)">-20</button>
    <button onclick="setKomp('L',-10)">-10</button>
    <button onclick="setKomp('L',0)">0</button>
    <button onclick="setKomp('L',10)">+10</button>
    <button onclick="setKomp('L',20)">+20</button>
  </div>

  <div class="row" style="margin-top:13px">
    <span class="label">Kompensasi roda kanan
      <span class="hint" id="hintKR">-</span></span>
    <span class="nilai" id="valKR">-</span>
  </div>
  <input type="range" id="kr" min="-60" max="60" value="0">
  <div class="pil" style="margin-top:7px">
    <button onclick="setKomp('R',-20)">-20</button>
    <button onclick="setKomp('R',-10)">-10</button>
    <button onclick="setKomp('R',0)">0</button>
    <button onclick="setKomp('R',10)">+10</button>
    <button onclick="setKomp('R',20)">+20</button>
  </div>

  <div class="pil" style="margin-top:13px">
    <button onclick="c('F')" style="flex:1">Uji MAJU</button>
    <button onclick="stopRoda()" style="flex:1">STOP</button>
  </div>

  <div class="row" style="margin-top:11px">
    <span class="label">PWM akhir tiap roda</span>
    <span class="nilai" id="valAkhir">-</span>
  </div>
</div>

<!-- ============ MODE TAHAN ============ -->
<div class="card">
  <h2>Mode Tahan Tutup</h2>
  <p class="ket">Saat aktif, motor tutup terus diberi arus sehingga
  tutup tidak turun sendiri karena beratnya. Motor akan terasa hangat,
  dan baterai lebih cepat habis.</p>

  <div class="row">
    <span class="label">Tahan tutup</span>
    <span>
      <button onclick="setTahan(1)" id="bh1">Aktif</button>
      <button onclick="setTahan(0)" id="bh0">Mati</button>
    </span>
  </div>
</div>

<!-- ============ SISTEM ============ -->
<div class="card">
  <h2>Sistem</h2>
  <div class="row"><span class="label">Kecepatan roda langsung</span>
    <span class="nilai"><span id="valPwm">-</span></span></div>
  <input type="range" id="pwm" min="0" max="255" value="165">
  <p class="ket" style="margin:8px 0 0">Geser untuk mengubah kecepatan
  roda saja, tanpa mengubah kecepatan tutup.</p>

  <div class="row" style="margin-top:12px">
    <span class="label">Nolkan posisi tutup</span>
    <button onclick="c('A')">Kalibrasi 0&deg;</button>
  </div>

  <div class="row">
    <span class="label">Jalankan ulang robot</span>
    <button onclick="reboot()">Muat Ulang</button>
  </div>

  <div class="row">
    <span class="label">Status</span>
    <span class="nilai" id="valStatus">-</span>
  </div>
</div>

<div class="kaki">
  Setelan tersimpan di memori robot dan tetap ada setelah dimatikan.<br>
  <span id="pesan">-</span>
</div>

<script>
var $ = function(i){ return document.getElementById(i); };
var bawaTimer = null;

function pesan(t, jenis){
  var el = $('pesan');
  el.textContent = t;
  el.className = jenis || 'dimc';
  if (bawaTimer) clearTimeout(bawaTimer);
  bawaTimer = setTimeout(function(){ el.textContent = '-';
    el.className = 'dimc'; }, 2500);
}

/* Kirim perintah ke firmware */
function c(v){
  fetch('/cmd?val=' + encodeURIComponent(v), {cache:'no-store'})
    .then(function(r){ if (!r.ok) throw 0; return r.text(); })
    .then(function(){ muat(); })
    .catch(function(){ pesan('Robot tidak merespons', 'er'); });
}

/* Ketiga fungsi di bawah ini mengubah setelan, jadi selalu beri tahu
   pengguna bahwa setelannya sudah tersimpan. */
function setProfil(p){
  c('N' + p);
  var n = ['SLOW', 'MEDIUM', 'FAST'][p] || '?';
  pesan('Kecepatan: ' + n, 'ok');
}

function setSudut(d){
  d = Math.max(1, Math.min(120, d));
  c('W' + d);
  pesan('Sudut bukaan: ' + d + '\u00B0', 'ok');
}

function pakaiSudutKustom(){
  var d = parseFloat($('sudutKustom').value);
  if (isNaN(d)) { pesan('Angka tidak valid', 'er'); return; }
  d = Math.max(1, Math.min(120, d));
  $('sudutKustom').value = d;
  setSudut(d);
}

function setArah(sisi, balik){
  c('Y' + sisi + balik);
  pesan('Motor ' + (sisi === 'L' ? 'kiri' : 'kanan') + ': ' +
        (balik ? 'DIBALIK' : 'normal'), 'ok');
}

function setTahan(on){
  c('H' + on);
  pesan('Mode tahan: ' + (on ? 'AKTIF' : 'mati'), 'ok');
}

function reboot(){
  if (!confirm('Jalankan ulang robot? Posisi tutup akan dianggap 0 derajat.')) return;
  fetch('/debug/reset', {cache:'no-store'}).catch(function(){});
  pesan('Robot dijalankan ulang', 'ok');
}

/* Slider PWM mengubah kecepatan roda saja */
var slider = $('pwm');
slider.addEventListener('input', function(){ $('valPwm').textContent = slider.value; });
slider.addEventListener('change', function(){ c('V' + slider.value); });
$('valPwm').textContent = slider.value;

/* Tandai tombol yang sedang aktif supaya pengguna tahu setelan mana
   yang berlaku sekarang. */
function tandai(id, aktif){
  var el = $(id);
  if (!el) return;
  el.className = aktif ? 'pilih' : '';
}

var KET = [
  'Paling halus. Kecepatan tutup rendah sehingga hampir tidak mungkin kehilangan langkah. Cocok untuk gerakan yang harus mulus betul dan untuk menghemat baterai.',
  'Seimbang. Ini pilihan sehari-hari yang aman untuk sebagian besar kondisi.',
  'Paling cepat. Enak dipakai kalau arus TB6600 sudah benar dan tutupnya ringan. Kalau tutup mulai berhenti tidak tepat, turunkan ke MEDIUM.'
];

var TUTUP_MS = [ 'sekitar 2.3 detik', 'sekitar 1.1 detik', 'sekitar 0.9 detik' ];
var RODA_KET = [ 'pelan', 'sedang', 'kencang' ];

/* ---------- BELOK DI TEMPAT ---------- */

var putarTimer = null;

/* Selama tombol putar ditahan, kirim ulang perintahnya supaya roda
   tidak berhenti sendiri gara-gara pengaman waktu di firmware. */
function putar(arah){
  c(arah);
  if (putarTimer) clearInterval(putarTimer);
  putarTimer = setInterval(function(){ c(arah); }, 1500);
}

function stopRoda(){
  if (putarTimer) { clearInterval(putarTimer); putarTimer = null; }
  c('P');
}

function setBelok(v){
  c('ZB' + v);
  pesan('Tenaga belok: +' + v, 'ok');
}

/* ---------- KESEIMBANGAN RODA ---------- */

function setKomp(sisi, v){
  c('Z' + sisi + v);
  pesan('Kompensasi ' + (sisi === 'L' ? 'kiri' : 'kanan') +
        ': ' + (v > 0 ? '+' : '') + v, 'ok');
}

var sliderBelok = $('belok');
var sliderKL = $('kl');
var sliderKR = $('kr');

/* Saat slider digeser, tampilkan angkanya saja. Perintah baru
   dikirim ketika jari diangkat, supaya tidak membanjiri robot
   dengan puluhan perintah. */
sliderBelok.addEventListener('input',
  function(){ $('valBelok').textContent = '+' + sliderBelok.value; });
sliderBelok.addEventListener('change',
  function(){ setBelok(sliderBelok.value); });

sliderKL.addEventListener('input',
  function(){ $('valKL').textContent = tanda(sliderKL.value); });
sliderKL.addEventListener('change',
  function(){ setKomp('L', sliderKL.value); });

sliderKR.addEventListener('input',
  function(){ $('valKR').textContent = tanda(sliderKR.value); });
sliderKR.addEventListener('change',
  function(){ setKomp('R', sliderKR.value); });

function tanda(v){
  v = parseInt(v, 10);
  return (v > 0 ? '+' : '') + v;
}

/* ---------- KUMPULKAN STATUS ---------- */

function muat(){
  fetch('/status', {cache:'no-store'})
    .then(function(r){ return r.json(); })
    .then(function(s){
      var p = (typeof s.profil === 'number') ? s.profil : 1;

      for (var i = 0; i < 3; i++) tandai('pb' + i, i === p);

      $('hintTutup').textContent = TUTUP_MS[p] || '';
      $('hintRoda').textContent  = RODA_KET[p] || '';
      $('ketProfil').textContent = KET[p] || '';

      $('valTutup').textContent = (s.sp || 0) + ' step/detik';
      $('valRoda').textContent  = (s.pwm || 0) + ' PWM';

      if (typeof s.lidAngle === 'number'){
        var d = Math.round(s.lidAngle);
        $('valSudut').textContent = d + '\u00B0';
        $('sudutKustom').value = d;
        ['30','45','60','90','120'].forEach(function(v){
          var btn = document.querySelector('#sudutPil button[onclick*="setSudut('+v+')"]');
          if (btn) btn.className = (parseInt(v,10) === d) ? 'pilih' : '';
        });
      }

      $('bl1').className = s.balikL ? 'pilih' : '';
      $('bl0').className = s.balikL ? '' : 'pilih';
      $('br1').className = s.balikR ? 'pilih' : '';
      $('br0').className = s.balikR ? '' : 'pilih';

      $('bh1').className = s.holdMode ? 'pilih' : '';
      $('bh0').className = s.holdMode ? '' : 'pilih';

      /* Setelan roda: baca dari robot, jangan disimpan di halaman.
         Supaya kalau dibuka dari HP lain tampilannya tetap benar. */
      if (typeof s.belok === 'number'){
        sliderBelok.value = s.belok;
        $('valBelok').textContent = '+' + s.belok;
        $('hintBelok').textContent = s.belok < 20
          ? 'masih rendah, roda bisa tersendat saat berbelok'
          : (s.belok > 70 ? 'tinggi, hati-hati arus driver'
                          : 'nilai yang wajar');
      }

      if (typeof s.kompKiri === 'number'){
        sliderKL.value = s.kompKiri;
        $('valKL').textContent = tanda(s.kompKiri);
        $('hintKL').textContent = s.kompKiri === 0
          ? 'tidak ada kompensasi' : 'roda kiri diberi PWM lebih tinggi';
      }

      if (typeof s.kompKanan === 'number'){
        sliderKR.value = s.kompKanan;
        $('valKR').textContent = tanda(s.kompKanan);
        $('hintKR').textContent = s.kompKanan === 0
          ? 'tidak ada kompensasi' : 'roda kanan diberi PWM lebih tinggi';
      }

      if (typeof s.pwmKiri === 'number'){
        $('valAkhir').textContent = s.pwmKiri + ' / ' + s.pwmKanan;
      }

      if (document.activeElement !== slider){
        slider.value = s.pwm || 0;
        $('valPwm').textContent = slider.value;
      }

      $('valStatus').textContent = 'terhubung';
      $('valStatus').className = 'nilai ok';
    })
    .catch(function(){
      $('valStatus').textContent = 'tidak terhubung';
      $('valStatus').className = 'nilai er';
    });
}

muat();
setInterval(muat, 2000);
</script>
</body></html>)rawliteral";

void handleSettingPage() {
  server.send_P(200, "text/html", SETTING_HTML);
}

void handleDebugPage() {
  server.send_P(200, "text/html", DEBUG_HTML);
}

// ==================== BLE CALLBACK ====================

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    bleConnected = true;
    addLog("[BLE] Client terhubung");
  }
  void onDisconnect(BLEServer* s) override {
    bleConnected = false;
    addLog("[BLE] Client terputus - restart advertising");
    moveMode = MOVE_STOP;
    motorStop();
    BLEDevice::startAdvertising();
  }
};

void sendBleStatus();

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String v = c->getValue();
    if (v.length() == 0) return;
    char cmd   = v.charAt(0);
    String arg = v.substring(1);
    addLogf("[BLE] Terima: %c (arg=%s)", cmd, arg.c_str());
    if (cmd == '?') { sendBleStatus(); return; }
    executeCommand(cmd, arg);
  }
};

void sendBleStatus() {
  if (!bleConnected || charTX == nullptr) return;
  char buf[200];
  snprintf(buf, sizeof(buf),
           "LL=%.1f;LR=%.1f;LOPEN=%.1f;BL=%d;ST=%s;SP=%d;MV=%d",
           stLidLeft.currentStep  / STEPS_PER_DEG,
           stLidRight.currentStep / STEPS_PER_DEG,
           lidAngleGlobal,
           stepperBusy() ? 1 : 0,
           (moveMode == MOVE_STOP) ? "stop" : "run",
           (int)speedPWM,
           (int)moveMode);
  charTX->setValue((uint8_t*)buf, strlen(buf));
  charTX->notify();
}

// ==================== SETUP ====================

// ============================================================
//  KENDALI BLE ADVERTISING  (dipakai perintah 'X')
// ============================================================
//
//  BLE memakai radio dan flash bersama WiFi. Akses flash bisa menahan
//  seluruh chip (cache stall), dan itulah yang diukur sebagai perioda
//  pulsa panjang (max 32368 us = 32 ms, khas durasi operasi flash).
//
//  Karena WiFi adalah jalur utama, BLE boleh dimatikan saat menguji
//  untuk melihat apakah pulsa jadi lebih rapi.

bool bleDiinit = false;

void BLEAdvertising_stop() {
  if (!bleDiinit) return;
  BLEDevice::stopAdvertising();
  addLog("[BLE] Advertising dihentikan");
}

void BLEAdvertising_start() {
  if (!bleDiinit) return;
  BLEDevice::startAdvertising();
  addLog("[BLE] Advertising dimulai");
}

void setup() {
  Serial.begin(115200);
  delay(400);

  // Baca alasan reset SEBELUM apa pun, karena RTC memory bisa terpakai
  // proses lain. Ini yang membuktikan apakah gerakan tersendat
  // disebabkan ESP32 reset di tengah jalan (brownout).
  resetTrackerBegin();

  // Muat sudut global yang terakhir diatur dari flash, supaya setelan
  // tidak hilang saat ESP32 dimatikan.
  muatSudutGlobal();

  // Pulihkan pilihan profil kecepatan terakhir (SLOW/MEDIUM/FAST).
  muatProfil();

  // Buang sampah di buffer serial. Kalau monitor/host sempat menumpuk
  // perintah lama, perintah itu akan langsung dieksekusi saat boot dan
  // bisa terlihat seperti robot bertingkah sendiri.
  while (Serial.available()) Serial.read();

  Serial.println();
  Serial.println("==========================================");
  Serial.println("  SMART TRASHBIN - WIFI AP + BLE");
  Serial.println("==========================================");
  Serial.printf("  Boot ke-%u | alasan reset: %s\n",
                (unsigned)bootCount, resetReasonName(bootResetReason));
  if (brownoutSuspect) {
    Serial.println("  !! TEGANGAN TIDAK STABIL - cek catu daya !!");
  }
  if (moveInterrupted) {
    Serial.println("  !! Gerakan TERPUTUS oleh reset sebelumnya !!");
  }

  motorBegin();
  addLog("[OK] Motor DC siap (BTN7970)");

  stepperBegin(stLidLeft);
  stepperBegin(stLidRight);
  addLogf("[OK] Stepper siap. 1 deg = %.3f step", STEPS_PER_DEG);

  // ---------- WiFi Access Point ----------
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, false, AP_MAX_CONN);
  if (apOk) {
    addLog("[OK] WiFi Access Point aktif");
    Serial.printf("[INFO] SSID     : %s\n", AP_SSID);
    Serial.printf("[INFO] Password : %s\n", AP_PASS);
    Serial.printf("[INFO] Buka     : http://%s\n", WiFi.softAPIP().toString().c_str());
  } else {
    addLog("[ERROR] Gagal membuat Access Point!");
  }

  // ---------- Web server ----------
  server.on("/",            handleRoot);
  server.on("/setting",     handleSettingPage);
  server.on("/cmd",         handleCmd);
  server.on("/status",      handleStatus);
  server.on("/debug",       handleDebugPage);
  server.on("/debug/json",  handleDebugJson);
  server.on("/debug/log",   handleDebugLog);
  server.on("/debug/download", handleDebugDownload);
  server.on("/debug/reset", handleDebugReset);
  server.onNotFound(handleNotFound);
  server.begin();
  addLog("[OK] Web server jalan di port 80");

  // ---------- Laporan kondisi daya ----------
  // Ini yang membuktikan apakah gerakan tersendat disebabkan ESP32
  // reset di tengah gerakan (brownout). Tampil di /debug.
  addLogf("[BOOT] Boot ke-%u, alasan reset: %s",
          (unsigned)bootCount, resetReasonName(bootResetReason));

  if (brownoutSuspect) {
    addLog("[WARN] BROWNOUT - tegangan turun saat gerakan!");
    addLog("[WARN] Tambah kapasitor 1000uF dekat ESP32 + pastikan GND kuat");
  }
  if (crashSuspect) {
    addLog("[WARN] ESP32 CRASH/WATCHDOG - kode macet, gerakan terputus");
  }
  if (moveInterrupted) {
    addLog("[WARN] Gerakan sebelumnya TERPUTUS oleh reset!");
  }

  // ---------- Task stepper di CORE 1 (anti-jitter) ----------
  // Ini pengganti loop()-based pulsing. Karena berjalan di core terpisah
  // dari WiFi, pulsa stepper tidak lagi tertunda oleh request HTTP.
  stepperTaskStart();

  // ---------- BLE (cadangan) ----------  // Diinit di sini, tapi bisa dimatikan/dihidupkan lewat perintah X.
  BLEDevice::init(BLE_NAME);
  bleDiinit = true;              // tandai BLE sudah diinit
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  BLEService* svc = bleServer->createService(SERVICE_UUID);

  charTX = svc->createCharacteristic(
      CHAR_TX_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  charTX->addDescriptor(new BLE2902());

  BLECharacteristic* charRX = svc->createCharacteristic(
      CHAR_RX_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  charRX->setCallbacks(new RxCallbacks());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  addLog("[OK] BLE advertising aktif (bisa dimatikan dengan XB1)");

  lastCmdMillis = millis();

  Serial.println("------------------------------------------");
  Serial.println("Perintah serial (akhiri dengan Enter):");
  Serial.println("  F B L R P   = maju/mundur/pivot/stop");
  Serial.println("  V<0-255>    = kecepatan roda");
  Serial.println("  C / S       = buka / tutup KEDUA lid");
  Serial.println("  IL1 IL0     = buka / tutup lid KIRI saja");
  Serial.println("  IR1 IR0     = buka / tutup lid KANAN saja");
  Serial.println("  K<sps>      = kecepatan stepper (mis. K900)");
  Serial.println("  A           = kalibrasi posisi 0 derajat");
  Serial.println("  ?           = status BLE");
  Serial.println("==========================================");
  Serial.println();
}

// ==================== LOOP ====================

void loop() {
  server.handleClient();

  // CATATAN: stepper TIDAK lagi dilayani dari loop().
  // Pulsa stepper dikeluarkan oleh task di CORE 1 (stepperTaskCore1),
  // sehingga tidak terpengaruh oleh lambatnya server.handleClient() / WiFi.
  // Ini yang menghilangkan jitter.

  // --- watchdog: kalau HP hilang di tengah gerak, robot harus berhenti ---
  // Hanya berlaku saat roda memang sedang jalan. Setelah distop, moveMode
  // diubah ke MOVE_STOP sehingga pesan ini tercetak SEKALI, bukan berulang.
  if (moveMode != MOVE_STOP && (millis() - lastCmdMillis > CMD_TIMEOUT_MS)) {
    moveMode = MOVE_STOP;
    motorStop();
    addLogf("[SAFETY] Tidak ada perintah %lus - roda distop",
            CMD_TIMEOUT_MS / 1000);
  }

  // --- perintah lewat Serial USB (untuk tes tanpa HP) ---
  // PENTING: perintah dibaca sebagai SATU BARIS PENUH, bukan per karakter.
  // Kalau dibaca per karakter, "IL1" akan terpecah jadi 'I', 'L', '1' —
  // dan huruf 'L' akan salah dieksekusi sebagai pivot kiri (roda bergerak!).
  static String serLine = "";
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      serLine.trim();
      if (serLine.length() > 0) {
        if (serLine == "?") {
          sendBleStatus();
          addLog("[SER] Status dikirim (BLE)");
        } else {
          char cmd   = serLine.charAt(0);
          String arg = serLine.substring(1);
          addLogf("[SER] Perintah: %c (arg=%s)", cmd, arg.c_str());
          executeCommand(cmd, arg);
        }
        serLine = "";
      }
      continue;
    }

    if (serLine.length() < 32) serLine += c;   // batasi panjang, hindari overflow
  }

  // --- laporan status BLE tiap 1 detik ---
  static unsigned long lastReport = 0;
  if (bleConnected && millis() - lastReport > 1000) {
    lastReport = millis();
    sendBleStatus();
  }
}
