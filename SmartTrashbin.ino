/*
 * SMART TRASHBIN — ESP32 WiFi AP + BLE. DC motor (wheels) + 2x TB6600 stepper (lid).
 * Control paths (either from phone):
 *  1. WiFi AP (primary): SSID SmartTrashbin / pass 12345678 / http://192.168.4.1
 *     UI embedded in firmware, no files needed.
 *  2. BLE (backup): device SmartTrashbin + controller-ble.html on a laptop.
 * HARDWARE (verified): ESP32 DevKit V4 32D (D0WD-V3 rev 3.1), COM6 via CP2102
 *   DC motor BTN7970: L PWM=25 D1=27 D2=26 | R PWM=14 D1=13 D2=12
 *   Stepper lid TB6600 (microstep 8 = 1600 pulse/rev, 2.0 A):
 *     L PUL=18 DIR=19 ENA=23 | R PUL=15 DIR=4 ENA=5
 * GPIO15 is a strapping pin: if TB6600 holds it HIGH at boot the board can fail
 * to start, so this code leaves it alone until setup() ends. Board will not
 * boot -> unplug right lid PUL, boot, reconnect.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <esp_timer.h>     // HW timer for stepper pulses (anti-jitter)
#include <esp_system.h>    // esp_reset_reason() - reset/brownout tracking
#include <Preferences.h>   // persist global lid angle to flash (NVS)
#include <stdarg.h>        // va_list for addLogf()

// Generated pages, all produced by konversi-html.py. Edit the matching .html
// source, never these headers.
#include "web_ui.h"       // INDEX_HTML,   control panel at /
#include "setting_ui.h"   // SETTING_HTML, settings page at /setting
#include "debug_ui.h"     // DEBUG_HTML,   diagnostics page at /debug

// ==================== WIFI CONFIG ====================

const char* AP_SSID = "SmartTrashbin";
const char* AP_PASS = "12345678";        // min 8 chars
const byte  AP_CHANNEL = 6;
const byte  AP_MAX_CONN = 4;

WebServer server(80);

// ==================== STATE (defined first) ====================
// Stepper must precede all functions: Arduino IDE auto-inserts prototypes near
// the top, so a struct declared after the stepper functions makes
// `void stepperBegin(Stepper&)` land before Stepper is known ->
// "'Stepper' was not declared in this scope".

enum MoveMode { MOVE_STOP, MOVE_FWD, MOVE_BACK, MOVE_LEFT, MOVE_RIGHT };
volatile MoveMode moveMode = MOVE_STOP;

unsigned long lastCmdMillis = 0;              // watchdog: time of last command

// Wheel stop timeout with no command: safety so the robot does not keep
// driving if the phone dies or WiFi drops. Deliberately loose: at 2500 ms it
// felt like "robot stops by itself while the button is held" (one late
// heartbeat triggered a stop). 8000 ms tolerates late heartbeats yet still
// stops within seconds if the phone is really gone.
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
  int     warmUp;            // leading pulses skipped when measuring jitter
  float   speedCurrent;      // current speed (steps/s), for ramp
  float   speedTarget;       // target speed (steps/s)
  bool    pulseHigh;
  bool    enabled;
};

// ==================== WIRING ====================

// --- DC motor (BTN7970) ---
const uint8_t M_LEFT_PWM  = 25;
const uint8_t M_LEFT_D1   = 27;
const uint8_t M_LEFT_D2   = 26;
const uint8_t M_RIGHT_PWM = 14;
const uint8_t M_RIGHT_D1  = 13;
const uint8_t M_RIGHT_D2  = 12;

// --- Stepper lid (TB6600) ---
const uint8_t LID_L_PUL = 18;
const uint8_t LID_L_DIR = 19;
const uint8_t LID_L_ENA = 23;
const uint8_t LID_R_PUL = 15;
const uint8_t LID_R_DIR = 4;
const uint8_t LID_R_ENA = 5;

// ==================== LOG RING (for /debug without a laptop) ====================
// Last 40 messages in RAM, readable from the phone at http://192.168.4.1/debug.
// Important Serial.println output also feeds it via addLog().

#define LOG_LINES   60          // 40 too few, 120 eats RAM
#define LOG_LEN     100

char     logRing[LOG_LINES][LOG_LEN];
uint8_t  logHead  = 0;
uint16_t logTotal = 0;
uint32_t logDrop  = 0;      // messages lost because the ring was full

void addLog(const char* msg) {
  // Also to Serial (if connected) and into the ring buffer.
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

// ==================== STEPPER CONSTANTS ====================
// The TB6600's microstep is set by PHYSICAL DIP switches (SW1/SW2/SW3); firmware
// cannot change it. What these variables hold is the microstep the firmware
// ASSUMES, so it can be told which DIP setting is in use without a recompile.
//   pulses/rev = 200 x divisor ; stepsPerDeg = pulses/rev / 360
//   1/1 200 | 1/2 400 | 1/4 800 | 1/8 1600 <-- default | 1/16 3200
// Device-verified: SW1/2/3 = OFF/ON/OFF -> 1/8; SW4/5/6 = ON/OFF/OFF -> 2.0 A;
// motor supply 24 V.
//
// If the assumed microstep does not match the DIP switches, every angle is
// silently wrong: firmware reports "90 deg" while the lid moves 45 or 180.
//
// Why 1/8 is the default, and why the old "1/8 stops the stepper" note was
// wrong: the motor was SILENT, and a stalled stepper always buzzes or vibrates -
// silence means no coil current, i.e. a current-path fault (cable, supply, ENA)
// or a constant mismatch, NOT the microstep level. Real cause: the DIP was at
// 1/8 while the firmware constant still said 1/16, so every angle was wrong.
//
// Torque per step by microstep (share of holding torque, Faulhaber App Note 015):
//   1/1 100%  |  1/2 70.7%  |  1/4 38.3%  |  1/8 19.5%  |  1/16 9.8%
// Detent torque alone is 5-20%, so on a heavy lid 1/16 barely moves at all.
// Lower microstep = more torque per step = a heavier lid moves reliably.
// Higher microstep = finer resolution = smoother and quieter, but weaker.
//
// Changed at runtime by the 'M' command or the setting page. Saved to flash.
float stepsPerRev = 1600.0;
float stepsPerDeg = stepsPerRev / 360.0;   // 4.4444 at 1/8

// Assumed microstep divisor, as the number people see on the DIP table (1,2,4,
// 8,16). Kept in step with stepsPerRev by setMicrostep(); the two must never
// disagree or the angles above break.
uint8_t microstepDiv = 8;

// The divisors the firmware accepts. 1/32 is deliberately absent: on a real
// Toshiba TB6600HG that DIP combination is marked NC in the datasheet, so
// supporting it would invite an undefined mode.
const uint8_t MICROSTEP_DIVS[]   = { 1, 2, 4, 8, 16 };
const uint8_t MICROSTEP_JUMLAH   = sizeof(MICROSTEP_DIVS) / sizeof(MICROSTEP_DIVS[0]);

bool microstepDidukung(uint8_t div) {
  for (uint8_t i = 0; i < MICROSTEP_JUMLAH; i++) {
    if (MICROSTEP_DIVS[i] == div) return true;
  }
  return false;
}

// ------------------------------------------------------------
//  GLOBAL LID ANGLE
// ------------------------------------------------------------
// Opening angle for ALL open/close commands (C/S/I, O, /debug buttons). 45 deg
// means OPEN moves 45 and CLOSE returns to 0.
const float LID_ANGLE_MIN = 1.0;
const float LID_ANGLE_MAX = 120.0;    // mechanical safety limit
const float LID_ANGLE_DEF = 90.0;     // used if never configured

float lidAngleGlobal = LID_ANGLE_DEF;

// Max lid position = max settable angle (never exceeds the mechanical limit).
const float LID_MAX_DEG = LID_ANGLE_MAX;

// ------------------------------------------------------------
//  STEPPER PULSE SPEED (steps per second)
// ------------------------------------------------------------
// VIBRATION/buzz without turning = stalled. Causes: PUL level too low (TB6600
// wants ~5V, not 3.3V), speed too high for the driver current, or TB6600 current
// too low. Lower this first.
//   safe 400-800 | medium 800-1200 | fast 1200-2000 (needs a real 5V signal)
// 1600 from live testing: max jitter 2-4 us at every speed (K300..K2000), lid
// stopped exactly at 90 deg (~0.94 s; K600: 1.23 s). Tested WITHOUT lid load -
// with weight/friction lower it via /debug (K900 / K600). Stopping short of
// 90 deg = lost steps, lower the speed.
// ------------------------------------------------------------
//  SPEED PROFILES: SLOW / MEDIUM / FAST
// ------------------------------------------------------------
// One button sets BOTH wheel PWM and lid speed (steps/s), so "slow" means slow
// robot-wide. SLOW = smoothest, lid speed far lower so step loss is impossible;
// MEDIUM = daily; FAST = riskiest if current is low or load heavy.
// Lid speed caps at 900 steps/s: above that testing showed step loss even under
// light load. Wheels may be faster - a DC motor has no sync problem.

#define PROFIL_SLOW    0
#define PROFIL_MEDIUM  1
#define PROFIL_FAST    2

// ------------------------------------------------------------
//  WHEEL-ONLY SPEED PROFILES
// ------------------------------------------------------------
// IMPORTANT: drive motors only. The lid stepper has its own speed - wheels
// (floor driving = comfort) and lid (heavy iris gear train = torque/precision)
// are separate systems, so coupling them means changing one breaks the other.
// Stepper speed is therefore set SEPARATELY and ignores SLOW/MEDIUM/FAST.
//           name        wheel PWM
struct ProfilKecepatan {
  const char* nama;
  uint8_t     pwmRoda;
};

const ProfilKecepatan PROFIL[3] = {
  { "SLOW",     45 },
  { "MEDIUM",  110 },
  { "FAST",    200 }
};

uint8_t profilAktif = PROFIL_MEDIUM;

// ------------------------------------------------------------
//  STEPPER SPEED (INDEPENDENT OF WHEEL PROFILE)
// ------------------------------------------------------------
// Ignores SLOW/MEDIUM/FAST (wheel-only). Set here or via the K command; saved to
// flash. 450 steps/s at 1/8: 1600 pulses = one rev, so ~0.28 rev/s, and a 90 deg
// move (400 steps) takes ~0.9 s.
// WHY NOT 600: 600 was the value at microstep 1/16. At 1/8 pulses per degree
// HALVE, so 600 moves the lid TWICE as fast - the easiest way to lose steps on a
// heavy iris lid. 450 matches the tested MEDIUM number at 1/8 (HARDWARE.md
// section 8).
#define STEP_DEFAULT_SPS   450.0f

// Active stepper speed. Set by the K command, saved to flash across power off.
float stepSpeedActive = STEP_DEFAULT_SPS;

// ============================================================
//  SIGNAL POLARITY TO TB6600  *** READ BEFORE CHANGING ***
// ============================================================
// TB6600 has OPTOCOUPLERS on PUL/DIR/ENA: 10-15 mA must flow through the LED.
// Two wirings; firmware polarity MUST match.
// 1) COMMON-ANODE (5V) <-- CORRECT, in use
//      X+ -> 5V ; X- -> GPIO ; buck GND -> ESP32 GND (required)
//    GPIO LOW = current flows = ACTIVE, so pulse = LOW then HIGH -> PULSE_ACTIVE
//    LOW. Current comes from the 5V rail, not the pin, so the optocoupler gets
//    the full 10-15 mA and the motor runs smooth.
// 2) COMMON-CATHODE (3.3V) <-- CAUSES VIBRATION
//      X- -> GND ; X+ -> GPIO
//    Current = (3.3V - 1.2V) / 220 ohm = only ~9.5 mA, and it sags because the
//    ESP32 GPIO is unstable at high current, so the optocoupler sometimes fails
//    to fire -> motor turns but STUTTERS (the reported problem).
// Switching: common-anode -> LOW (default); common-cathode -> HIGH.
// Always recompile+upload after changing; never mix, or the motor moves at the
// wrong time.
// ============================================================

#define PULSE_ACTIVE   LOW    // common-anode 5V   (default)
// #define PULSE_ACTIVE   HIGH  // common-cathode 3.3V (not recommended)

// Polarity derivatives, so no raw values are hard-coded elsewhere.
#define PULSE_IDLE     (PULSE_ACTIVE == LOW  ? HIGH : LOW)
#define PULSE_ON       (PULSE_ACTIVE)
#define PULSE_OFF      (PULSE_IDLE)

// DIR: common-anode wiring inverts direction vs common-cathode.
#define DIR_FOR_POS    (PULSE_ACTIVE == LOW ? LOW  : HIGH)
#define DIR_FOR_NEG    (PULSE_ACTIVE == LOW ? HIGH : LOW)

// ENA: motor is FREE (not holding) when no current flows through ENA.
#define ENA_ACTIVE     (PULSE_ACTIVE)      // current flows = motor LOCKED
#define ENA_INACTIVE   (PULSE_ACTIVE == LOW ? HIGH : LOW)  // motor FREE

// ------------------------------------------------------------
//  HOLD MODE  *** IMPORTANT FOR DIAGNOSIS ***
// ------------------------------------------------------------
// Normally the motor is RELEASED after a move (ENA off = free): saves power and
// runs cool. But a FREE motor can turn by itself from lid load (gravity),
// vibration, or residual magnetism - the symptom "motor keeps turning after
// firmware stopped". HOLD_MODE true keeps it LOCKED; if that symptom DISAPPEARS
// with it true, the cause is free motor + load, not the signal.
// Runtime switch 'H' (no re-upload):  H1 -> lock | H0 -> release (default)
// ------------------------------------------------------------
bool holdMode = false;

// How long the motor stays locked after a move (ms). 0 = until the next command.
#define HOLD_TIMEOUT_MS  0

// ==================== MOTOR CONSTANTS ====================

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
// (struct Stepper is defined above, near WebServer)

// Built here because they need the LID_* pin constants. Designated
// initializers (.field = ...) so field order cannot silently break if the
// struct changes again.
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


// ==================== DC MOTOR ====================

// Each wheel gets its own PWM value. DC motors do not start at the same PWM:
// one may spin from 70 while its partner needs 100. At low PWM that looks like
// "one motor does nothing", and it is worst when pivoting, because both wheels
// fight the floor and the whole robot's weight is being dragged sideways.
// Pivoting also uses a higher PWM than forward motion for the same reason.
// Per-wheel compensation and the pivot boost are both settable from /setting
// and saved to flash.

void motorBegin() {
  pinMode(M_LEFT_D1,  OUTPUT);
  pinMode(M_LEFT_D2,  OUTPUT);
  pinMode(M_RIGHT_D1, OUTPUT);
  pinMode(M_RIGHT_D2, OUTPUT);

  // New PWM API (core esp32 >= 3.0); ledcSetup/ledcAttachPin were removed.
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

// Per-motor direction invert: if the wiring is backwards the robot drives
// opposite to the button. Flip it here instead of rewiring.
//   YL1 / YL0  -> invert / restore left
//   YR1 / YR0  -> same for right
// Saved to flash, so it survives a power cut.
bool motorBalikKiri  = false;
bool motorBalikKanan = false;

// Per-wheel PWM compensation. 0 = none, positive = this wheel gets more PWM
// than its partner. Use it when the robot does not drive straight, or when one
// wheel looks slower while pivoting. Raise the weak wheel's value rather than
// lowering the strong one, so the robot does not lose power.
int8_t pwmKompensasiKiri  = 0;
int8_t pwmKompensasiKanan = 0;

// Cap so a bad setting cannot push the driver far past its safe PWM.
const int8_t KOMPENSASI_MAKS = 60;

// Extra PWM while pivoting, since dragging wheels sideways is heavier than
// driving forward. Without it the robot tends to stall or judder on the spot.
uint8_t tambahanBelok = 45;

// Upper bound to keep driver and motor safe.
const uint8_t TAMBAHAN_BELOK_MAKS = 90;

// Final PWM for one wheel, including compensation and pivot boost.
// Result is always clamped to 0..255.
uint8_t motorPwmUntuk(bool sisiKiri, bool sedangBelok) {
  int nilai = (int)speedPWM;

  nilai += sisiKiri ? (int)pwmKompensasiKiri
                    : (int)pwmKompensasiKanan;

  // Pivot boost only applies when the base PWM is non-zero, so a deliberate
  // PWM of 0 still means stop.
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
  // Pivot turns the wheels against each other, so PWM is computed per wheel.
  const bool belok = (moveMode == MOVE_LEFT || moveMode == MOVE_RIGHT);

  const uint8_t pL = motorPwmUntuk(true,  belok);
  const uint8_t pR = motorPwmUntuk(false, belok);

  switch (moveMode) {
    case MOVE_FWD:
      motorSideDrive(M_LEFT_PWM,  M_LEFT_D1,  M_LEFT_D2,
                     !motorBalikKiri,  motorBalikKiri,  pL);
      motorSideDrive(M_RIGHT_PWM, M_RIGHT_D1, M_RIGHT_D2,
                     !motorBalikKanan, motorBalikKanan, pR);
      break;

    case MOVE_BACK:
      motorSideDrive(M_LEFT_PWM,  M_LEFT_D1,  M_LEFT_D2,
                     motorBalikKiri,  !motorBalikKiri,  pL);
      motorSideDrive(M_RIGHT_PWM, M_RIGHT_D1, M_RIGHT_D2,
                     motorBalikKanan, !motorBalikKanan, pR);
      break;

    // Pivot left: left wheel back, right wheel forward.
    case MOVE_LEFT:
      motorSideDrive(M_LEFT_PWM,  M_LEFT_D1,  M_LEFT_D2,
                     motorBalikKiri,  !motorBalikKiri,  pL);
      motorSideDrive(M_RIGHT_PWM, M_RIGHT_D1, M_RIGHT_D2,
                     !motorBalikKanan, motorBalikKanan, pR);
      break;

    // Pivot right: mirror of pivot left.
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

// Pulses are emitted by a task pinned to core 1, while WiFi/HTTP run on core 0,
// so HTTP requests cannot delay a pulse. Emitting from loop() measured up to
// 1156 us of jitter with 215 of 3128 pulses late; a hardware timer on the WiFi
// core was no better. Steppers also cannot jump from 0 to full speed, hence the
// ramp.
//
// ESP_TIMER_ISR is NOT available in Arduino core 3.3.12: the header guards it
// behind CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD, which is not enabled.
// That is why this uses a core-pinned task instead.

// Core the stepper task runs on. WiFi/HTTP use core 0.
#define STEPPER_CORE  1

// Reset tracking. If motion stutters while the jitter stats look clean, the
// ESP32 probably reset mid-move (brownout from motor noise), and a reset zeroes
// every counter, hiding the evidence.
//
// Detection: keep a counter in RTC memory, which survives a normal reset and is
// lost only when power actually drops. Increment on each boot; a value above 1
// means a reset happened without a power cut, which points at brownout.

// Magic marker proving RTC memory holds our data.
#define RTC_MAGIC 0x54524231UL   // "TRB1"

RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t rtcBootCount;
RTC_NOINIT_ATTR uint32_t rtcMoveInterrupted;  // move cut short by a reset

uint32_t bootResetReason = 0;
uint32_t bootCount       = 1;
uint32_t moveInterrupted = 0;
bool     brownoutSuspect = false;
bool     crashSuspect    = false;

// Readable names for the ESP-IDF reset reasons.
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

// Called first thing in setup(): reads the reset reason and counts boots
// without a power cut.
void resetTrackerBegin() {
  bootResetReason = (uint32_t)esp_reset_reason();

  if (rtcMagic == RTC_MAGIC) {
    // RTC memory still valid -> reset WITHOUT a power cut
    rtcBootCount++;
  } else {
    // Power genuinely came back on
    rtcMagic        = RTC_MAGIC;
    rtcBootCount    = 1;
    rtcMoveInterrupted = 0;
  }

  bootCount       = rtcBootCount;
  moveInterrupted = rtcMoveInterrupted;

  brownoutSuspect = (bootResetReason == 9);

  // A panic or watchdog reset also cuts motion short. Reported separately from
  // brownout so the advice differs.
  crashSuspect = (bootResetReason == 4) ||
                 (bootResetReason == 5) ||
                 (bootResetReason == 6) ||
                 (bootResetReason == 7);
  // Logging happens in setup(), where the log buffer already exists.
}

// Set when a move starts. If the ESP32 resets mid-move, this marker is still in
// RTC memory on the next boot.
void markMoveStart() {
  rtcMoveInterrupted = 1;
}

// Cleared when a move finishes normally.
void markMoveEnd() {
  rtcMoveInterrupted = 0;
}

// Move-duration measurement, used to detect lost steps.
//
// Sending 400 pulses at 800 steps/s should take a predictable time. Compare the
// actual elapsed time against the commanded average speed: a real average far
// below the requested one means pulses are not coming out as ordered.
//
// This only sees the firmware side. The motor can still lose steps mechanically
// (a slipping coupler) without showing up here, but matching numbers do prove
// the pulse train itself is clean.

volatile unsigned long gMoveStartUs   = 0;
volatile unsigned long gMoveEndUs     = 0;
volatile uint32_t      gMoveLastSteps = 0;
volatile float         gMoveLastSpeed = 0.0f;
volatile uint32_t      gMoveCount     = 0;
volatile uint32_t      gMoveStepsAtStart = 0;

// Move precision tracker: after each OPEN or CLOSE, records the gap between the
// commanded position and the position actually reached. A gap of 0 every time
// means the move is consistent; a non-zero one quantifies the drift instead of
// leaving it to feel.
volatile uint32_t gMoveReqSteps       = 0;   // steps requested by the last move
volatile int32_t  gPresisiMaksSelisih = 0;   // largest gap ever seen
volatile uint32_t gPresisiUji         = 0;   // how many moves tested
volatile uint32_t gPresisiGagal       = 0;   // how many missed

// Total pulses emitted by all motors since boot.
volatile uint32_t stepTotal = 0;

// Measured inter-pulse period in microseconds, which separates "irregular
// pulses" (jitter) from "pulses too infrequent" (period too long).
volatile uint32_t gPeriodMaxUs = 0;
volatile uint32_t gPeriodMinUs = 0xFFFFFFFF;
volatile uint32_t gPeriodSumUs = 0;
volatile uint32_t gPeriodCount = 0;
volatile uint32_t gPeriodLastUs = 0;

// Spike tracker: one pulse held far longer than scheduled. That is what makes
// motion stutter. A small average with a large max means clean pulses with
// occasional stalls, which is felt as a lurch.
volatile uint32_t gLonjakCount          = 0;   // stalls >500us
volatile uint32_t gLonjakPertamaPulsa   = 0;   // which pulse stalled first
volatile uint32_t gLonjakPertamaUs      = 0;   // size of first stall (us)
volatile uint32_t gLonjakPosisiStep     = 0;   // step at first stall
volatile uint32_t gLonjakTerbesarUs     = 0;
volatile uint32_t gLonjakTerbesarStep   = 0;

// Diagnostic switches for "why is the period too long?"
//
// Measured evidence: min period 599 us (so the spin loop is fast enough to hit
// the schedule), but max 5135-32368 us, or 8-50x late, averaging 2-3x the
// schedule (884 steps/s against a requested 1600). Since the minimum is correct,
// something outside core 1 is interfering.
//
//   bleOff             -> turn BLE off. BLE shares radio and flash with WiFi,
//                         and a flash access can stall the whole chip; 32 ms is
//                         typical of one flash operation.
//   tickHanyaMotorAktif -> only tick motors that are moving, lightening the loop.
//
// Controlled by command 'X':
//   XB1 / XB0  BLE off / on       XT1 / XT0  active motor only / both
bool bleOff              = false;
bool tickHanyaMotorAktif = false;

// BLE advertising wrappers, so command 'X' can call them without duplicating
// the init code. (Definitions live near setup(); these are declarations.)
void BLEAdvertising_stop();
void BLEAdvertising_start();

void markMoveStartTimed() {
  gMoveStartUs      = micros();
  gMoveStepsAtStart = stepTotal;   // baseline for counting pulses
  gMoveCount++;

  // Clear schedule and spike trackers for this move
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

// Ramp length must NOT be a fixed number. A fixed ramp ate almost the whole
// move at low speed and the motor stalled at the end: at K300 the lid stopped at
// 84 degrees instead of 90, while K600-K1600 landed exactly on 90. Ramp length
// is now proportional to speed (speed x 0.5), clamped to the bounds below.
//
// At microstep 1/8 a 90 degree move is 400 steps, half of the 1/16 figure, so
// RAMP_MAX_STEPS is 400. At 1/16 the 800 bound made sense because a 90 degree
// move really is 800 steps; at 1/8 a bound of 800 would spend the entire move on
// acceleration and deceleration and never reach full speed.
#define RAMP_MIN_STEPS    100.0f   // shortest ramp
#define RAMP_MAX_STEPS    400.0f   // longest ramp (one 90 deg move at 1/8)

// Deceleration floor: near the target, speed never drops below this fraction of
// the target speed. Too small and the motor stalls.
#define SLOW_FLOOR_FRAC   0.35f

// Minimum pulse speed. Below this a stepper tends to lose torque, so it is kept
// above that value. This number is critical: it once made the stepper stop dead.
//
// The ramp raises speed gradually, but starting from zero makes the first step
// infinitely long, so the initial speed is forced up to this value.
//
// What went wrong before: this was 100. With the stepper running at 200 steps/s
// that floor was half the target, so the motor jumped 0 -> 100 with no
// acceleration. A jump that large loses every step: the motor just buzzes. Every
// open/close command looked like no response at all.
//
// Safe rule: this must be small relative to the lowest stepper speed in use. The
// default is 450, so 60 is about a seventh and safe. The static_assert below and
// the runtime clamp both enforce it.
//
// Why 100 became 60: at microstep 1/8 the slowest profile is SLOW at 200
// steps/s. A floor of 100 is half the target, too close, so the motor jumps from
// standstill to 100 without enough acceleration. At 60 the floor is 30 percent of
// 200, leaving the ramp room to work in the SLOW profile.
#define SPEED_MIN_SPS     60.0f

// Compile-time guard: refuse to build if the floor exceeds a quarter of the
// default stepper speed. Lowering STEP_DEFAULT_SPS without adjusting
// SPEED_MIN_SPS then fails at compile time rather than after an upload, where it
// would show up only as a silent, motionless stepper.
static_assert(SPEED_MIN_SPS <= (STEP_DEFAULT_SPS / 4.0f),
              "SPEED_MIN_SPS terlalu besar dibanding STEP_DEFAULT_SPS. "
              "Turunkan SPEED_MIN_SPS, kalau tidak stepper akan diam total "

              "karena dipaksa melompat dari diam tanpa akselerasi.");

// Minimum pulse width for the TB6600.
//
// The TB6600HG chip only specifies twCLKH/twCLKL as 2.2 us, so 10 us is legal at
// the chip level. But ready-made TB6600 modules add an optocoupler stage in front
// of the chip, and an optocoupler has rise and fall times. A pulse that is too
// narrow may not register as a full pulse.
//
// What this fixed: the value was 10 us. At 600 steps/s the total period is
// 833 us, so the active pulse was only 1.2 percent of it. The motor responded
// with a very faint hum audible only with an ear against it, and the body got
// slightly warm: coils energised, but the driver never saw a full pulse, so the
// motor did not turn. 20 us gives twice the margin.
//
// The project research already flagged this as something to check on the device
// rather than assume from the chip datasheet.
const unsigned long PULSE_MIN_US = 20;

// Pulse width is also made proportional to the period. At low speed the period
// is long, and a flat 20 us inside a 2000 us period is still too narrow for some
// optocouplers. This is the percentage of the period used as the active pulse:
// 15 percent is wide enough for a slow optocoupler and leaves 85 percent idle, so
// the driver can still tell the rising and falling edges apart. At 600 steps/s
// the period is 833 us, giving a pulse of about 125 us instead of 10.
const unsigned long PULSE_DUTY_PERSEN = 15;

// Jitter monitor: how far pulses drift from their schedule. A large value means
// something (usually CPU load) is disturbing the timer.
volatile uint32_t jitMaxUs   = 0;    // worst deviation (us)
volatile uint32_t jitSumUs   = 0;    // total deviation (for the average)
volatile uint32_t jitCount   = 0;    // sample count
volatile uint32_t jitLateCnt = 0;    // pulses late by >50us

// Direction tracker, for diagnosing "opens and closes the same way". If the lid
// has been commanded both up and down, DIR works. One direction only means DIR
// is not reaching the TB6600. Set when the command is received, not from physical
// motion, since there is no sensor.
volatile bool gMovedUp   = false;   // commanded to a larger angle
volatile bool gMovedDown = false;   // commanded to a smaller angle

void stepperBegin(Stepper &s) {
  pinMode(s.pinPul, OUTPUT);
  pinMode(s.pinDir, OUTPUT);
  pinMode(s.pinEna, OUTPUT);
  // Use PULSE_IDLE rather than a literal LOW so polarity follows PULSE_ACTIVE.
  // A wrong idle level can keep the optocoupler energised, heating and buzzing
  // the motor.
  digitalWrite(s.pinPul, PULSE_IDLE);
  digitalWrite(s.pinDir, DIR_FOR_POS);
  digitalWrite(s.pinEna, ENA_INACTIVE);   // start FREE, motor stays cool
  s.enabled      = false;
  s.moving       = false;
  s.speedCurrent = 0;
  s.speedTarget  = 0;
}

void stepperEnable(Stepper &s, bool on) {
  digitalWrite(s.pinEna, on ? ENA_ACTIVE : ENA_INACTIVE);
  s.enabled = on;
}

// Must be called EVERY time a move starts (s.moving = true).
//
// stepperTick uses s.halfPeriodUs to decide when the next pulse is due. If it is
// still 0 from struct initialisation, two things happen and both make the motor
// look dead: the first pulse fires immediately (elapsed 0 >= 0), and then
// halfPeriodUs is derived from a speed that is ramping up from zero, giving
// periods of hundreds of milliseconds. This fills halfPeriodUs from a valid
// starting speed (the ramp floor, or the target if lower), so the first two pulses
// already keep the right schedule.
void stepperPrimePeriod(Stepper &s, float speedSps) {
  float spsAwal = SPEED_MIN_SPS;
  if (spsAwal > speedSps) spsAwal = speedSps;    // do not exceed the target
  if (spsAwal < 1.0f)     spsAwal = 1.0f;        // avoid divide by zero

  s.halfPeriodUs = (unsigned long)(500000.0f / spsAwal);

  // The pulse width must still fit inside the period.
  if (s.halfPeriodUs < (PULSE_MIN_US + 2)) {
    s.halfPeriodUs = PULSE_MIN_US + 2;
  }
}

// Release the motor after a move, honouring holdMode. Replaces every direct
// stepperEnable(s, false) call. With holdMode true the motor stays energised so
// load or gravity cannot turn it. This is also the diagnostic path: if "keeps
// spinning" disappears with holdMode true, the cause is a free motor plus load,
// not the signal.
void stepperRelease(Stepper &s) {
  if (holdMode) return;          // stay locked, motor holds position
  stepperEnable(s, false);
}

// Manual DIR test. Two causes of "always the same direction" look identical on
// screen: the 5 V rail is dead (the buck dies when testing from a laptop), so
// DIR+ has no voltage; or the DIR wire is wrong or loose, so DIR never arrives.
//
// Since there is no sensor, the test drives DIR and PUL separately and checks the
// order of motion: set DIR one way and move a little, then set DIR the other way
// and move a little more. If DIR works the two moves oppose each other and the
// motor returns to where it started, which you see as rocking. If DIR does not
// work the moves go the same way and the motor walks off in one direction.

// Drive one motor by a given number of steps in a forced direction. Used only by
// the DIR test. Blocking, so call it from a safe context.
void stepperJog(Stepper &s, long steps, int forcedDir, float sps) {
  if (steps <= 0) return;

  // Force the direction without disturbing the permanent target
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

  // The initial period must be valid before the first tick.
  stepperPrimePeriod(s, sps);

  s.lastPulseUs = micros();
  s.moving      = true;

  // Clear the measurement trackers so the reported numbers cover only this jog
  // instead of accumulating from previous moves.
  gPeriodCount        = 0;
  gPeriodSumUs        = 0;
  gPeriodMaxUs        = 0;
  gPeriodMinUs        = 0xFFFFFFFF;
  gPeriodLastUs       = 0;
  gLonjakCount        = 0;
  gLonjakTerbesarUs   = 0;

  // Wait for completion with a safe bound. The bound is 30 s, not 3 s: at
  // microstep 1/8 a 90 degree test move (400 steps) at 100 steps/s takes 4.0 s,
  // so the old 3 s limit cut the move short and under-reported pulses.
  unsigned long t0 = millis();
  while (s.moving && (millis() - t0) < 30000UL) {
    stepperTick(s, micros());
  }

  s.moving = false;
  stepperRelease(s);

  // Restore the target so the position does not jump in the UI
  s.targetStep = savedTarget;
  s.dir        = savedDir;
}

// ============================================================
//  TORQUE AND MAX SPEED TEST  (AUTOMATIC SWEEP)
// ============================================================
//
// Stepper torque cannot be set from firmware. It comes from two hardware things:
//
//  1. The current set on the TB6600 (SW1/SW2/SW3), usually 0.5-4.0 A. A NEMA 17
//     wants 1.5-2.0 A. If the current is too low the motor loses steps or cannot
//     lift the load, and no command from the ESP32 can add torque: PUL and DIR
//     only decide when and which way the motor turns, not how hard.
//
//  2. The microstep level (SW4/SW5/SW6). Counter-intuitively, a higher microstep
//     LOWERS torque per step. Per Faulhaber Application Note 015, torque per
//     microstep as a fraction of holding torque:
//        1 -> 100.00 %    2 -> 70.71 %    4 -> 38.27 %
//        8 ->  19.51 %   16 ->  9.80 %
//     At 16 a single step gives under 10 percent of holding torque, while the
//     motor's own detent torque is 5-20 percent. So many microsteps cannot beat
//     the detent torque at all and produce no motion. For a heavy load a LOW
//     microstep gives more torque, and a lid needs no 3200 pulses/rev.
//
// If torque is short, in order: raise the TB6600 current (biggest and cheapest
// win), drop the microstep, raise the motor supply to 24 V, lower the speed.
//
// Microstep is selectable at runtime (command XD<n> or the setting page), which
// updates stepsPerRev and stepsPerDeg together. The TB6600 DIP switches must be
// set to match, or every commanded angle is wrong. pulses/rev by divisor:
//        1  ->   200.0 / 360.0 = 0.5556
//        2  ->   400.0 / 360.0 = 1.1111
//        4  ->   800.0 / 360.0 = 2.2222
//        8  ->  1600.0 / 360.0 = 4.4444  (default)
//       16  ->  3200.0 / 360.0 = 8.8889
//
// Since firmware cannot change torque, this test MEASURES the speed at which the
// motor starts losing steps. That gives the safe limit and shows whether the
// problem is speed or current.

// Result of one test stage
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

// Highest speed that was still exact. -1.0 means nothing was safe yet.
float sweepKecepatanAman = -1.0f;

// Run one test move at one speed, then compare requested and emitted steps.
void sweepSatuTahap(Stepper &s, float sps, long langkah) {
  // Set up the move
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

  // Clear the trackers so the numbers cover this stage only
  gPeriodCount      = 0;
  gPeriodSumUs      = 0;
  gPeriodMaxUs      = 0;
  gPeriodMinUs      = 0xFFFFFFFF;
  gPeriodLastUs     = 0;
  gLonjakCount      = 0;
  gLonjakTerbesarUs = 0;

  uint32_t awal = stepTotal;

  // The initial period must be valid before the first tick.
  stepperPrimePeriod(s, sps);

  s.lastPulseUs = micros();
  s.moving      = true;

  // Run to completion; the 5 s bound keeps the robot from hanging if something
  // is wrong.
  unsigned long t0 = millis();
  while (s.moving && (millis() - t0) < 5000UL) {
    stepperTick(s, micros());
  }

  s.moving = false;
  stepperRelease(s);

  // Record the result
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

  // Return to the starting position so the lid does not keep opening
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

  // Pause between stages so motor and driver do not overheat.
  delay(300);
}

// Sweep the whole speed range on one lid, slow to fast. Stops early after two
// consecutive misses, so the motor is not driven on once it is clearly unable.
void sweepKecepatan(Stepper &s, const char* nama) {
  const float daftar[] = { 200, 400, 600, 800, 1000,
                           1200, 1400, 1600, 1800, 2000 };
  const uint8_t jumlah = sizeof(daftar) / sizeof(daftar[0]);

  // 800 steps, written as 180 degrees times stepsPerDeg so it follows a
  // microstep change: 800 steps at 1/8 (180 degrees), 1600 at 1/16. Long enough
  // to reach full speed, short enough to run many stages without hitting
  // anything.
  const long LANGKAH = (long)lroundf(180.0f * stepsPerDeg);

  sweepJumlahTahap  = 0;
  sweepTotalMeleset = 0;

  float speedSebelumnya = stepSpeedActive;

  addLogf("[SWEEP] ===== MULAI ===== lid %s", nama);
  addLogf("[SWEEP] %u tahap, %ld langkah tiap tahap (%.0f derajat)",
          (unsigned)jumlah, LANGKAH, (float)LANGKAH / stepsPerDeg);

  uint8_t gagalBerturut = 0;

  for (uint8_t i = 0; i < jumlah; i++) {
    sweepSatuTahap(s, daftar[i], LANGKAH);

    if (sweepHasil[sweepJumlahTahap - 1].meleset) {
      gagalBerturut++;
    } else {
      gagalBerturut = 0;
    }

    // Two misses in a row means the limit is passed.
    if (gagalBerturut >= 2) {
      addLogf("[SWEEP] Dua tahap meleset berturut-turut, uji dihentikan di %.0f step/s",
              daftar[i]);
      break;
    }
  }

  // Find the highest speed that was still exact
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

  // Restore the previous speed
  stepSpeedActive = speedSebelumnya;
}

// Pin signal test, separating firmware faults from wiring faults. If the stepper
// does not move at all, either the ESP32 emits no pulses (firmware) or it emits
// them and the motor stays still (wiring or power). This test counts the pulses
// actually emitted on the PUL pin, so a correct count proves the ESP32 did its
// job and the fault is outside it. Read it two ways: the log count, or watch the
// PUL pin with an LED or meter while the test runs.
void ujiSinyalPin(Stepper &s, const char* nama, uint32_t jumlah, float sps) {
  addLogf("[UJIPIN] %s: kirim %lu pulsa @ %.0f step/s",
          nama, (unsigned long)jumlah, sps);

  // Set up the move like a normal one
  s.dir          = +1;
  s.targetStep   = s.currentStep + (long)jumlah;
  s.speedTarget  = sps;
  s.speedCurrent = 0;
  s.pulseHigh    = false;
  s.warmUp       = 2;

  stepperEnable(s, true);
  digitalWrite(s.pinDir, DIR_FOR_POS);
  digitalWrite(s.pinPul, PULSE_OFF);
  delayMicroseconds(20);

  uint32_t awal = stepTotal;


  stepperPrimePeriod(s, sps);   // valid initial period (see function)

  s.lastPulseUs = micros();
  s.moving      = true;

  unsigned long t0 = millis();
  while (s.moving && (millis() - t0) < 5000UL) {
    stepperTick(s, micros());
  }
  s.moving = false;
  stepperRelease(s);

  uint32_t keluar = stepTotal - awal;

  addLogf("[UJIPIN] %s: pulsa keluar %lu dari %lu diminta",
          nama, (unsigned long)keluar, (unsigned long)jumlah);

  if (keluar == jumlah) {
    addLog("[UJIPIN] ESP32 SUDAH BENAR mengirim pulsa.");
    addLog("[UJIPIN] Kalau motor tetap diam, masalahnya DI LUAR ESP32:");
    addLog("[UJIPIN] cek 5V ke PUL+/DIR+/ENA+, kabel PUL, dan DIP switch.");
  } else {
    addLogf("[UJIPIN] JUMLAH PULSA TIDAK COCOK - selisih %ld",
            (long)keluar - (long)jumlah);
  }

  // Return to original position so the lid does not drift permanently
  s.targetStep = s.currentStep - (long)jumlah;
  s.dir        = -1;
  s.speedTarget  = sps;
  s.speedCurrent = 0;
  s.pulseHigh    = false;
  s.warmUp       = 2;

  stepperEnable(s, true);
  digitalWrite(s.pinDir, DIR_FOR_NEG);
  digitalWrite(s.pinPul, PULSE_OFF);
  delayMicroseconds(20);

  stepperPrimePeriod(s, sps);   // valid initial period

  s.lastPulseUs = micros();
  s.moving      = true;
  t0 = millis();
  while (s.moving && (millis() - t0) < 5000UL) {
    stepperTick(s, micros());
  }
  s.moving = false;
  stepperRelease(s);
}

// ============================================================
//  ENA POLARITY TEST  (tells common-anode from common-cathode)
// ============================================================
//
//  Symptom: stepper is STIFF at rest (cannot be turned by hand) but does
//  NOT move and is SILENT when commanded, while the firmware log reports
//  a full pulse count (e.g. 200 of 200).
//
//  That points to inverted ENA polarity: TB6600 releases the motor exactly
//  when told to move and locks it when it should be idle.
//
//  Cause: firmware assumes common-anode 5V wiring (ENA+ to 5V, ENA- to
//  GPIO, so active LOW). With common-cathode wiring (ENA+ to GPIO, ENA-
//  to GND) it is active HIGH and the logic is inverted.
//
//  This test drives the motor with TWO different polarities. Just watch on
//  which attempt the motor actually turns.
//
//  Usage: send 'P' and watch the motor.
//    P   -> test LEFT lid
//    PR  -> test RIGHT lid
void ujiPolaritasEna(Stepper &s, const char* nama) {
  const uint32_t JML = 400;
  const float    SPS = 200.0f;

  addLogf("[UJIENA] %s: mulai. Perhatikan percobaan mana yang BERPUTAR.", nama);
  addLog("[UJIENA] Percobaan 1 dari 2: level ENA seperti setelan sekarang.");
  delay(1000);

  // ---------- Attempt 1: current polarity ----------
  {
    s.dir          = +1;
    s.targetStep   = s.currentStep + (long)JML;
    s.speedTarget  = SPS;
    s.speedCurrent = 0;
    s.pulseHigh    = false;
    s.warmUp       = 2;

    stepperEnable(s, true);
    digitalWrite(s.pinDir, DIR_FOR_POS);
    digitalWrite(s.pinPul, PULSE_OFF);
    delayMicroseconds(20);

    stepperPrimePeriod(s, SPS);   // valid initial period

    s.lastPulseUs = micros();
    s.moving      = true;
    unsigned long t0 = millis();
    while (s.moving && (millis() - t0) < 5000UL) {
      stepperTick(s, micros());
    }
    s.moving = false;

    addLogf("[UJIENA] Percobaan 1 selesai. ENA dipaksa %s.",
            (ENA_ACTIVE == LOW) ? "LOW" : "HIGH");
  }

  stepperRelease(s);
  delay(1500);

  // ---------- Attempt 2: inverted polarity ----------
  addLog("[UJIENA] Percobaan 2 dari 2: level ENA DIBALIK. Perhatikan lagi.");
  delay(1000);

  {
    // Force the ENA level opposite to the firmware setting
    digitalWrite(s.pinEna, ENA_INACTIVE);
    s.enabled = true;   // keep stepperEnable from overriding it

    s.dir          = +1;
    s.targetStep   = s.currentStep + (long)JML;
    s.speedTarget  = SPS;
    s.speedCurrent = 0;
    s.pulseHigh    = false;
    s.warmUp       = 2;

    digitalWrite(s.pinDir, DIR_FOR_POS);
    digitalWrite(s.pinPul, PULSE_OFF);
    delayMicroseconds(20);

    stepperPrimePeriod(s, SPS);   // valid initial period

    s.lastPulseUs = micros();
    s.moving      = true;
    unsigned long t0 = millis();
    while (s.moving && (millis() - t0) < 5000UL) {
      stepperTick(s, micros());
    }
    s.moving = false;

    addLogf("[UJIENA] Percobaan 2 selesai. ENA dipaksa %s.",
            (ENA_INACTIVE == LOW) ? "LOW" : "HIGH");

    // Back to normal polarity, and let the motor really release
    digitalWrite(s.pinEna, ENA_INACTIVE);
    s.enabled = false;
  }

  // Return to the starting position so the lid does not drift permanently
  s.targetStep = s.currentStep - (long)JML;
  s.dir        = -1;
  s.speedTarget  = SPS;
  s.speedCurrent = 0;
  s.pulseHigh    = false;
  s.warmUp       = 2;

  stepperEnable(s, true);
  digitalWrite(s.pinDir, DIR_FOR_NEG);
  digitalWrite(s.pinPul, PULSE_OFF);
  delayMicroseconds(20);

  stepperPrimePeriod(s, SPS);   // valid initial period

  s.lastPulseUs = micros();
  s.moving      = true;
  unsigned long t0 = millis();
  while (s.moving && (millis() - t0) < 5000UL) {
    stepperTick(s, micros());
  }
  s.moving = false;
  stepperRelease(s);

  addLog("[UJIENA] Selesai.");
  addLog("[UJIENA] Kalau motor BERPUTAR di percobaan 1 saja:");
  addLog("[UJIENA]   polaritas ENA sudah BENAR, masalahnya di tempat lain.");
  addLog("[UJIENA] Kalau motor BERPUTAR di percobaan 2 saja:");
  addLog("[UJIENA]   polaritas ENA TERBALIK. Wiring kamu common-cathode.");
  addLog("[UJIENA] Kalau DIAM di kedua percobaan:");
  addLog("[UJIENA]   sinyal PUL atau DIR tidak sampai ke TB6600.");
}

// ============================================================
//  FORCE RELEASE HOLD MODE  (diagnoses a stiff motor)
// ============================================================
//
//  A stiff motor that cannot be turned by hand at rest has two possible
//  causes: holdMode active (firmware holding it on purpose, normal, cleared
//  with H0), or inverted ENA polarity (driver locks the motor when it
//  should be free -- fix the wiring). If it is still stiff after H0, it is
//  the polarity.
void ujiLepasMotor(Stepper &s, const char* nama) {
  addLogf("[LEPAS] %s: paksa ENA ke level BEBAS.", nama);

  bool sebelum = holdMode;
  holdMode = false;

  digitalWrite(s.pinEna, ENA_INACTIVE);
  s.enabled = false;

  addLogf("[LEPAS] %s: ENA sekarang %s (level %d).",
          nama,
          (s.enabled ? "AKTIF" : "BEBAS"),
          s.pinEna);

  addLog("[LEPAS] Coba putar batang motor dengan tangan sekarang.");
  addLog("[LEPAS] Kalau MASIH kaku, polaritas ENA terbalik.");
  addLog("[LEPAS] Kalau sudah bisa diputar, motor sudah lepas dengan benar.");

  holdMode = sebelum;
}

// ============================================================
//  PHYSICAL PIN TEST  (reads the pin level, not just writes it)
// ============================================================
//
//  Earlier tools only WROTE to the pin and reported "200 of 200 pulses
//  out". That only proves stepperTick ran, not that the chip pin actually
//  changed level. If a pin is held (cable loading it, damaged pin, conflict
//  with another ESP32 function) the firmware still reports success while
//  the pin never moves -- exactly our symptom: always success, no motor
//  motion, and no change in sound even with wider pulses.
//
//  This test writes the pin then READS IT BACK with digitalRead and
//  compares. A mismatch means the pin cannot be driven freely, which is
//  the real cause.
//
//  NOTE ON VALIDITY: with the TB6600 still connected, its input circuit can
//  pull the pin level, which is exactly what makes this test useful --
//  a pin that will not change level when written shows a load is holding it.
void ujiPinFisik(Stepper &s, const char* nama) {
  addLogf("[UJIPIN2] ===== %s =====", nama);
  addLogf("[UJIPIN2] PUL=GPIO%d  DIR=GPIO%d  ENA=GPIO%d",
          (int)s.pinPul, (int)s.pinDir, (int)s.pinEna);

  // ---------- Test 1: can pin PUL be toggled? ----------
  addLog("[UJIPIN2] Tes PUL: tulis HIGH lalu baca, tulis LOW lalu baca.");

  pinMode(s.pinPul, OUTPUT);
  delayMicroseconds(50);

  int gagalPul = 0;

  for (int i = 0; i < 8; i++) {
    digitalWrite(s.pinPul, HIGH);
    delayMicroseconds(30);
    int bacaHigh = digitalRead(s.pinPul);

    digitalWrite(s.pinPul, LOW);
    delayMicroseconds(30);
    int bacaLow = digitalRead(s.pinPul);

    if (bacaHigh != HIGH) gagalPul++;
    if (bacaLow  != LOW)  gagalPul++;

    addLogf("[UJIPIN2] PUL putaran %d: tulis HIGH baca %s, tulis LOW baca %s",
            i + 1,
            (bacaHigh == HIGH ? "HIGH" : "LOW"),
            (bacaLow  == LOW  ? "LOW"  : "HIGH"));
  }

  digitalWrite(s.pinPul, PULSE_IDLE);

  if (gagalPul == 0) {
    addLog("[UJIPIN2] PUL OK - pin bisa dibolak-balik dengan benar.");
  } else {
    addLogf("[UJIPIN2] PUL BERMASALAH - %d dari 16 pembacaan tidak cocok.", gagalPul);
    addLog("[UJIPIN2] Pin PUL tidak bisa dikendalikan bebas. Cek kabel ke TB6600.");
  }

  // ---------- Test 2: can pin DIR be toggled? ----------
  addLog("[UJIPIN2] Tes DIR: tulis LOW lalu baca, tulis HIGH lalu baca.");

  pinMode(s.pinDir, OUTPUT);
  delayMicroseconds(50);

  int gagalDir = 0;

  for (int i = 0; i < 8; i++) {
    digitalWrite(s.pinDir, LOW);
    delayMicroseconds(30);
    int bacaLow = digitalRead(s.pinDir);

    digitalWrite(s.pinDir, HIGH);
    delayMicroseconds(30);
    int bacaHigh = digitalRead(s.pinDir);

    if (bacaLow  != LOW)  gagalDir++;
    if (bacaHigh != HIGH) gagalDir++;

    addLogf("[UJIPIN2] DIR putaran %d: tulis LOW baca %s, tulis HIGH baca %s",
            i + 1,
            (bacaLow  == LOW  ? "LOW"  : "HIGH"),
            (bacaHigh == HIGH ? "HIGH" : "LOW"));
  }

  digitalWrite(s.pinDir, DIR_FOR_POS);

  if (gagalDir == 0) {
    addLog("[UJIPIN2] DIR OK - pin bisa dibolak-balik dengan benar.");
  } else {
    addLogf("[UJIPIN2] DIR BERMASALAH - %d dari 16 pembacaan tidak cocok.", gagalDir);
  }

  // ---------- Test 3: can pin ENA be toggled? ----------
  addLog("[UJIPIN2] Tes ENA: tulis LOW lalu baca, tulis HIGH lalu baca.");

  pinMode(s.pinEna, OUTPUT);
  delayMicroseconds(50);

  int gagalEna = 0;

  for (int i = 0; i < 8; i++) {
    digitalWrite(s.pinEna, LOW);
    delayMicroseconds(30);
    int bacaLow = digitalRead(s.pinEna);

    digitalWrite(s.pinEna, HIGH);
    delayMicroseconds(30);
    int bacaHigh = digitalRead(s.pinEna);

    if (bacaLow  != LOW)  gagalEna++;
    if (bacaHigh != HIGH) gagalEna++;

    addLogf("[UJIPIN2] ENA putaran %d: tulis LOW baca %s, tulis HIGH baca %s",
            i + 1,
            (bacaLow  == LOW  ? "LOW"  : "HIGH"),
            (bacaHigh == HIGH ? "HIGH" : "LOW"));
  }

  digitalWrite(s.pinEna, ENA_INACTIVE);
  s.enabled = false;

  if (gagalEna == 0) {
    addLog("[UJIPIN2] ENA OK - pin bisa dibolak-balik dengan benar.");
  } else {
    addLogf("[UJIPIN2] ENA BERMASALAH - %d dari 16 pembacaan tidak cocok.", gagalEna);
  }

  // ---------- Conclusion ----------
  addLog("[UJIPIN2] ----- Kesimpulan -----");

  if (gagalPul == 0 && gagalDir == 0 && gagalEna == 0) {
    addLog("[UJIPIN2] Ketiga pin ESP32 SEHAT dan bisa dikendalikan.");
    addLog("[UJIPIN2] Jadi masalahnya pasti SETELAH ESP32:");
    addLog("[UJIPIN2] kabel, TB6600, arus, atau suplai motor.");
  } else {
    addLog("[UJIPIN2] ADA PIN YANG TIDAK BISA DIKENDALIKAN.");
    addLog("[UJIPIN2] Ini penyebab motor tidak bergerak walau log");
    addLog("[UJIPIN2] firmware selalu melaporkan pulsa keluar lengkap.");
    addLog("[UJIPIN2] Coba lepas kabel pin bermasalah dari TB6600,");
    addLog("[UJIPIN2] lalu jalankan uji ini lagi. Kalau jadi OK,");
    addLog("[UJIPIN2] berarti rangkaian input TB6600 yang menahan pin.");
  }
}

// ============================================================
//  MULTIMETER MODE  (diagnoses "hums but does not move")
// ============================================================
//
//  Symptom: motor only HUMS faintly (audible with an ear against it) but
//  does not budge, while every firmware test passes: healthy pins (J),
//  full pulse count (E), both ENA polarities tried (U).
//
//  HUMMING = the TB6600 is applying holding current, which proves the
//  MOTOR supply and driver are alive. What is missing is the RESPONSE TO
//  PULSES, i.e. the PUL optocoupler never sees a current transition.
//
//  In common-anode wiring the optocoupler current must COMPLETE THE LOOP:
//      5V -> PUL+ -> optocoupler LED -> PUL- -> GPIO -> ESP32 GND
//                              -> buck GND -> back to 5V
//
//  If buck GND and ESP32 GND are NOT connected the loop is broken: the
//  motor keeps humming (holding current takes another path) but the pulses
//  never arrive. This is the most common cause of this symptom.
//
//  USAGE:
//    Send 'M' -> pin PUL HELD at IDLE level
//    Send 'N' -> pin PUL HELD at ACTIVE level
//  Then measure with a multimeter (DC volts):
//    1. PUL+ to PUL-             -> must show a voltage difference
//    2. ESP32 GND to buck GND    -> MUST be 0V (connected)
//    3. PUL- to ESP32 GND        -> must change between M and N
//
//  If ESP32 GND to buck GND is not 0V (or unreadable), tie the two module
//  grounds together. That is the fix for this humming symptom.
void ujiTahanPul(Stepper &s, const char* nama, bool aktif) {
  s.moving = false;              // make sure no move is in progress

  pinMode(s.pinPul, OUTPUT);
  digitalWrite(s.pinPul, aktif ? PULSE_ON : PULSE_OFF);

  int level = digitalRead(s.pinPul);

  addLogf("[UKUR] %s: pin PUL (GPIO%d) DITAHAN di level %s.",
          nama, (int)s.pinPul, aktif ? "AKTIF" : "IDLE");
  addLogf("[UKUR] Level terukur di pin: %s (%d)",
          (level == HIGH ? "HIGH" : "LOW"), level);

  addLog("[UKUR] Ukur dengan multimeter mode tegangan DC:");
  addLogf("[UKUR]  1) PUL+ ke PUL-         : harus ada beda tegangan");
  addLogf("[UKUR]  2) GND ESP32 ke GND buck: HARUS 0 V (tersambung)");
  addLog("[UKUR]  3) GND buck ke GND ESP32: harus nyambung, bukan putus");
  addLog("[UKUR] Kalau nomor 2 TIDAK 0 V -> GND belum tersambung.");
  addLog("[UKUR] Kalau GND putus, motor mendesis tapi tidak bergerak.");
  addLog("[UKUR] Sambungkan GND buck ke GND ESP32, lalu uji ulang.");
}

// DIR test: move forward a little, then back a little.
// If DIR works the motor rocks back and forth in place;
// otherwise it keeps running in one direction.
void stepperDirTest(Stepper &s, const char* nama) {
  // 400 step = 90 degrees at 1/8 microstep (45 at 1/16): visible and
  // audible, and long enough for the motor to reach full speed mid-move.
  // 200 is too short: the ramp (min 100 steps) eats nearly the whole move,
  // so the motor only reaches ~60% speed.
  long JOG = 400;
  float sps = 300.0;       // slow so it is clearly visible

  // Remember the active speed, then FORCE the test speed. stepperJog uses
  // its own sps, but the actual variable must be set so the ratio report
  // compares against the right number.
  float speedSebelumnya = stepSpeedActive;
  stepSpeedActive = sps;

  long posAwal = s.currentStep;

  addLogf("[UJIDIR] %s: mulai uji (jog %ld step, 2 arah @ %.0f step/s)",
          nama, JOG, sps);

  // Move 1: positive direction
  stepperJog(s, JOG, +1, sps);
  long posSetelahMaju = s.currentStep;

  // Move 2: negative direction, back to the start position
  stepperJog(s, JOG, -1, sps);
  long posSetelahMundur = s.currentStep;

  long maju   = posSetelahMaju  - posAwal;
  long mundur = posSetelahMundur - posSetelahMaju;

  addLogf("[UJIDIR] %s: gerakan1=%+ld, gerakan2=%+ld step", nama, maju, mundur);

  // Report the ACTUAL jog speed, not stepSpeedActive (which can differ
  // widely because the DIR test uses its own speed).
  if (gPeriodCount > 0) {
    unsigned long pAvg = gPeriodSumUs / gPeriodCount;
    addLogf("[UJIDIR] %s: %lu pulsa, selisih jadwal rata2=%lu us (max %ld)",
            nama, (unsigned long)gPeriodCount, pAvg, (long)gPeriodMaxUs);
  }

  addLogf("[UJIDIR] %s: %s", nama,
          (maju > 0 && mundur < 0)
            ? "firmware benar (maju lalu mundur)"
            : "firmware TIDAK membalik arah - BUG!");

  // Restore the recorded position so calibration stays valid
  s.currentStep = posAwal;
  s.targetStep  = posAwal;
  s.moving      = false;
  stepperRelease(s);

  // Restore the speed that was active before the test
  stepSpeedActive = speedSebelumnya;
  addLogf("[UJIDIR] %s: kecepatan dikembalikan ke %.0f step/s",
          nama, speedSebelumnya);

  addLog("[UJIDIR] Perhatikan motor:");
  addLog("[UJIDIR]  - BERGOYANG bolak-balik = DIR bekerja, masalah di tempat lain");
  addLog("[UJIDIR]  - JALAN TERUS satu arah = DIR TIDAK sampai ke driver");
  addLog("[UJIDIR]    Penyebab: 5V buck mati, ATAU kabel DIR lepas/salah pin");
}

void stepperMoveToDeg(Stepper &s, float deg, float speedSps) {  if (speedSps < 50.0) speedSps = 50.0;

  s.targetStep   = lroundf(deg * stepsPerDeg);

  long delta     = s.targetStep - s.currentStep;

  if (delta == 0) {
    s.moving = false;
    return;
  }

  // AUTO DIRECTION: always derived from the position difference, so open and
  // close are guaranteed opposite and the lid returns along the same path.
  // The UI cannot force a direction, so motion away from the target is
  // impossible.
  s.dir          = (delta >= 0) ? +1 : -1;

  // ---- Record the REQUESTED step count for the precision test ----
  // Compared after the move against the pulses actually emitted.
  gMoveReqSteps = (uint32_t)(delta < 0 ? -delta : delta);

  // Record the direction actually commanded; the /debug page uses it to
  // prove DIR works (went both up AND down).
  if (s.dir > 0) gMovedUp   = true;
  else           gMovedDown = true;

  s.speedTarget  = speedSps;
  s.speedCurrent = 0;             // start at rest, then ramp up
  s.pulseHigh    = false;
  s.pulseWidthUs = 0;
  s.warmUp       = 0;

  // ---- FIX: COMPUTE halfPeriodUs HERE *** KEY *** ----
  //  Bug: halfPeriodUs was not computed at move start, so it still held the
  //  struct init value 0. The first pulse then fired immediately (elapsed
  //  0 >= 0) and the next halfPeriodUs came from a speedCurrent just ramping
  //  from zero -- ~accelPerStep (e.g. 2 step/s) -> 500000/2 = 250000us =
  //  0.25s PER PULSE. The motor emitted a few pulses then looked dead, and
  //  an 800-step move took so long the core-1 guard (>3M iterations) killed
  //  it. Fixed by priming the initial period via stepperPrimePeriod().
  stepperPrimePeriod(s, speedSps);

  s.lastPulseUs  = micros();
  // Skip the first 2 pulses when measuring jitter: they are still ramping up
  // from zero so their periods are unrepresentative.
  s.warmUp       = 2;

  stepperEnable(s, true);
  // Direction follows the wiring polarity (common-anode inverts DIR logic).
  uint8_t dirLevel = (s.dir > 0) ? DIR_FOR_POS : DIR_FOR_NEG;
  digitalWrite(s.pinDir, dirLevel);

  // Log the COMMANDED direction: if the log shows dir flipping but the motor
  // always turns the same way, the DIR signal is not reaching the TB6600.
  addLogf("[DIR] %s: %ld -> %ld step (dir=%+ld, GPIO%d=%s)",
          (s.dir > 0 ? "BUKA" : "TUTUP"),
          s.currentStep, s.targetStep, s.dir,
          (int)s.pinDir,
          (dirLevel == HIGH ? "HIGH" : "LOW"));

  // Precision: force PUL to IDLE before changing DIR. If the previous move
  // left PUL active, changing DIR now makes the driver see a false edge.
  digitalWrite(s.pinPul, PULSE_OFF);
  s.pulseHigh = false;

  // DIR must be stable well before the first pulse edge: the TB6600 needs a
  // DIR setup time, and 20us is far above that at any speed.
  delayMicroseconds(20);
  s.moving = true;

  // Flag in RTC memory that a move is in flight: if the ESP32 resets
  // mid-move (e.g. brownout from motor noise) the flag survives to the next
  // boot and identifies the stall as caused by that reset.
  markMoveStart();
  markMoveStartTimed();
}

// Speed ramps up from 0 to speedTarget and back down near the goal; this is
// what prevents stall/vibration on start and stop.
float stepperNextSpeed(Stepper &s) {
  long remaining = labs(s.targetStep - s.currentStep);

  // ---- CORRECT RAMP ----
  //  Old bug: ramp length was FIXED (400 steps) while the target speed
  //  varied, so at low speed (K300) it filled nearly half the move and
  //  deceleration fell to 10% x 300 = 30 step/s -- too slow, the motor
  //  STALLED at the end and the lid stopped at ~84 deg instead of 90.
  //
  //  Fix: (1) ramp length PROPORTIONAL to target speed, capped so moves are
  //  not slow; (2) deceleration never drops below 35% of target; (3) raised
  //  minimum speed so the motor keeps torque.
  //
  //  Tuning hints:
  //   - Lid stops BEFORE target (84 of 90 deg): stall at the end. Lower
  //     RAMP_MIN_FRAC or raise SLOW_FLOOR_FRAC.
  //   - Jerky start: raise RAMP_MIN_FRAC.

  // Ramp length proportional to speed: faster = longer.
  float rampLenF = s.speedTarget * 0.5f;
  if (rampLenF > RAMP_MAX_STEPS) rampLenF = RAMP_MAX_STEPS;   // don't get too long

  // --- Upper bound: ramp must not exceed HALF the move length ---
  // (see the bug explanation below)
  {
    long setengahGerakan = remaining / 2;
    if (setengahGerakan < 1) setengahGerakan = 1;
    if ((long)rampLenF > setengahGerakan) rampLenF = (float)setengahGerakan;
  }

  // --- Lower bound applied AFTER the clamp above ---
  // ORDER MATTERS: if RAMP_MIN_STEPS were applied first, the half-move clamp
  // could override it and produce a ramp shorter than the minimum.
  // The lower bound must also not exceed half the move: for very short moves
  // (e.g. jog 20 steps) forcing a 150-step ramp means the motor never
  // reaches full speed at all.
  if (rampLenF < RAMP_MIN_STEPS) {
    float maksRamp = (float)(remaining / 2);
    if (maksRamp < 1.0f) maksRamp = 1.0f;
    rampLenF = (RAMP_MIN_STEPS < maksRamp) ? RAMP_MIN_STEPS : maksRamp;
  }
  if (rampLenF < 1.0f) rampLenF = 1.0f;

  // ---- BUG FIXED HERE ----
  //  Before: ramp length was not bounded by the move length. For an 800-step
  //  move at K1600, rampLen = 1600*0.5 = 800, so 'remaining < rampLen' was
  //  true from the very first step and the motor was slowed from step one --
  //  it NEVER reached target speed. Measured: average only 882 of 1600 (55%),
  //  theory predicts 781 (49%) -- a match. The speed changed every step, so
  //  the motor lost sync easily -> stuttering/vibration.
  //
  //  FIX: rampLen may not exceed HALF the move length -- half accelerating,
  //  half decelerating -- so the motor always has a middle section at full
  //  speed and the profile is a proper trapezoid.
  //  (this clamp is applied above, before the lower bound)
  const long rampLen = (long)rampLenF;

  // ---- MINIMUM SPEED DURING THE RAMP ----
  //  Computed AFTER rampLen is settled, and must not exceed the target speed:
  //  if it did, the motor would jump from rest straight to that speed with no
  //  acceleration, which for a stepper means total step loss -- it just buzzes
  //  without turning.
  float spsMin = SPEED_MIN_SPS;
  if (spsMin > s.speedTarget) spsMin = s.speedTarget;

  // Slow down near the goal
  float capSps = s.speedTarget;
  if (remaining < rampLen) {
    float frac = (float)remaining / (float)rampLen;
    if (frac < SLOW_FLOOR_FRAC) frac = SLOW_FLOOR_FRAC;
    float slow = s.speedTarget * frac;
    if (slow < capSps) capSps = slow;
  }

  // Ramp up gradually toward capSps.
  // accelPerStep is derived from the rampLenF currently in force, so it is
  // automatically right for short and long moves alike.
  const float accelPerStep = s.speedTarget / rampLenF;
  if (s.speedCurrent < capSps) {
    s.speedCurrent += accelPerStep;
    if (s.speedCurrent > capSps) s.speedCurrent = capSps;
  } else {
    s.speedCurrent = capSps;
  }

  // Floor: a stepper needs a minimum speed to keep torque; too slow stalls
  // and vibrates. But it must not exceed the target speed, or the motor
  // jumps from rest to that speed with no acceleration -- which makes the
  // stepper sit still and just buzz (see spsMin above).
  if (s.speedCurrent < spsMin) s.speedCurrent = spsMin;
  return s.speedCurrent;
}

// Compute the next period for one motor.
//
// JITTER MEASUREMENT NOTE: we do NOT measure jitter as the interval between
// pulses, because the pulse period genuinely changes with the ramp. What is
// measured is TASK JITTER: how long the stepper task was held off before it
// could service a pulse -- the gap between successive task calls. If that gap
// is much larger than requested, something is blocking the task.
static inline void stepperTick(Stepper &s, unsigned long now) {
  if (!s.moving) return;

  // Phase 1: pulse is ACTIVE -> end it once the pulse width has elapsed.
  // Width is PULSE_MIN_US, widened proportionally via PULSE_DUTY_PERSEN when
  // the period is long (low speed), so the TB6600 optocoupler always has
  // enough time to read the pulse. See PULSE_MIN_US above.
  if (s.pulseHigh) {
    unsigned long lebarPulsa = PULSE_MIN_US;
    {
      unsigned long perioda = s.halfPeriodUs * 2UL;
      if (perioda > 0) {
        unsigned long proporsional = perioda * PULSE_DUTY_PERSEN / 100UL;
        if (proporsional > lebarPulsa) lebarPulsa = proporsional;
      }
    }
    if (now - s.lastPulseUs < lebarPulsa) return;
    // End the pulse. With common-anode 5V, "done" = back to HIGH.
    digitalWrite(s.pinPul, PULSE_OFF);
    s.pulseHigh   = false;
    s.lastPulseUs = now;
    return;
  }

  // Phase 2: wait out the rest of the period, then raise the pulse again
  unsigned long elapsed = now - s.lastPulseUs;
  if (elapsed < s.halfPeriodUs) return;

  // Measure the OVERSHOOT: how far past schedule we are.
  // The FIRST and SECOND pulses after a move starts are deliberately NOT
  // measured: the first fires while halfPeriodUs is still 0 / just set, and
  // speedCurrent ramps from zero so the second period is far longer than
  // normal. Both are legitimate ramp transitions, not disturbances; counting
  // them produces a misleading "1 pulse late" that makes max jitter look bad
  // when the pulses are in fact clean. s.warmUp holds the remaining pulses to
  // skip before measurement counts.
  if (s.warmUp > 0) {
    s.warmUp--;
  } else {
    unsigned long over = elapsed - s.halfPeriodUs;

    if (over < 50000UL) {        // above 50 ms = a pause, not jitter
      jitSumUs += (uint32_t)over;
      jitCount++;
      if (over > jitMaxUs) jitMaxUs = (uint32_t)over;
      if (over > 50)       jitLateCnt++;
    }
  }

  // Start the pulse. With common-anode 5V, "pulse" = pull LOW.
  digitalWrite(s.pinPul, PULSE_ON);
  s.pulseHigh   = true;
  s.lastPulseUs = now;
  s.currentStep += s.dir;
  stepTotal++;

  // ---- PRECISION FIX: make sure the LAST pulse is always closed ----
  //  The old code checked "target reached" after raising the pulse and
  //  returned without lowering PUL, so PUL stayed ACTIVE (LOW for
  //  common-anode) until the next move began. If that next move used a
  //  DIFFERENT DIR, the TB6600 saw an edge on PUL at the same time DIR
  //  changed and counted it as one extra step in the new direction. The motor
  //  shifted one step (0.1125 deg) INCONSISTENTLY depending on timing --
  //  the cause of "80 percent spot on, the rest shifted".
  //
  //  Fix: once the last step is sent, wait one pulse width then CLOSE the
  //  pulse, so PUL is always back to idle before the move ends and there is
  //  no false edge when DIR changes.
  if (s.currentStep == s.targetStep) {
    // Wait out the pulse width so the TB6600 can read this last step. Same
    // width as Phase 1 (min PULSE_MIN_US, wider for long periods): a
    // different width here would make the last pulse shorter than the ones
    // before it, and that pulse is the one most often lost since it decides
    // the final position.
    unsigned long lebarTerakhir = PULSE_MIN_US;
    {
      unsigned long perioda = s.halfPeriodUs * 2UL;
      if (perioda > 0) {
        unsigned long proporsional = perioda * PULSE_DUTY_PERSEN / 100UL;
        if (proporsional > lebarTerakhir) lebarTerakhir = proporsional;
      }
    }

    unsigned long tunggu = micros();
    while ((micros() - tunggu) < lebarTerakhir) {
      // short spin; no delay() so control is not handed to another task
      // in the middle of the last pulse
    }

    digitalWrite(s.pinPul, PULSE_OFF);
    s.pulseHigh   = false;

    // Small settle after the falling edge before the move is called done,
    // giving the driver time to finish the final step.
    tunggu = micros();
    while ((micros() - tunggu) < 5) { }

    s.moving = false;
    stepperRelease(s);
    return;
  }

  // ---- Measure PERIOD ERROR against the CURRENT schedule ----
  // This reading must happen BEFORE halfPeriodUs is recomputed for the next
  // pulse; after that the comparison is against the NEXT pulse's schedule,
  // not the one just waited out, which makes the number meaningless.
  // Do not compare against stepSpeedActive either: during a ramp the speed
  // changes. The valid schedule is halfPeriodUs: error = elapsed - halfPeriodUs.
  {
    long err = (long)elapsed - (long)s.halfPeriodUs;

    // Ignore the first pulse (halfPeriodUs is not meaningful yet)
    if (gPeriodLastUs != 0 && err > -100000L && err < 100000L) {
      if (err > (long)gPeriodMaxUs)  gPeriodMaxUs = (uint32_t)err;
      if (err < (long)gPeriodMinUs)  gPeriodMinUs = (uint32_t)err;

      uint32_t a = (uint32_t)(err < 0 ? -err : err);
      gPeriodSumUs += a;
      gPeriodCount++;

      // Record on which PULSE a large spike occurred: this separates a
      // start spike (ramp transition, harmless) from a mid-move spike
      // (a block, the cause of stuttering).
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

  // Compute the next speed (ramp) for the next period
  float sps = stepperNextSpeed(s);
  s.halfPeriodUs = (unsigned long)(500000.0 / sps);
  if (s.halfPeriodUs < (PULSE_MIN_US + 2)) {
    s.halfPeriodUs = PULSE_MIN_US + 2;
  }
}

// ============================================================
//  STEPPER TASK ON CORE 1  (the core of the jitter fix)
// ============================================================
//
//  Problem: if stepper pulses come from loop() or a timer on core 0 (the
//  WiFi core), every HTTP request delays a pulse -> jerky motion.
//  Measured before the fix: max jitter 1156 us, 215 of 3128 pulses late
//  by >50 us.
//
//  Solution: run the pulse loop on CORE 1 at high priority. Arduino
//  WiFi/HTTP runs on core 0, so the two do not disturb each other.

TaskHandle_t stepperTaskHandle = nullptr;

// ---- Instrumentation: time spent by the stepper task itself ----
volatile uint32_t spinLoopCnt   = 0;   // number of spin iterations
volatile uint32_t spinMaxUs     = 0;   // longest spin iteration (us)
volatile uint32_t yieldLateMax  = 0;   // lateness after taskYIELD (us)
volatile uint32_t delayLateMax  = 0;   // lateness after vTaskDelayUntil (us)

// Pulse loop. Runs on core 1.
//
// IMPORTANT: the strategy here is to SPIN while a motor is moving, with no
// taskYIELD or delay. On core 1, taskYIELD may not return promptly (another
// task takes over), which delays pulses by milliseconds. Pure spin + micros()
// emits pulses on time for the whole move. When idle the task sleeps fully so
// it wastes no CPU.
void stepperTaskCore1(void* param) {
  for (;;) {
    // ---- MOVING MODE: precise spin, NO sleep ----
    // While any motor moves this task never sleeps: vTaskDelayUntil has 1 ms
    // tick resolution while the pulse period can be 0.83 ms (K600) or less,
    // so sleeping between pulses makes the next pulse wait for the next tick,
    // giving ~490 us measured average jitter (nearly half a tick) -- the
    // cause of jerky motion. Full spin gives ~0.2 us average jitter.
    // Trade-off: core 1 is fully used while the lid moves, which is fine:
    // moves last <2 s and core 1 exists for this (WiFi is on core 0).
    if (stLidLeft.moving || stLidRight.moving) {
      spinLoopCnt++;
      unsigned long t0 = micros();

      while (stLidLeft.moving || stLidRight.moving) {
        unsigned long now = micros();

        // Only service motors that ARE moving. Calling a stopped motor would
        // just return immediately, but skipping it cuts the number of calls
        // per iteration and makes the loop lighter.
        if (tickHanyaMotorAktif) {
          if (stLidLeft.moving)  stepperTick(stLidLeft,  now);
          if (stLidRight.moving) stepperTick(stLidRight, now);
        } else {
          stepperTick(stLidLeft,  now);
          stepperTick(stLidRight, now);
        }

        // Anti-stuck guard: if still moving past the time limit, force a stop
        // so no lid runs out of control.
        //
        // IMPORTANT: this guard uses TIME, not a loop-iteration count. It used
        // a fixed count (> 3000000), which was wrong because that time limit
        // shifted whenever stepsPerRev (microstep) changed. Real case: after
        // the microstep change to 1/8, test command E400 (=90 degrees) at 100
        // step/s needs 4.0 s, and the old counter cut it at ~3 s, stopping at
        // 389 of 400 pulses and printing "[ERROR] Stepper >3s" -- although the
        // pulses emitted were perfect (max jitter 2 us). The guard was wrong,
        // not the motor or driver.
        //
        // 30 s is far above the longest legitimate move: 120 degrees at SLOW
        // 200 step/s is 533 steps = 2.7 s, so 30 s is never reached by a valid
        // move but still stops a truly stuck motor.
        if (micros() - t0 > 30000000UL) {
          stLidLeft.moving  = false;
          stLidRight.moving = false;
          stepperRelease(stLidLeft);
          stepperRelease(stLidRight);
          addLog("[ERROR] Stepper >30s - dihentikan paksa");
          break;
        }
      }

      uint32_t spent = (uint32_t)(micros() - t0);
      if (spent > spinMaxUs) spinMaxUs = spent;

      // Both motors finished cleanly -> clear the RTC memory flag. The flag
      // stays set for the whole move, so a reset mid-move is visible on the
      // next boot.
      if (!stLidLeft.moving && !stLidRight.moving) {
        markMoveEnd();
        gMoveEndUs = micros();   // record the move end BEFORE computing

        // ---- PRECISION MEASUREMENT ----
        // Compare steps REQUESTED with steps actually sent. Equal means the
        // move was precise; a difference means steps were dropped, which is
        // what shifts the stopping point.
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
                    (float)selisih / stepsPerDeg);
          } else {
            addLogf("[PRESISI] tepat %lu langkah - presisi",
                    (unsigned long)gMoveReqSteps);
          }
        }

        // Compute the ACTUAL average speed from the move duration. Far below
        // the requested speed indicates lost steps or ragged pulses.
        if (gMoveStartUs && gMoveEndUs > gMoveStartUs) {
          unsigned long durUs = gMoveEndUs - gMoveStartUs;
          uint32_t      steps = (uint32_t)(stepTotal - gMoveStepsAtStart);
          if (durUs > 0 && steps > 0) {
            float aktual = (float)steps * 1000000.0f / (float)durUs;
            gMoveLastSpeed = aktual;
            gMoveLastSteps = steps;

            // Only show it when interesting (> 20% off)
            float diminta = stepSpeedActive;
            if (aktual < diminta * 0.8f) {
              addLogf("[HATI] Gerakan %.0f step/s aktual, diminta %.0f "
                      "(%.0f%% dari target)",
                      aktual, diminta, (aktual / diminta) * 100.0f);
            } else {
              addLogf("[OK] Gerakan %.0f step/s aktual (diminta %.0f)",
                      aktual, diminta);
            }

            // Show the PULSE SCHEDULE error -- the valid metric: compared
            // against the schedule in force (halfPeriodUs), not against
            // stepSpeedActive, which does not hold during a ramp.
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

              // Report SPIKES: these are what cause the jerk.
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

                // A first stall on pulse 1 or 2 is a ramp transition and
                // harmless; mid-move it is the cause of stuttering.
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
      continue;   // check again immediately, without sleeping
    }

    // ---- IDLE MODE: sleep, waste no CPU ----
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void stepperTaskStart() {
  BaseType_t ok = xTaskCreatePinnedToCore(
      stepperTaskCore1,      // task function
      "stepper",             // name
      4096,                  // stack
      nullptr,               // argument
      3,                     // priority (higher than the Arduino loop)
      &stepperTaskHandle,
      STEPPER_CORE);         // core 1, not the WiFi core

  if (ok == pdPASS) {
    addLogf("[OK] Task stepper di core %d (anti-jitter)", STEPPER_CORE);
  } else {
    addLog("[ERROR] Gagal membuat task stepper!");
  }
}

bool stepperBusy() { return stLidLeft.moving || stLidRight.moving; }

// Is any pulse due right now? Used by the stepper task to decide whether to
// spin or sleep; nothing due means no need to burn CPU.
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

// Move ONLY one lid.
void lidToDeg(Stepper &s, float deg, float speed) {
  stepperMoveToDeg(s, deg, speed);
}

// Move BOTH lids together. Used by the C/S commands.
void lidBothToDeg(float deg, float speed) {
  stepperMoveToDeg(stLidLeft,  deg, speed);
  stepperMoveToDeg(stLidRight, deg, speed);
}

// Apply a speed profile to the whole robot.
//
// Called when the user presses SLOW/MEDIUM/FAST and at boot to restore the
// last choice. If `simpan` is true the choice is written to flash so it
// survives.
void simpanProfil();
void pilihProfil(uint8_t p, bool simpan) {
  if (p > 2) p = PROFIL_MEDIUM;
  profilAktif = p;

  // ONLY wheel speed follows the profile. stepSpeedActive is deliberately
  // left untouched: the stepper is independent. Wheel speed is about driving
  // comfort, lid speed about torque and gear precision -- combining them
  // would make "SLOW" also slow the lid, and "FAST" could make the lid lose
  // steps.
  speedPWM = PROFIL[p].pwmRoda;

  // If the wheels are moving, apply the new speed immediately.
  if (moveMode != MOVE_STOP) motorApply();

  addLogf("[PROFIL] Roda %s - PWM %d (stepper tetap %.0f step/s)",
          PROFIL[p].nama, speedPWM, stepSpeedActive);

  if (simpan) simpanProfil();
}

// Save drive motor settings to flash. Called on every change so they
// survive a power-off.
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

// Save only the profile choice. Used when a speed button is pressed.
void simpanProfil() {
  Preferences p;
  if (!p.begin("trashbin", false)) return;
  p.putUChar("profil",  profilAktif);
  p.putFloat("stepSps", stepSpeedActive);
  // The stored speed is only valid for the microstep it was computed at, so the
  // divisor is saved alongside it. Without this the next boot could not tell an
  // old 1/16 speed from a current one.
  p.putUChar("dipMicro", microstepDiv);
  p.end();
}

// ------------------------------------------------------------
//  MICROSTEP SELECTION
// ------------------------------------------------------------
// Sets the microstep the firmware ASSUMES and recomputes stepsPerDeg. The DIP
// switches on the TB6600 must already be set to match; this only tells the
// firmware what they say, which is why the UI asks you to confirm.
//
// Refused while a lid is moving: the speed and target of an in-flight move were
// computed at the old steps-per-degree, so changing it mid-move would send the
// lid to the wrong angle.
//
// Step speed is rescaled by the same ratio, so the lid keeps moving at the same
// physical speed: at 1/16 the pulse count per degree doubles, so the pulses per
// second must double too. Without this, switching from 1/8 to 1/16 would make
// every move run at half speed and possibly stall.
bool setMicrostep(uint8_t div, bool simpan) {
  if (!microstepDidukung(div)) {
    addLogf("[STEP] Microstep 1/%u tidak didukung (1,2,4,8,16 saja)", (unsigned)div);
    return false;
  }

  if (stepperBusy()) {
    addLog("[STEP] Tidak bisa ganti microstep saat lid bergerak. Tunggu selesai.");
    return false;
  }

  if (div == microstepDiv) {
    if (simpan) simpanProfil();
    return true;
  }

  // Rescale the active speed so the physical lid speed is unchanged.
  float rasio = (float)div / (float)microstepDiv;

  // currentStep is a raw step COUNT, so the same count means a different ANGLE
  // once stepsPerDeg changes: 400 steps is 90 deg at 1/8 but 45 deg at 1/16.
  // Capture the angles now, while the old stepsPerDeg is still in effect, so the
  // counts can be rebuilt afterwards and the reported position stays continuous.
  float degL = stLidLeft.currentStep  / stepsPerDeg;
  float degR = stLidRight.currentStep / stepsPerDeg;

  microstepDiv = div;
  stepsPerRev  = 200.0f * (float)div;
  stepsPerDeg  = stepsPerRev / 360.0f;

  stLidLeft.currentStep  = lroundf(degL * stepsPerDeg);
  stLidRight.currentStep = lroundf(degR * stepsPerDeg);
  stLidLeft.targetStep   = stLidLeft.currentStep;
  stLidRight.targetStep  = stLidRight.currentStep;

  float spsBaru = stepSpeedActive * rasio;
  float spsLama = stepSpeedActive;

  // Tell the truth when the guardrail bites. Preserving lid speed exactly is
  // impossible at the extremes: 450 step/s at 1/1 is already ~810 deg/s, and
  // scaling that to 1/16 would need 28 step/s, below the 50 that keeps a stepper
  // turning at all. Clamping is correct here, but the speed then really does
  // change, so it is reported rather than hidden.
  bool dijepit = false;
  if (spsBaru < 50.0f)   { spsBaru = 50.0f;   dijepit = true; }
  if (spsBaru > 4000.0f) { spsBaru = 4000.0f; dijepit = true; }
  stepSpeedActive = spsBaru;

  addLogf("[STEP] Microstep 1/%u: %.0f pulsa/putaran, 1 derajat = %.4f langkah",
          (unsigned)div, stepsPerRev, stepsPerDeg);
  addLogf("[STEP] Kecepatan %.0f -> %.0f step/s", spsLama, stepSpeedActive);
  if (dijepit) {
    addLog("[STEP] Kecepatan lid ikut berubah karena batas 50-4000 step/s.");
    addLog("[STEP] Setel ulang kecepatan kalau terasa terlalu cepat/lambat.");
  }
  addLogf("[STEP] Posisi lid tetap %.1f / %.1f derajat", degL, degR);
  addLog("[STEP] Pastikan DIP SW1/SW2/SW3 di TB6600 sudah disetel ke nilai ini.");

  if (simpan) simpanProfil();
  return true;
}

// Old name for simpanMotor(), kept so older code does not all need changing.
void simpanArahMotor() {
  simpanMotor();
}

// Load the profile choice from flash at boot.
void muatProfil() {
  Preferences p;
  if (!p.begin("trashbin", true)) {
    pilihProfil(PROFIL_MEDIUM, false);
    return;
  }

  uint8_t v = p.getUChar("profil", PROFIL_MEDIUM);

  // Stepper speed is loaded separately because the stepper is independent of
  // the wheel profile, and so a value stored before the profile struct
  // changed is still readable.
  //
  // IMPORTANT: a stored value can come from settings that no longer fit, and the
  // magnitude does not always reveal it. Real example: 600 was stored while the
  // firmware used 1/16 microstep; at 1/8 the same 600 means the lid moves TWICE
  // as fast. 600 itself still looks plausible (within 50..4000), so a range check
  // alone cannot catch it. Hence the stored divisor: if it differs from the
  // current one, the saved speed was computed for a different
  // pulses-per-degree and MUST NOT be used as-is. It is rescaled instead, so the
  // lid keeps the same physical speed.
  float sps = p.getFloat("stepSps", STEP_DEFAULT_SPS);

  uint8_t divTersimpan = p.getUChar("dipMicro", 0);

  p.end();

  if (v > 2) v = PROFIL_MEDIUM;
  pilihProfil(v, false);

  // Restore the microstep first, so the speed below is interpreted correctly.
  // A missing marker (old firmware or fresh flash) keeps the compiled default.
  bool divValid = microstepDidukung(divTersimpan);

  if (divValid && divTersimpan != microstepDiv) {
    float rasio = (float)divTersimpan / (float)microstepDiv;
    microstepDiv = divTersimpan;
    stepsPerRev  = 200.0f * (float)divTersimpan;
    stepsPerDeg  = stepsPerRev / 360.0f;

    float spsBaru = sps * rasio;
    if (spsBaru < 50.0f)   spsBaru = 50.0f;
    if (spsBaru > 4000.0f) spsBaru = 4000.0f;

    addLogf("[STEP] Microstep dari memori = 1/%u, kecepatan %.0f -> %.0f step/s",
            (unsigned)divTersimpan, sps, spsBaru);
    sps = spsBaru;
  } else if (!divValid && divTersimpan != 0) {
    addLogf("[STEP] Microstep tersimpan 1/%u tidak dikenal - pakai 1/%u",
            (unsigned)divTersimpan, (unsigned)microstepDiv);
  } else if (divTersimpan == 0) {
    // No marker: fresh flash or firmware from before this setting existed.
    addLogf("[STEP] Belum ada microstep tersimpan - pakai default 1/%u",
            (unsigned)microstepDiv);
  }

  if (sps < 50.0f || sps > 4000.0f) {
    addLogf("[STEP] Kecepatan tersimpan %.0f tidak masuk akal - pakai default %.0f",
            sps, STEP_DEFAULT_SPS);
    sps = STEP_DEFAULT_SPS;
  }

  stepSpeedActive = sps;

  addLogf("[STEP] Kecepatan stepper dari memori = %.0f step/s", stepSpeedActive);
  addLogf("[STEP] Microstep firmware = 1/%u (%.0f pulsa/putaran, 1 derajat = %.4f langkah)",
          (unsigned)microstepDiv, stepsPerRev, stepsPerDeg);
}

// ------------------------------------------------------------
//  SAVE / LOAD GLOBAL ANGLE TO FLASH (NVS)
// ------------------------------------------------------------
//  So a configured angle survives power-off. LID_ANGLE_DEF is used if it was
//  never set.
//
//  Flash writes are DEFERRED, never done inline from a command handler. An NVS
//  write is a SPI flash operation, and Espressif documents that caches are
//  disabled during SPI1 work and other tasks are suspended while the other core
//  busy-waits. A write issued while a lid is moving can therefore stall the
//  pulse task and drop steps. Handlers set gSudutKotor instead; flushKotor()
//  runs from loop() once the motors are idle.
void simpanSudutGlobal() {
  Preferences p;
  if (!p.begin("trashbin", false)) return;
  p.putFloat("lidAngle", lidAngleGlobal);
  p.end();
}

// Set by the 'W' handler. The value is in RAM immediately and only the saving
// waits, so behaviour is unchanged if power is cut before the flush -- the
// compromise is an unsaved angle, not a wrong one.
bool gSudutKotor = false;

// Write a pending angle once nothing is moving. Called every loop().
void flushKotor() {
  if (!gSudutKotor)   return;
  if (stepperBusy())  return;   // never write flash mid-move
  simpanSudutGlobal();
  gSudutKotor = false;
  addLogf("[SUDUT] Sudut global %.1f derajat tersimpan", lidAngleGlobal);
}

void muatSudutGlobal() {
  Preferences p;
  if (!p.begin("trashbin", true)) {
    lidAngleGlobal = LID_ANGLE_DEF;
    return;
  }

  float v = p.getFloat("lidAngle", LID_ANGLE_DEF);

  // Also load the drive motor settings, stored in the same place.
  motorBalikKiri  = p.getBool("balikKiri",  false);
  motorBalikKanan = p.getBool("balikKanan", false);

  pwmKompensasiKiri  = p.getChar("kompKiri",  (int8_t)0);
  pwmKompensasiKanan = p.getChar("kompKanan", (int8_t)0);
  tambahanBelok      = p.getUChar("tambahBelok", (uint8_t)45);

  // Guard against corrupted or out-of-range flash values.
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

// ==================== COMMAND LOGIC (shared by WiFi & BLE) ====================
void executeCommand(char cmd, const String &arg) {
  lastCmdMillis = millis();      // watchdog reset on any command
  switch (cmd) {
    case 'F': moveMode = MOVE_FWD;   motorApply(); break;
    case 'B': moveMode = MOVE_BACK;  motorApply(); break;
    case 'L': moveMode = MOVE_LEFT;  motorApply(); break;
    case 'R': moveMode = MOVE_RIGHT; motorApply(); break;
    case 'P': moveMode = MOVE_STOP;  motorStop();  break;

    // 'N' = select speed profile (SLOW/MEDIUM/FAST). N<0|1|2> or N<slow|medium|fast>.
    // One profile sets both wheel and lid speed; saved to flash.
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

    // 'Q' = stepped speed sweep: finds the speed at which the motor starts losing
    // steps. Q/QL left, QR right, QB both. Torque is NOT settable from firmware,
    // so this measures a speed limit, not added power; torque means changing the
    // TB6600 DIP-switch current (see sweepKecepatan()).
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

    // 'E' = slow pin-signal test (no load): separates firmware faults from wiring
    // faults when the stepper never moves. E/EL left, ER right, EB both, E<n>
    // pulses. 100 step/s so movement is visible by eye. Default 400 pulses = 90
    // deg at 1/8 microstep, tracking stepsPerRev (800 pulses at 1/16).
    case 'E': {
      String a = arg;
      a.toUpperCase();
      uint32_t jumlah = 400;
      int angkaMulai = -1;
      for (unsigned i = 0; i < a.length(); i++) {
        if (a.charAt(i) >= '0' && a.charAt(i) <= '9') { angkaMulai = i; break; }
      }
      if (angkaMulai >= 0) {
        long v = a.substring(angkaMulai).toInt();
        if (v < 10)   v = 10;
        if (v > 3200) v = 3200;
        jumlah = (uint32_t)v;
      }
      bool kiri  = (a.indexOf('L') >= 0) || (a.indexOf('R') < 0);
      bool kanan = (a.indexOf('R') >= 0) || (a.indexOf('L') < 0);
      addLog("[UJIPIN] Mulai. Motor akan bergerak SANGAT pelan.");
      addLog("[UJIPIN] Perhatikan: apakah motor bergerak, dan apakah ada bunyi.");
      if (kiri)  ujiSinyalPin(stLidLeft,  "KIRI",  jumlah, 100.0f);
      if (kanan) ujiSinyalPin(stLidRight, "KANAN", jumlah, 100.0f);
      addLog("[UJIPIN] Selesai.");
      break;
    }

    // 'U' = ENA polarity test for motors that hold stiff but never turn: tries both
    // ENA polarities in sequence, so watch which attempt rotates.
    // U left, UR right, UB both.
    case 'U': {
      String a = arg;
      a.toUpperCase();
      bool kiri  = (a.indexOf('L') >= 0) || (a.indexOf('R') < 0);
      bool kanan = (a.indexOf('R') >= 0) || (a.indexOf('L') < 0);
      // Hold mode off first so it does not mask the result.
      if (holdMode) {
        addLog("[UJIENA] Mode tahan sedang AKTIF - dimatikan dulu untuk uji.");
        holdMode = false;
      }
      if (kiri)  ujiPolaritasEna(stLidLeft,  "KIRI");
      if (kanan) ujiPolaritasEna(stLidRight, "KANAN");
      break;
    }

    // 'T' = force motor release (stiff-motor diagnosis): disables hold, forces ENA
    // to its free level, then you turn the shaft by hand. Distinguishes "firmware
    // holds it" from "ENA polarity inverted". T left, TR right, TB both.
    case 'T': {
      String a = arg;
      a.toUpperCase();
      bool kiri  = (a.indexOf('L') >= 0) || (a.indexOf('R') < 0);
      bool kanan = (a.indexOf('R') >= 0) || (a.indexOf('L') < 0);
      if (kiri)  ujiLepasMotor(stLidLeft,  "KIRI");
      if (kanan) ujiLepasMotor(stLidRight, "KANAN");
      break;
    }

    // 'J' = physical pin test: reads pin levels back with digitalRead, not just
    // writes. For when the log reports full pulses but the motor stays still.
    // J left (PUL=18 DIR=19 ENA=23), JR right (PUL=15 DIR=4 ENA=5), JB both.
    case 'J': {
      String a = arg;
      a.toUpperCase();
      bool kiri  = (a.indexOf('L') >= 0) || (a.indexOf('R') < 0);
      bool kanan = (a.indexOf('R') >= 0) || (a.indexOf('L') < 0);
      if (kiri)  ujiPinFisik(stLidLeft,  "LID KIRI");
      if (kanan) ujiPinFisik(stLidRight, "LID KANAN");
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

    // 'I' = per-side lid control: IL1/IL0 left, IR1/IR0 right, IL1R1 combined.
    // A side argument is required so it cannot clash with the 'L'/'R' pivots.
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
        // Digit after the side letter; missing digit means open (1).
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
        i++;   // skip the digit already consumed
      }
      if (!anyAction) {
        addLogf("[WARN] Argumen I tidak valid: %s (pakai L/R)", arg.c_str());
      }
      break;
    }

    // 'K' = stepper speed (step/s), independent of the wheel profile. K<sps>,
    // e.g. K400. Unrelated to SLOW/MEDIUM/FAST, which are wheels only. Saved to flash.
    case 'K': {
      if (arg.length() > 0) {
        float sps = arg.toFloat();
        // Floor 50: below that the stepper almost certainly stalls under the gear
        // load, and the ramp would sit on its lower bound.
        if (sps < 50.0f)    sps = 50.0f;
        if (sps > 4000.0f)  sps = 4000.0f;
        stepSpeedActive = sps;
        simpanProfil();
        addLogf("[STEP] Kecepatan stepper = %.0f step/s (disimpan)", stepSpeedActive);
      }
      break;
    }

    // 'D' = DIR test: drives one motor forward then back. Separates "lid always one
    // direction" causes: dead TB6600 5V supply (buck converter also dead), or DIR
    // wire loose/wrong pin. D and DL left, DR right.
    case 'D': {
      String a = arg;
      a.toUpperCase();
      bool kanan = (a.indexOf('R') >= 0);
      if (kanan) stepperDirTest(stLidRight, "KANAN");
      else       stepperDirTest(stLidLeft,  "KIRI");
      break;
    }

    // 'M' = hold PUL pin at a fixed level for multimeter measurement ("motor hisses
    // but does not move"). M/M0 idle, M1 active; MR/MR0/MR1 for the right lid.
    // HIGH/LOW per common-anode 5V wiring. See ujiTahanPul() for the GND check.
    case 'M': {
      String a = arg;
      a.toUpperCase();
      bool kanan = (a.indexOf('R') >= 0);
      bool aktif = (a.indexOf('1') >= 0);
      if (kanan) ujiTahanPul(stLidRight, "KANAN", aktif);
      else       ujiTahanPul(stLidLeft,  "KIRI",  aktif);
      break;
    }

    // 'H' = hold motor after a move ("keeps spinning" diagnosis). H1 locks, H0
    // releases (default). If H1 removes the symptom, the cause is a free motor
    // under load, not DIR/PUL signals.
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

    // 'Y' = reverse drive-motor direction, for motor wires fitted backwards so the
    // robot moves opposite to its buttons. YL1/YL0 left, YR1/YR0 right. 'Y' because
    // 'R' is taken by the right pivot; saved to flash.
    case 'Y': {
      // Y2 = full diagnostic report (the "check condition" button): every number
      // that decides whether the stepper moves, in one serial-readable block.
      // Used by debug-stepper.cmd in this folder.
      if (arg.length() >= 1 && (arg.charAt(0) == '2')) {
        addLog("==================================================");
        addLog("  LAPORAN DIAGNOSA STEPPER");
        addLog("==================================================");
        addLogf("[DIAG] Boot ke-%u, reset terakhir: %s",
                (unsigned)bootCount, resetReasonName(bootResetReason));
        addLogf("[DIAG] Brownout dicurigai: %s | Crash: %s",
                brownoutSuspect ? "YA" : "tidak",
                crashSuspect ? "YA" : "tidak");
        addLog("[DIAG] --- KONFIGURASI ---");
        addLogf("[DIAG] Microstep diasumsikan 1/%u - cocokkan dengan DIP SW1/SW2/SW3",
                (unsigned)microstepDiv);
        addLogf("[DIAG] %.0f pulsa/putaran (1 derajat = %.4f langkah)",
                stepsPerRev, stepsPerDeg);
        addLog("[DIAG] Arus TB6600 diasumsikan 2.0 A (SW4=ON SW5=OFF SW6=OFF)");
        addLog("[DIAG] Kalau DIP switch berbeda dari di atas, sudut akan SALAH.");
        addLogf("[DIAG] Kecepatan stepper aktif = %.0f step/s", stepSpeedActive);
        addLogf("[DIAG] Sudut global (buka) = %.1f derajat", lidAngleGlobal);
        addLog("[DIAG] --- POLARITAS FIRMWARE ---");
        addLogf("[DIAG] PULSE_ACTIVE = %s",
                (PULSE_ACTIVE == LOW) ? "LOW (wiring common-anode 5V)"
                                      : "HIGH (wiring common-cathode 3.3V)");
        addLogf("[DIAG] PULSE_IDLE   = %s (level saat diam)",
                (PULSE_IDLE == HIGH) ? "HIGH" : "LOW");
        addLogf("[DIAG] DIR_FOR_POS  = %s, DIR_FOR_NEG = %s",
                (DIR_FOR_POS == HIGH) ? "HIGH" : "LOW",
                (DIR_FOR_NEG == HIGH) ? "HIGH" : "LOW");
        addLogf("[DIAG] ENA_ACTIVE   = %s (motor DIKUNCI)",
                (ENA_ACTIVE == LOW) ? "LOW" : "HIGH");
        addLogf("[DIAG] ENA_INACTIVE = %s (motor BEBAS)",
                (ENA_INACTIVE == LOW) ? "LOW" : "HIGH");
        addLogf("[DIAG] Mode tahan (holdMode) = %s",
                holdMode ? "AKTIF" : "mati");
        addLog("[DIAG] --- PIN ---");
        addLogf("[DIAG] Lid KIRI : PUL=GPIO%u DIR=GPIO%u ENA=GPIO%u",
                (unsigned)LID_L_PUL, (unsigned)LID_L_DIR, (unsigned)LID_L_ENA);
        addLogf("[DIAG] Lid KANAN: PUL=GPIO%u DIR=GPIO%u ENA=GPIO%u",
                (unsigned)LID_R_PUL, (unsigned)LID_R_DIR, (unsigned)LID_R_ENA);
        addLog("[DIAG] --- POSISI & MOTOR ---");
        addLogf("[DIAG] Lid KIRI : %ld step (%.2f derajat), moving=%s",
                stLidLeft.currentStep,
                stLidLeft.currentStep / stepsPerDeg,
                stLidLeft.moving ? "YA" : "tidak");
        addLogf("[DIAG] Lid KANAN: %ld step (%.2f derajat), moving=%s",
                stLidRight.currentStep,
                stLidRight.currentStep / stepsPerDeg,
                stLidRight.moving ? "YA" : "tidak");
        addLogf("[DIAG] Total pulsa keluar sejak boot = %lu",
                (unsigned long)stepTotal);
        addLogf("[DIAG] Gerakan terakhir: %lu langkah, %.0f step/s aktual",
                (unsigned long)gMoveLastSteps, gMoveLastSpeed);
        addLog("[DIAG] --- KUALITAS PULSA (gerakan terakhir) ---");
        if (gPeriodCount > 0) {
          addLogf("[DIAG] %lu pulsa, jadwal selisih rata2=%lu us, min=%ld max=%ld",
                  (unsigned long)gPeriodCount,
                  (unsigned long)(gPeriodSumUs / gPeriodCount),
                  (long)gPeriodMinUs, (long)gPeriodMaxUs);
          addLogf("[DIAG] Pulsa macet >500us: %lu kali",
                  (unsigned long)gLonjakCount);
        } else {
          addLog("[DIAG] Belum ada gerakan sejak boot - belum ada data pulsa.");
        }
        addLog("[DIAG] --- PRESISI ---");
        addLogf("[DIAG] Diuji %lu kali, meleset %lu kali, selisih terbesar %ld langkah",
                (unsigned long)gPresisiUji, (unsigned long)gPresisiGagal,
                (long)gPresisiMaksSelisih);
        addLog("[DIAG] --- ARAH ---");
        addLogf("[DIAG] Pernah diperintah BUKA : %s", gMovedUp   ? "YA" : "belum");
        addLogf("[DIAG] Pernah diperintah TUTUP: %s", gMovedDown ? "YA" : "belum");
        addLog("[DIAG] --- KESIMPULAN OTOMATIS ---");
        if (gPeriodCount > 0 && gPeriodMaxUs <= 50) {
          addLog("[DIAG] Pulsa dari ESP32 RAPI (jitter <= 50us).");
          addLog("[DIAG] Kalau motor tetap diam/mendesis, penyebabnya DI LUAR");
          addLog("[DIAG] ESP32: tegangan catu TB6600, arus DIP, atau kabel.");
          addLog("[DIAG] TB6600 butuh 19-40VDC. Di bawah 19V motor bisa");
          addLog("[DIAG] hanya mendesis tanpa berputar.");
        } else if (gPeriodCount == 0) {
          addLog("[DIAG] Belum ada gerakan diuji. Jalankan 'E' dulu.");
        } else {
          addLogf("[DIAG] Pulsa TIDAK rapi (jitter maks %lu us).",
                  (unsigned long)gPeriodMaxUs);
          addLog("[DIAG] Ini masalah di sisi ESP32/task - laporkan ke pengembang.");
        }
        addLog("==================================================");
        break;
      }

      if (arg.length() < 2) {
        addLog("[MOTOR] Perintah Y butuh argumen, mis. YL1 / YR0 / Y2");
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
      // Applied at once so a moving wheel changes direction without stopping.
      if (moveMode != MOVE_STOP) motorApply();
      break;
    }

    // 'Z' = wheel compensation and extra power when turning, for drifting when
    // going straight or a slower wheel when pivoting. ZL<n>/ZR<n> compensation
    // -60..+60, ZB<n> extra turning PWM 0..90. e.g. ZL15 gives the left wheel PWM
    // 15 above base: raise the weak wheel, never lower the strong one. Saved to flash.
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
      // Applied at once so the change is felt immediately.
      if (moveMode != MOVE_STOP) motorApply();
      break;
    }

    // 'X' = test switches for hunting long pulse periods. XB1 BLE off (is
    // BLE/flash holding pulses?), XB0 on; XT1 tick only the active motor (spin
    // loop load), XT0 both. Also the microstep selector, XD<n>, where n is the
    // divisor on the TB6600 DIP switches: XD1/XD2/XD4/XD8/XD16. XD alone reports
    // the current setting.
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
      } else if (a.startsWith("D")) {
        // XD<n>: set the assumed microstep. No argument just reports.
        if (a.length() > 1) {
          int div = a.substring(1).toInt();
          if (div > 0) setMicrostep((uint8_t)div, true);
          else addLog("[STEP] Argumen XD harus angka, mis. XD8");
        } else {
          addLogf("[STEP] Microstep sekarang 1/%u (%.0f pulsa/putaran)",
                  (unsigned)microstepDiv, stepsPerRev);
        }
      } else {
        addLogf("[UJI] BLE=%s  tickHanyaAktif=%s  microstep=1/%u",
                bleOff ? "mati" : "hidup",
                tickHanyaMotorAktif ? "ya" : "tidak",
                (unsigned)microstepDiv);
      }
      break;
    }

    // 'G' = set one lid angle (fine control/tuning). G<L|R><angle> in DEGREES,
    // e.g. GL10, GL12.5, GR0. Direction comes from the position difference;
    // motion is clamped so it cannot hit the mechanics.
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
      // Safe range: 0 to LID_MAX_DEG
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

    // 'O' = open/close using the global angle. O<L|R|B><1|0>, 1 = open to global
    // angle (CW), 0 = close to 0 deg (CCW), e.g. OL1, OL0, OB1, OB0.
    // Why the old "close, but it still moves" bug happened: the old command used
    // RELATIVE offsets, so pressing CLOSE while closed kept subtracting and the
    // motor kept moving. This targets a FIXED angle: already there means no movement.
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
        long targetBaru = lroundf(degTujuan * stepsPerDeg);
        // Already at target means no movement at all, which is what prevents
        // "already closed but still moving".
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

    // 'W' = set global angle used by every open/close command. W<angle> in
    // degrees: W10 makes BUKA mean 10 deg, W45 means 45 deg. Saved to flash,
    // but only once the lids stop -- see flushKotor().
    case 'W': {
      float d = arg.toFloat();
      if (d < LID_ANGLE_MIN) d = LID_ANGLE_MIN;
      if (d > LID_ANGLE_MAX) d = LID_ANGLE_MAX;
      lidAngleGlobal = d;
      gSudutKotor    = true;
      addLogf("[SUDUT] Sudut global = %.1f derajat (akan disimpan setelah gerakan berhenti)",
              lidAngleGlobal);
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
           "\"microstep\":%d,\"pwmKiri\":%d,\"pwmKanan\":%d}",
           stLidLeft.currentStep  / stepsPerDeg,
           stLidRight.currentStep / stepsPerDeg,
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
           (int)microstepDiv,
           (int)motorPwmUntuk(true,  false),
           (int)motorPwmUntuk(false, false));
  server.send(200, "application/json", buf);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ==================== /debug PAGE ====================
// Laptop-free diagnostic page at http://192.168.4.1/debug: stepper jitter stats
// (max, average, late pulses), per-lid position and speed, WiFi/memory/uptime,
// the last 40 log lines, and quick test buttons (open/close each lid, sweep).
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
      // --- left stepper ---
      "\"ll\":%.2f,"
      "\"llTarget\":%.2f,"
      "\"llMoving\":%d,"
      "\"llSpeed\":%.0f,"
      "\"llEnabled\":%d,"
      // --- right stepper ---
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
      // --- config ---
      "\"stepSpeed\":%.0f,"
      "\"rampSteps\":%ld,"
      "\"pulseMinUs\":%lu,"
      "\"pwm\":%d,"
      "\"move\":\"%s\","
      "\"lopen\":%.1f,"
      // --- task instrumentation (deeper diagnosis) ---
      "\"spinLoops\":%lu,"
      "\"spinMaxUs\":%lu,"
      "\"delayLateMaxUs\":%lu,"
      "\"core\":%d,"
      // --- reset / brownout tracking ---
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
      "\"microstep\":%d,"
      "\"stepsPerDeg\":%.4f"
    "}",
    (unsigned long)(millis() / 1000),
    (unsigned long)ESP.getFreeHeap(),
    (unsigned long)ESP.getMinFreeHeap(),
    (int)WiFi.softAPgetStationNum(),
    (int)WiFi.RSSI(),
    stLidLeft.currentStep  / stepsPerDeg,
    stLidLeft.targetStep   / stepsPerDeg,
    stLidLeft.moving ? 1 : 0,
    stLidLeft.speedCurrent,
    stLidLeft.enabled ? 1 : 0,
    stLidRight.currentStep / stepsPerDeg,
    stLidRight.targetStep  / stepsPerDeg,
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
    (int)microstepDiv,
    stepsPerDeg);
  server.send(200, "application/json", buf);
}

// ==================== FULL DIAGNOSTIC REPORT (for download and sending on) ====================
// Builds one text file with everything needed to analyse a problem remotely:
// readable full status, automatic verdict on what is OK and what is wrong, the
// log history, and past diagnoses. Used by /debug/download (as a file) and
// readable in the web UI.
String buildLaporan() {
  String o;
  o.reserve(6000);
  unsigned long up = millis() / 1000UL;
  o += "==================================================\n";
  o += "LAPORAN DIAGNOSA SMART TRASHBIN\n";
  o += "==================================================\n\n";
  // ---------- SUMMARY ----------
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
  o += "Lid KIRI  posisi  : " + String(stLidLeft.currentStep / stepsPerDeg, 1) +
       " derajat (target " +
       String(stLidLeft.targetStep / stepsPerDeg, 1) + ")\n";
  o += "Lid KIRI  status  : " + String(stLidLeft.moving ? "BERGERAK" : "diam") +
       ", enabled=" + String(stLidLeft.enabled ? 1 : 0) +
       ", step=" + String(stLidLeft.currentStep) + "\n";
  o += "Lid KANAN posisi  : " + String(stLidRight.currentStep / stepsPerDeg, 1) +
       " derajat (target " +
       String(stLidRight.targetStep / stepsPerDeg, 1) + ")\n";
  o += "Lid KANAN status  : " + String(stLidRight.moving ? "BERGERAK" : "diam") +
       ", enabled=" + String(stLidRight.enabled ? 1 : 0) +
       ", step=" + String(stLidRight.currentStep) + "\n";
  o += "Kecepatan stepper : " + String(stepSpeedActive, 0) + " step/s\n";
  o += "Mode tahan motor  : " + String(holdMode ? "AKTIF (dikunci)" : "mati (dilepas)") + "\n";
  o += "Pernah BUKA       : " + String(gMovedUp ? "ya" : "belum") + "\n";
  o += "Pernah TUTUP      : " + String(gMovedDown ? "ya" : "belum") + "\n";
  o += "Total pulsa keluar: " + String(stepTotal) + "\n";
  o += "\n";
  // ---------- ACTUAL SPEED (step-loss detection) ----------
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
  // ---------- PRECISION / CONSISTENCY ----------
  o += "--- PRESISI / KONSISTENSI ---\n";
  o += "Gerakan diuji     : " + String(gPresisiUji) + "\n";
  o += "Meleset           : " + String(gPresisiGagal) + "\n";
  o += "Selisih terbesar  : " + String(gPresisiMaksSelisih) + " langkah ("
       + String(gPresisiMaksSelisih / stepsPerDeg, 3) + " derajat)\n";
  if (gPresisiUji == 0) {
    o += "[?] Belum ada gerakan yang diuji.\n";
  } else if (gPresisiGagal == 0) {
    o += "[OK] SEMUA gerakan presisi - tidak ada langkah yang hilang.\n";
    o += "     Titik berhenti konsisten di setiap pengulangan.\n";
  } else {
    float persen = (float)gPresisiGagal * 100.0f / (float)gPresisiUji;
    o += "[X] " + String(persen, 0) + "% gerakan meleset.\n";
    o += "    Selisih " + String(gPresisiMaksSelisih) +
         " langkah = " + String(gPresisiMaksSelisih / stepsPerDeg, 3) +
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
  // ---------- LAST MOVEMENT ----------
  o += "--- GERAKAN TERAKHIR ---\n";
  o += "Mode roda         : " + String((int)moveMode) + "\n";
  o += "PWM roda          : " + String(speedPWM) + "\n";
  o += "Kanan CMD terakhir: " + String(millis() - lastCmdMillis) + " ms lalu\n";
  o += "\n";
  // ---------- SYSTEM ----------
  o += "--- SISTEM ---\n";
  o += "RAM bebas         : " + String(ESP.getFreeHeap()) + " byte\n";
  o += "RAM minimum       : " + String(ESP.getMinFreeHeap()) + " byte\n";
  o += "Klien WiFi        : " + String(WiFi.softAPgetStationNum()) + "\n";
  o += "Task stepper core : " + String(STEPPER_CORE) + "\n";
  o += "Log total / drop  : " + String(logTotal) + " / " + String(logDrop) + "\n";
  o += "\n";
  // ---------- AUTOMATIC VERDICT ----------
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
  // What really matters: how low RAM ever went and whether it recovered. A
  // momentary low reading is fine as long as it did not run out and recovers.
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

// Download the full report as a .txt FILE. The Content-Disposition header makes
// the browser SAVE it as a file so it can be sent straight to a dev.
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
  // Send log messages oldest first
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

// Both pages are served straight from flash, so no RAM is used for the markup.
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
           stLidLeft.currentStep  / stepsPerDeg,
           stLidRight.currentStep / stepsPerDeg,
           lidAngleGlobal,
           stepperBusy() ? 1 : 0,
           (moveMode == MOVE_STOP) ? "stop" : "run",
           (int)speedPWM,
           (int)moveMode);
  charTX->setValue((uint8_t*)buf, strlen(buf));
  charTX->notify();
}

// ==================== SETUP ====================

// ==================== BLE ADVERTISING CONTROL (used by command 'X') ====================
// BLE shares the radio and flash with WiFi. Flash access can stall the whole chip
// (cache stall), which is what shows up as long pulse periods (max 32368 us =
// 32 ms, typical flash operation duration). WiFi is the primary link, so BLE may
// be switched off during testing to see whether the pulses get cleaner.
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
  // Read the reset reason BEFORE anything else, since RTC memory may be used by
  // other processes. This is what proves whether a stuttering movement came from
  // a mid-run ESP32 reset (brownout).
  resetTrackerBegin();
  // Load the last global angle from flash so the setting survives power-off.
  muatSudutGlobal();
  // Restore the last speed profile (SLOW/MEDIUM/FAST).
  muatProfil();
  // Flush junk in the serial buffer. Leftover buffered commands from a monitor/host
  // would otherwise execute right at boot and look like the robot acting on its own.
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
  addLogf("[OK] Stepper siap. 1 deg = %.3f step", stepsPerDeg);
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
  // ---------- Power condition report (shown on /debug) ----------
  // Proves whether a stuttering movement came from a mid-move reset (brownout).
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
  // ---------- Stepper task on CORE 1 (anti-jitter) ----------
  // Replaces loop()-based pulsing: on a separate core from WiFi, stepper pulses
  // are no longer delayed by HTTP requests.
  stepperTaskStart();
  // ---------- BLE (fallback) ----------  // Inited here, but can be turned off/on with command X.
  BLEDevice::init(BLE_NAME);
  bleDiinit = true;              // mark BLE as inited
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
  // NOTE: the stepper is no longer serviced from loop(). Pulses come from the
  // CORE 1 task (stepperTaskCore1), so slow server.handleClient() / WiFi cannot
  // delay them. That is what removes the jitter.
  // --- watchdog: if the phone vanishes mid-move, the robot must stop ---
  // Only fires while the wheels run. After the stop, moveMode becomes MOVE_STOP so
  // this prints ONCE, not repeatedly.
  if (moveMode != MOVE_STOP && (millis() - lastCmdMillis > CMD_TIMEOUT_MS)) {
    moveMode = MOVE_STOP;
    motorStop();
    addLogf("[SAFETY] Tidak ada perintah %lus - roda distop",
            CMD_TIMEOUT_MS / 1000);
  }
  // --- commands over USB Serial (for testing without a phone) ---
  // IMPORTANT: commands are read as ONE FULL LINE, not per character. A
  // per-character parser splits "IL1" into 'I', 'L', '1' and the 'L' is wrongly
  // executed as a left pivot (the wheels move!).
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
    if (serLine.length() < 32) serLine += c;   // length cap to avoid overflow
  }
  // --- BLE status report every 1 second ---
  static unsigned long lastReport = 0;
  if (bleConnected && millis() - lastReport > 1000) {
    lastReport = millis();
    sendBleStatus();
  }
  // --- deferred flash write: only once the lids are idle ---
  flushKotor();
}

