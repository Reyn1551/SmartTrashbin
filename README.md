# SmartTrashbin

An ESP32 robot trash bin. Two DC motors drive the wheels through BTN7970
drivers, and two NEMA 17 steppers open and close the lid through TB6600 drivers.

The ESP32 runs its own WiFi hotspot and serves the control panel from inside the
firmware, so there is nothing to install on your phone and no internet needed.
Bluetooth is there as a backup control path.

| | |
|---|---|
| Hotspot | `SmartTrashbin` |
| Password | `12345678` |
| Control panel | http://192.168.4.1 |
| Diagnostics | http://192.168.4.1/debug |

## Using it

1. Power the robot on.
2. On your phone, join the WiFi network `SmartTrashbin` with password `12345678`.
   Android will ask whether to stay connected with no internet. Say yes, and turn
   mobile data off so the phone does not wander back to it.
3. Open http://192.168.4.1

The arrow buttons on the left drive the robot while you hold them. The buttons on
the right open and close the lid. Rotate the phone to landscape and tap the panel
to open it.

If something feels wrong, a lid moving roughly or a wheel that will not turn, open
http://192.168.4.1/debug from the phone. It reads the jitter statistics and tells
you what it thinks is wrong, without needing a laptop.

## Hardware

Wiring, pinout, DIP switch settings, and the faults this robot has actually
produced are all in **[HARDWARE.md](HARDWARE.md)**. Read that before rewiring
anything. The stepper signal wiring in particular is not what you would guess.

Short version: the TB6600 inputs need 5 V and the GPIO pins sink to ground
(common-anode, active-LOW). The firmware is set up for this, so do not pair that
wiring with active-HIGH firmware.

## Commands

Sent over HTTP (`/cmd?val=X`), Bluetooth, or serial at 115200 baud.

| Command | Action |
|---|---|
| `F` / `B` | forward / backward |
| `L` / `R` | pivot left / right |
| `P` | stop |
| `V<n>` | wheel PWM, 0–255 |
| `N0` / `N1` / `N2` | speed profile SLOW / MEDIUM / FAST |
| `C` / `S` | open / close both lids |
| `IL1` / `IL0` | open / close left lid |
| `IR1` / `IR0` | open / close right lid |
| `W<angle>` | set the opening angle |
| `A` | calibrate: current position is 0° |
| `K<sps>` | stepper speed in steps per second |
| `XD<n>` | microstep the firmware assumes: `XD1` `XD2` `XD4` `XD8` `XD16` |

Angle, motor-direction and microstep settings go to flash and survive a power cut.

## Microstep setting

The lid's microstep is set by **physical DIP switches on the TB6600** — firmware
cannot change it. What you can set is which one the firmware *assumes*, so
switching the DIP switches no longer means editing code and re-uploading.

Set it from the **Microstep Tutup** card on `/setting`, or with `XD<n>`. The
firmware keeps the reported lid angle and rescales the step speed, so the lid
moves at the same physical speed across the change.

Pick a lower microstep (1/4, 1/2) if the lid skips or stalls under load, since
torque per step goes up. Pick a higher one (1/16) for quieter, smoother movement,
and watch for lost steps. 1/8 is the default and the only value verified on this
robot; HARDWARE.md section 4 has the DIP tables.

## Build and upload

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app --upload -p COM6 .
```

The `huge_app` partition scheme is required. The firmware with its embedded UI is
about 1.8 MB and the default scheme is too small.

### Changing the web pages

The three pages are generated, and the headers are not meant to be edited:

| Source | Header | Page |
|---|---|---|
| `controller-wifi.html` | `web_ui.h` | `/` |
| `debug-page.html` | `debug_ui.h` | `/debug` |
| `setting-page.html` | `setting_ui.h` | `/setting` |

Edit the `.html` file, then run:

```powershell
python konversi-html.py
```

That rewrites the three headers. Compile and upload after that.

### Capturing a diagnostic log without a laptop

```powershell
python ambil-log.py --detik 60 --catatan "open close 10 kali"
```

There is also `cek-stepper.cmd`, a USB-only stepper check that needs neither WiFi
nor Bluetooth.

## Two things worth knowing

**Position is counted, not measured.** There is no position sensor. The firmware
counts the pulses it sends to the driver, so a stepper that skips a step stays
invisible to it: it reports the target angle either way. The "Movement Precision"
panel on `/debug` exists to show how often that happens.

**Test with full power.** The diagnostics only mean something when the motor
battery is on, because the 5 V buck needs it to feed the TB6600 inputs. Testing
from a laptop with the motor battery off means the driver gets no signal, and
nothing measured that way says anything about the hardware. During development the
battery was deliberately kept off to avoid back-feed, so do not draw hardware
conclusions from tests run that way.

## Files

```
SmartTrashbin.ino        firmware
controller-wifi.html     control panel, edit this
debug-page.html          diagnostics page, edit this
setting-page.html        settings page, edit this
web_ui.h                 generated
debug_ui.h               generated
setting_ui.h             generated
konversi-html.py         builds the three headers from the HTML
ambil-log.py             records a log over WiFi
cek-stepper.py/.cmd      USB-only stepper check
controller-ble.html      Bluetooth control UI
HARDWARE.md              wiring, pinout, fault reference
```

## License

Not decided yet.
