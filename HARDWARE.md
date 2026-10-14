# SmartTrashbin — Hardware Reference

Everything needed to wire this robot, plus the failure modes that have actually
happened on this build and how to tell them apart.

Two rules before you start:

- **Test with full power.** Motor battery on, 5 V buck alive. If you power the
  ESP32 from a laptop with the motor battery off, the buck dies, the TB6600 gets
  no signal, and every result is worthless. Most "the wiring is broken" sessions
  on this project were really this.
- **Nothing here senses position.** The firmware counts pulses it *sends*. If the
  rotor skips a step, the firmware still reports the target angle. Keep that in
  mind whenever an angle looks right but the lid is somewhere else.

---

## 1. Bill of materials

| Part | Component | Notes |
|---|---|---|
| Controller | ESP32 DevKit V4 32D (ESP32-D0WD-V3 rev 3.1) | Serial via CP2102, COM6 on this machine |
| Wheel driver | BTN7970 ×2 | One per side, PWM + two direction pins |
| Wheel motors | DC gearbox | Differential drive |
| Lid driver | TB6600 ×2 | One per lid |
| Lid motors | NEMA 17, 1.8° | 200 full steps per revolution |
| 5 V rail | Buck converter | **Must be alive** for the TB6600 inputs |
| Motor supply | 24 V | See the supply section; 9 V is too weak |

---

## 2. Pin map

These are the values in the firmware. The old README had the right wheel wrong
(`33/32/14`); the code is the authority.

| Function | Signal | GPIO |
|---|---|---|
| Left wheel | PWM / D1 / D2 | 25 / 27 / 26 |
| Right wheel | PWM / D1 / D2 | 14 / 13 / 12 |
| Left lid | PUL / DIR / ENA | 18 / 19 / 23 |
| Right lid | PUL / DIR / ENA | 15 / 4 / 5 |

### GPIO15 warning

GPIO15 is an ESP32 strapping pin. If the TB6600 pulls it HIGH during boot the
ESP32 can fail to start. The firmware does not touch it until `setup()` is done.
If the board will not boot: unplug the right lid's PUL wire, boot, plug it back.

---

## 3. Stepper wiring — this is where this robot breaks

The TB6600 inputs (`PUL+`, `DIR+`, `ENA+`) sit behind an optocoupler. The driver
reads the **current through that LED**, not a voltage. Wire it for 5 V and let the
GPIO pin *sink* to ground:

```
5V (buck)   ──►  PUL+ / DIR+ / ENA+      (all three, both drivers)
GPIO 18     ──►  PUL-    (left lid)
GPIO 19     ──►  DIR-    (left lid)
GPIO 23     ──►  ENA-    (left lid)
GPIO 15     ──►  PUL-    (right lid)
GPIO 4      ──►  DIR-    (right lid)
GPIO 5      ──►  ENA-    (right lid)
GND buck ──┬── GND ESP32 ──┬── GND TB6600 ──┬── GND battery
```

This is **common-anode, active-LOW**. Firmware sets `PULSE_ACTIVE = LOW` to match.

Why it matters, in numbers. With the old common-cathode 3.3 V wiring the current
was `(3.3 V − 1.2 V) / 220 Ω = 9.5 mA`, below the 10–15 mA the datasheet wants, and
the GPIO had to source it. From the 5 V rail it is `(5 V − 1.2 V) / 220 Ω = 17 mA`,
and the GPIO only sinks. The LED then fires on every pulse instead of flickering.

**The single most common mistake:** moving `PUL+` to 5 V but leaving `DIR+` on GND.
All three `+` pins move together.

### The common ground is not optional

Without `GND buck ↔ GND ESP32`, the LOW/HIGH levels have no reference and nothing
works, or works erratically. Check it with a multimeter in continuity mode.

### Never mix the two polarities

5 V common-anode wiring with active-HIGH firmware makes the motor move at the
wrong times — dropped steps, or no movement at all. Change `PULSE_ACTIVE` and
re-upload in the same session you change the wiring.

---

## 4. TB6600 DIP switches

Set on the physical module. The firmware constants must agree with them.

| Setting | DIP | Value |
|---|---|---|
| Microstep 1/8 | SW1 OFF, SW2 ON, SW3 OFF | 1600 pulses/rev |
| Current 2.0 A | SW4 ON, SW5 OFF, SW6 OFF | For a NEMA 17 |

Matching firmware: `STEPS_PER_REV = 1600.0`, `STEPS_PER_DEG = 4.4444`.

