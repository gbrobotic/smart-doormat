# Smart Anti-Theft Doormat (ESP32 + Bluetooth LE)

An ESP32-based doormat that detects footsteps and works as a small security system you can control from your phone over Bluetooth.

**Live control page:** https://gbrobotic.github.io/smart-doormat
(Use Chrome on Android or PC. On iPhone, use the Bluefy browser.)

## Features
- Step detection with a homemade foil pressure switch (no piezo needed)
- Arm / disarm from a Bluetooth web page, the "Serial Bluetooth Terminal" app, a push button, or a secret tap code on the mat
- PIN required to disarm over Bluetooth
- 15 s exit delay and 8 s entry window
- Intruder alarm (buzzer, OLED, LED) with auto re-arm after 60 s
- Visitor chime and loitering alert (someone standing more than 10 s)
- Tamper alert if the mat is pulled away or the wire is cut
- Live status on the phone and on a 0.96" OLED

## Components
| Part | Approx. cost (₹) |
|---|---|
| ESP32 DevKit V1 | 350 |
| 0.96" SSD1306 OLED (I2C) | 200 |
| Active buzzer | 20 |
| Push button | 5 |
| Aluminium foil, foam sheet, cardboard | ~0 |

## Wiring
| ESP32 pin | Connects to |
|---|---|
| GPIO21 | OLED SDA |
| GPIO22 | OLED SCL |
| 3.3V / GND | OLED VCC / GND |
| GPIO25 | Buzzer + (buzzer − to GND) |
| GPIO27 | Push button (other leg to GND) |
| GPIO13 | Foil sheet A (foil sheet B to GND) |
| GPIO26 | Tamper wire loop (other end to GND) |

**Mat sensor:** two foil sheets on cardboard with a 3–5 mm foam sheet (with holes cut in it) between them. Stepping on the mat pushes the foils together.

## Uploading the firmware
1. Install the libraries **Adafruit SSD1306** and **Adafruit GFX** from the Arduino Library Manager.
2. Board: **ESP32 Dev Module**.
3. Tools → Partition Scheme → **Huge APP (3MB No OTA/1MB SPIFFS)**.
4. Open `firmware/SmartDoormat_BLE/SmartDoormat_BLE.ino` and upload.
5. Not using the tamper wire? Set `USE_TAMPER_LOOP` to `0`.

## Bluetooth commands
| Command | Action |
|---|---|
| `ARM` | Start arming (15 s to leave) |
| `DISARM 1234` | Disarm / stop alarm / reset tamper (1234 = PIN) |
| `STATUS` | Get current status |

The PIN and Bluetooth name can be changed at the top of the code (`BT_PIN`, `BT_NAME`).

## Secret tap code
**TAP TAP … pause (1–2.5 s) … TAP TAP**: arms the mat when disarmed, and disarms it during the entry window or alarm.
