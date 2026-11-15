# DTTS-V2-Software
Firmware and Frontend repository for DTTS V2 (Dynamic Thrust Test Stand).

## Repository contents

| Path | What it is |
|---|---|
| [ESP32S3_ESC.ino](ESP32S3_ESC.ino) | Main ESC control firmware: button throttle control, software stop, ESC calibration, RGB LED status. |
| [DTTS-V2-Hardware-Verification.ino/](DTTS-V2-Hardware-Verification.ino/) | Board bring-up sketch: reads the MCP3561 current-sense ADC and battery-voltage sense, and prints CSV over serial. |
| [testing_scripts/log_serial.py](testing_scripts/log_serial.py) | Prints the board's serial output on a PC (`/dev/ttyACM0`, 115200 baud). |

---

## ESC control firmware (`ESP32S3_ESC.ino`)

### Building and flashing
1. Install the **esp32** board package (Espressif) in the Arduino IDE.
2. Install the **ESP32Servo** library (Library Manager).
3. Select an **ESP32S3** board and the board's serial port, then upload.
4. Open the Serial Monitor at **115200 baud**.

### Safety first
- **Remove the propeller** whenever you calibrate the ESC or test new firmware.
- RIGHT is a **software stop only**, not a physical emergency cutoff. Always be ready to unplug the battery.
- The board sends 0% throttle at boot, and control can't be enabled during the first **5 seconds** after power-up.
- Test new firmware with the ESC **unpowered** first.

### Pin map

| Function | GPIO |
|---|---|
| CENTER button | 16 |
| UP button | 21 |
| DOWN button | 38 |
| LEFT button | 18 |
| RIGHT button | 17 |
| RGB LED red / green / blue (common anode, LOW = on) | 8 / 6 / 7 |
| ESC signal (through comparator U6 to J5) | 15 |

All buttons are active LOW and use internal pull-ups. Each press is debounced for 50 ms.

> The hardware-verification sketch uses a different UP/DOWN/LEFT/RIGHT mapping. The table above is for `ESP32S3_ESC.ino`.

### Button controls

| Button | Action |
|---|---|
| CENTER, short press (< 1 s) | Reset the logging time (`time=` goes back to 0). |
| CENTER, hold 1–3 s and release | Turn throttle control on or off. It always turns on at 0% throttle. |
| CENTER, hold ≥ 3 s and release (control off) | Start **ESC calibration** (see below). The LED turns cyan once you've held long enough. |
| CENTER, hold ≥ 3 s and release (control on) | Turns control off only. Hold 3 s again to calibrate. |
| UP | +100 µs throttle (control must be on). |
| DOWN | −100 µs throttle (control must be on). |
| LEFT (hold) | Jump the target to 100% throttle (control must be on). |
| RIGHT | **Software stop**: throttle to 0% and control off right away. Also aborts calibration. |

Notes:
- Throttle runs from 1000 µs (0%) to 2000 µs (100%) at 50 Hz, in 100 µs steps.
- Speeding up is ramped by 100 µs every 20 ms. Slowing down happens immediately.
- Turning control on or starting calibration is refused while UP or DOWN is held, or during the 5 s startup wait.
- If CENTER is held while the board boots, its first release is ignored, so the board can't switch control on by accident.

### ESC calibration

Calibration teaches the ESC what 0% and 100% throttle look like. Do it once for each new ESC, or after changing ESC settings.

1. **Remove the propeller.** Make sure throttle control is off (LED off).
2. Hold **CENTER** for 3 s until the LED turns **cyan**, then release.
3. **Step 1/3 (LED blinks blue slowly):** disconnect the battery from the ESC, then press CENTER.
4. **Step 2/3 (LED blinks magenta quickly, throttle at 100%):** connect the battery. The ESC beeps to show it has stored full throttle. Then press CENTER.
5. **Step 3/3 (LED blinks yellow slowly, throttle at 0%):** the ESC beeps again, then plays its ready tones. Then press CENTER.
6. **Done (LED solid green for 3 s):** calibration is complete. Control stays off. Hold CENTER for 1 s to turn it on.

The Serial Monitor prints each instruction and repeats the current one every 5 s. The usual status line is paused during calibration.

**To abort**, press **RIGHT** at any step. The throttle drops to 0%, the LED flashes red quickly, and you'll be told to unplug the battery before trying again. While calibrating, UP, DOWN and LEFT are ignored.

### RGB LED reference

| LED | Meaning |
|---|---|
| Off | Idle, control off |
| Solid red | Throttle control on |
| Solid cyan | CENTER held ≥ 3 s: release to start calibration |
| Slow blue blink | Calibration 1/3: disconnect the battery |
| Fast magenta blink | Calibration 2/3: 100% throttle, connect the battery |
| Slow yellow blink | Calibration 3/3: 0% throttle, wait for ready tones |
| Solid green (3 s) | Calibration complete |
| Fast red flashing (1.5 s) | Calibration aborted |

### Serial output
At boot the board prints a control summary. While it isn't calibrating, it prints a status line every 500 ms:

```
time=12345 ms, enabled=1, target=1300 us, actual=1300 us
```

- `time`: ms since the last timer reset (short CENTER press)
- `enabled`: 1 if throttle control is on
- `target`: the throttle you've commanded
- `actual`: the ramped pulse currently sent to the ESC

To log this on a PC, run `python3 testing_scripts/log_serial.py` (needs `pyserial`). Change the port in the script if the board isn't `/dev/ttyACM0`.