**There are two different chips sold as "TB6600".** A real Toshiba **TB6600HG**
only goes to 1/16; the "upgrade" **TB67S109AFTG** goes to 1/32. Spot them by the
chip: the TB6600HG has pins sticking through the PCB. On a TB6600HG the 1/32 DIP
combination is marked `NC` in the datasheet, so selecting it puts the chip in an
undefined mode.

If you change microstepping, you must change `STEPS_PER_REV`, `STEPS_PER_DEG`, and
the saved `DIP_TERMINAL` marker, then re-upload. The firmware compares the marker
against flash at boot and logs a warning if they disagree, but it cannot guess the
right angle for you.

---

## 5. Why 1/8 microstepping

Torque per microstep, as a fraction of holding torque (Faulhaber Application
Note 015):

| Microstep | Torque per step |
|---|---|
| 1 | 100.00 % |
| 2 | 70.71 % |
| 4 | 38.27 % |
| 8 | 19.51 % |
| 16 | 9.80 % |

The motor's own detent torque is 5–20 % of holding torque. At 1/16, one microstep
produces under 10 %, so a good share of pulses cannot beat the detent torque and
produce no motion at all. The Faulhaber note puts it plainly: taking a microstep
does not mean the motor will move.

This lid drives roughly 20 gears and is heavy, so 1/8 was chosen: double the
torque per step, resolution still far beyond what a lid needs.

Related: a change to 1/8 once got recorded here as "makes the stepper stop
dead". That conclusion was wrong. The motor was silent, and a stepper short on
torque buzzes — silence means no current at all. The real cause was the DIP switch
moved to 1/8 while the firmware constants still said 1/16, so every commanded
angle was wrong.

---

## 6. Supply

| Rail | Requirement |
|---|---|
| Motor | 24 V, 12–24 V minimum for decent torque |
| TB6600 logic | 5 V from the buck, into all `+` pins |
| ESP32 | USB or its own 5 V, independent of the motor rail if possible |

A 9 V motor rail gives weak torque and stalls easily.

### Brownout

Motor inrush can dip the rail enough to reset the ESP32 mid-move, which cancels the
move and wipes every counter — so the problem hides itself. `/debug` reports the
reset reason and boot count; a boot count above 1 with no power cycle means it
restarted on its own.

Fixes: a 1000 µF electrolytic plus a 100 nF ceramic near the ESP32 5 V/3 V3 pins,
thicker ground wires, and keeping the ESP32 supply off the motor rail.

---

## 7. Symptom → cause

The four layers below the hardware have all been proven correct from the firmware's
own logs: pulse timing (4 µs jitter, 0 of 23934 pulses late), direction commands
(`dir=+1` then `dir=-1`), GPIO writes (`GPIO19=LOW` then `HIGH`), and no resets
during movement. So for these symptoms, look at hardware.

### 7.1 The lid always moves the same direction

Press **UJI DIR** in `/debug`. The motor jogs forward then backward.

| What you see | Meaning | Next |
|---|---|---|
| Lid rocks back and forth | DIR works | Check mechanics and torque, §7.4 |
| Lid keeps going one way | DIR is not reaching the driver | Go through the list below |

In order:

1. **Is the buck putting out 5 V?** Measure output to its own ground. 0 V means
   that is your problem — most likely the motor battery is off.
2. **Are `DIR+` and `DIR-` connected?** `DIR+` → 5 V, `DIR-` → GPIO19 (left) or
   GPIO4 (right). Re-seat both ends; if a crimp pulls free easily it was loose.
3. **Is `DIR+` still on GND?** Very common when moving to common-anode. All three
   `+` pins must move to 5 V.
4. **Is the buck ground tied to the ESP32 ground?** Check continuity.
5. **Try swapping `DIR+` and `DIR-`.** If the direction becomes correct, that was
   it. If it stays one-way, DIR is not arriving at all — broken or wrong pin.

### 7.2 The stepper buzzes or shudders without turning

| Priority | Cause | Where to fix |
|---|---|---|
| 1 | Signal wiring — see §3 | Wiring |
| 2 | Speed too high | Firmware: ramp, or lower `K` |
| 3 | TB6600 current too low | DIP switches |
| 4 | Microstep mismatch | DIP switches |
| 5 | Motor coil wires in the wrong order | Motor wiring |

Five causes, in the order they usually bite:
1. **Signal wiring.** See §3. Fix this first.
2. **Too fast.** Lower the speed with `K300` or `K600` from `/debug`. If it is smooth
   at `K600` but shudders at `K1600`, the limit is torque, not signal.
3. **Current.** A NEMA 17 wants 1.5–2.0 A.
4. **Microstep.** If it is not 1/8 here, every angle in the firmware is wrong.
5. **Coil order.** Wrong pairing still moves the motor, but badly.

### 7.3 The lid stutters instead of moving smoothly

Signal level to the optocoupler — §3. Measure the buck, verify all six `+` pins are
on 5 V, verify common ground, then re-upload with the matching `PULSE_ACTIVE`.

If it still stutters after that, in order: current too low (§4), microstep wrong
(§4), motor rail voltage too low (§6), ESP32 brownout (§6), speed too high.

### 7.4 The shaft keeps turning after the firmware stops

The firmware has sent its last pulse. `stepTotal` is not rising. So the rotation is
not coming from the firmware. Three causes, distinguishable without a scope:

1. **Free motor plus load (most likely).** By default the firmware releases the motor
   after a move (`ENA` off) to save power and heat, so gravity and vibration can turn
   the shaft. It looks exactly like "still running", and always goes the same way
   because gravity does.
2. **False pulses from noise.** If the `PUL` wire picks up interference, each spike
   counts as a pulse. Signature: irregular and buzzy, not smooth.
3. **Damaged TB6600.** Signature: smooth and constant with no input at all,
   sometimes with a hissing noise.

Work through this in order:

```
Shaft turns while the firmware is idle
  │
  ├─ Press TAHAN Motor ON in /debug, then test open/close
  │    ├─ Stops  ──► Cause 1, free motor + load
  │    └─ Keeps turning ──► next
  │
  ├─ Unplug ENA- from the TB6600
  │    ├─ Stops  ──► ENA signal problem
  │    └─ Keeps turning ──► next
  │
  └─ Unplug PUL- from the TB6600
       ├─ Stops  ──► Cause 2, noise
       └─ Keeps turning ──► Cause 3, dead TB6600
```

For cause 1, hold mode is the fix: `H1` (lost on reboot) or `bool holdMode = true;`
for good. The motor is then energised continuously and gets warm; NEMA 17 at 1.5 A
is fine but wants air around it.

For cause 2: keep `PUL` away from motor and power cables, use shielded cable with
the shield to ground, add a 100 nF cap from `PUL-` to ground near the driver, and
shorten the signal runs.

---

## 8. Stepper tuning

Constants in the firmware, and what each one costs you when it is wrong.

| Constant | Current | If it is wrong |
|---|---|---|
| `STEPS_PER_REV` | 1600.0 | Must match the DIP microstep, or all angles are scaled wrong |
| `RAMP_MIN_STEPS` | 100.0 | Too high and short moves never reach full speed |
| `RAMP_MAX_STEPS` | 400.0 | Too high and one 90° move is spent entirely on ramp |
| `SLOW_FLOOR_FRAC` | 0.35 | Too low and the motor stalls at the end of a move |
| `SPEED_MIN_SPS` | 60.0 | Too high versus target speed and the motor jumps from standstill, losing all steps |

The ramp length is `speed × 0.5`, clamped to those bounds, and never more than half
the move so there is always a constant-speed middle section.

Symptom guide: the lid stopping short of the target (say 84° of 90°) means it
stalled at the end, so lower `RAMP_MIN_STEPS` or raise `SLOW_FLOOR_FRAC`. A snatch
at the start means raise `RAMP_MIN_STEPS`.

Measured after the ramp fix: `K1600` reached 1687 steps/s and `K2000` reached 2009,
against 882 and 1048 before.

---

## 9. Commands

Sent over HTTP (`/cmd?val=X`), Bluetooth, or serial at 115200.

### Wheels

| Command | Action |
|---|---|
| `F` / `B` | forward / backward |
| `L` / `R` | pivot left / right |
| `P` | stop |
| `V<n>` | PWM, 0–255 |
| `N0` / `N1` / `N2` | speed profile SLOW / MEDIUM / FAST (also `NSLOW` etc.) |

### Lids

| Command | Action |
|---|---|
| `C` / `S` | open / close both lids |
| `IL1` / `IL0` | open / close left lid |
| `IR1` / `IR0` | open / close right lid |
| `W<angle>` | set opening angle, used by the open commands |
| `A` | calibrate: current position becomes 0° |
| `K<sps>` | stepper speed, steps per second |

### Diagnostics

| Command | Action |
|---|---|
| `Q` | speed sweep, finds where steps are lost. `QL` / `QR` / `QB` for side |
| `E<n>` | send n slow pulses with no load, to separate firmware from wiring |
| `J` | read back the physical pin levels with `digitalRead` |
| `D` | direction test, jog forward then back |

### Settings

| Command | Action |
|---|---|
| `YL1` / `YL0` | invert / restore left motor direction |
| `YR1` / `YR0` | same for the right motor |
| `H1` / `H0` | hold motor locked / release after a move |
| `X` | stop / start BLE advertising |

Angles and motor direction inverts are saved to flash, so they survive a power cut.

---

## 10. Web endpoints

| URL | Content |
|---|---|
| `/` | control panel |
| `/setting` | settings page |
| `/cmd?val=X` | send a command |
| `/status` | JSON status |
| `/debug` | diagnostics page |
| `/debug/json` | all values as JSON |
| `/debug/log` | recent log as text |
| `/debug/download` | full report as a file |
| `/debug/reset` | zero the statistics |

Reading `/debug`: jitter under 50 µs is smooth, 50–200 µs is slightly rough, over
200 µs is visibly snatchy. The automatic diagnosis panel interprets the numbers for
you.

The speed buttons (`K600`…`K2000`) reset to the default on reboot; they are not
saved.

---

## 11. Known limits and next steps

The lid position is open loop. Being open loop, accumulated error is invisible, and
step loss inside the driver or rotor cannot be detected at all. To close the loop you
need a sensor: a TCRT5000 reflective sensor for homing to a hard stop, or an AS5600
magnetic encoder for continuous angle.

Two firmware-level improvements were identified but not implemented, because both
carry real regression risk on a working state machine:

1. **Generate pulses with the RMT peripheral** instead of a FreeRTOS task. This
   removes the whole class of jitter problems, since the hardware emits the pulses
   and nothing can preempt them mid-pulse. It is also a large rewrite: you build an
   `rmt_data_t` array covering the ramp, chunk around RMT block memory limits, and
   track completion yourself. `ESP_TIMER_ISR` is confirmed unavailable in this core
   version, so RMT is the practical deterministic path.
2. **Keep NVS writes out of the motion path.** Espressif's SPI flash documentation
   states that caches are disabled during SPI1 operations and other tasks are
   suspended while the other core busy-waits. `simpanSudutGlobal()` writes NVS on the
   `W` command, so a `W` during a move can stall the pulse task and drop pulses. The
   refactored firmware defers this save until the motors are idle, which is the cheap
   half of the fix; a full fix would queue writes.

Worth knowing: the 4 µs jitter figure was measured in a specific test. It does not
represent a flash write or heavy WiFi traffic happening at the same time.

---

## 12. Build and flash

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app --upload -p COM6 .
```

The `huge_app` partition scheme is required — the firmware with its embedded UI is
about 1.8 MB and the default scheme is too small.

`web_ui.h`, `debug_ui.h`, and `setting_ui.h` are generated. Edit the `.html` sources
and run `python konversi-html.py`; never edit the headers directly.

Removing the motor load for a torque test gives an upper bound. The real limit shows
up only with the lid attached, so test the way it actually runs.

---

## Appendix: datasheet values that were actually read

Recorded so nobody has to re-download these, and marked with what was verified
versus assumed.

**Toshiba TB6600HG** — "PWM Chopper-Type bipolar Stepping Motor Driver IC",
dated 2014-01-30.

| Parameter | Value |
|---|---|
| Minimum CLK pulse width (`twCLKH`, `twCLKL`) | 2.2 µs, no maximum given |
| Input HIGH (`VIH`) | 2.0 V min, 5.5 V max |
| Input LOW (`VIL`) | −0.2 V min, 0.8 V max |
| Hysteresis (`VH`) | ~400 mV |
| Input current at 5 V | 50–75 µA typical |
| Standby release | input ignored for ~200 µs after leaving standby |

Two corrections this produced, both against earlier project notes:

- The claim that "3.3 V is too weak because it is below the logic threshold" is not
  supported by this datasheet. `VIH` minimum is 2.0 V, so 3.3 V clears it by about
  1.3 V. Wiring mistakes and optocoupler current are still real problems; the
  threshold is not the reason.
- The "DIR needs 5 µs of setup" figure is **not** from this datasheet. The chip pin
  is named `CW/CCW` and is a plain level input, with no setup or hold parameter
  listed. For comparison, the TI DRV8825 — which does document these — asks for
  **650 ns**, not 5 µs. Treat the firmware's 5 µs as a conservative habit, not a
  datasheet requirement.

**Faulhaber Application Note 015** — the source of the torque-per-microstep table in
§5. Its own summary: taking a microstep does not mean the motor will move, and
microstepping buys resolution without the accuracy that usually implies.

**Verified in Espressif sources:** `ESP_TIMER_ISR` is genuinely unavailable in this
core version — `esp_timer.h` guards the enum member behind
`CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD`, which is off here.

**Not verified, do not rely on:** the module-level schematic. Ready-made TB6600
boards add their own optocoupler input stage that is not in the chip datasheet, so
effective timing can differ. The AS5600 specs were never read from a primary source.

