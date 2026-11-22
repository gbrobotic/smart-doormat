/*
  ============================================================
   SMART ANTI-THEFT DOORMAT  -  ESP32  (Bluetooth BLE version)
  ============================================================
   Features:
    1. Step detection with a homemade foil pressure switch (no piezo)
       (optional: ESP32 capacitive touch with a single foil sheet)
    2. Arm / Disarm with:
         - Bluetooth (BLE) web page in Chrome  (doormat_control.html)
         - "Serial Bluetooth Terminal" Android app
         - Push button
         - Secret tap code on the mat
    3. Disarm over Bluetooth needs a PIN (default 1234)
    4. Exit delay   - 15 s to leave after arming
    5. Entry window - 8 s to tap the secret code before the alarm starts
    6. Intruder alarm - buzzer + OLED + LED, auto re-arm after 60 s
    7. Visitor chime - short beep when someone steps on the mat while disarmed
    8. Loitering alert - someone standing on the mat for more than 10 s
    9. Tamper alert - mat wire cut / mat pulled away (works even when disarmed)
   10. Live status sent to the phone over Bluetooth
   11. OLED self-repair (re-initialised every 5 s if it goes blank)

   Bluetooth commands (send as text):
     ARM            -> start arming (15 s exit delay)
     DISARM 1234    -> disarm / stop alarm / reset tamper (1234 = your PIN)
     STATUS         -> get current status

   Secret tap code (default):  TAP TAP ...pause... TAP TAP

   Libraries (Arduino Library Manager): Adafruit SSD1306, Adafruit GFX Library
   BLE library is built into the ESP32 board package.
   Board: "ESP32 Dev Module"
   Tools > Partition Scheme > "Huge APP (3MB No OTA/1MB SPIFFS)"
  ============================================================
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ===================== USER SETTINGS =====================
#define USE_TOUCH_SENSOR  0   // 0 = foil pressure switch (recommended), 1 = ESP32 touch pin
#define USE_TAMPER_LOOP   1   // 1 = tamper wire connected, 0 = not using tamper

const char* BT_NAME = "SmartDoormat";   // Bluetooth name shown on the phone
const char* BT_PIN  = "1234";           // PIN needed to DISARM over Bluetooth

// ---------- Pins (ESP32 DevKit V1) ----------
const int PIN_MAT_SWITCH = 13;   // foil sheet A (foil sheet B -> GND)
const int PIN_MAT_TOUCH  = 4;    // T0, only used if USE_TOUCH_SENSOR = 1
const int PIN_BUZZER     = 25;   // active buzzer (+)
const int PIN_BUTTON     = 27;   // push button -> GND
const int PIN_TAMPER     = 26;   // tamper wire loop -> GND
const int PIN_LED        = 2;    // on-board LED
// OLED: SDA = 21, SCL = 22

// ---------- OLED ----------
const int     SCREEN_W  = 128;
const int     SCREEN_H  = 64;
const uint8_t OLED_ADDR = 0x3C;

// ---------- Timings (milliseconds) ----------
const unsigned long EXIT_DELAY_MS      = 15000;
const unsigned long ENTRY_WINDOW_MS    = 8000;
const unsigned long ALARM_DURATION_MS  = 60000;
const unsigned long LOITER_MS          = 10000;
const unsigned long LOITER_REPEAT_MS   = 5000;
const unsigned long VISITOR_MSG_MS     = 3000;
const unsigned long MAT_DEBOUNCE_MS    = 40;
const unsigned long BTN_DEBOUNCE_MS    = 50;
const unsigned long TAMPER_DEBOUNCE_MS = 300;
const unsigned long OLED_CHECK_MS      = 5000;

// ---------- Secret tap code ----------
const unsigned long TAP_MAX_PRESS_MS = 600;
const unsigned long SHORT_GAP_MAX_MS = 700;
const unsigned long LONG_GAP_MIN_MS  = 1000;
const unsigned long LONG_GAP_MAX_MS  = 2500;

enum GapType { GAP_SHORT, GAP_LONG, GAP_BAD };
// Gaps between taps. {SHORT, LONG, SHORT} = TAP TAP ..pause.. TAP TAP
const GapType SECRET_CODE[] = { GAP_SHORT, GAP_LONG, GAP_SHORT };
const int     SECRET_GAPS   = sizeof(SECRET_CODE) / sizeof(SECRET_CODE[0]);

// ---------- Touch tuning (only if USE_TOUCH_SENSOR = 1) ----------
const float TOUCH_ON_RATIO  = 0.85;
const float TOUCH_OFF_RATIO = 0.92;
// =========================================================

// BLE "Nordic UART Service" - works with Web Bluetooth and terminal apps
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   // phone -> ESP32
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"   // ESP32 -> phone

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
bool oledOk = false;

enum State { DISARMED, ARMING, ARMED, ENTRY, ALERT, TAMPER };
State state = DISARMED;
unsigned long stateStart = 0;

// Mat
bool matPressed = false;
bool matRawLast = false;
unsigned long matRawChange = 0;
unsigned long pressStart = 0;
float touchBaseline = 0;

// Tap code
int tapCount = 0;
unsigned long lastTapRelease = 0;

// Misc
int  intrusionCount = 0;
bool showVisitor = false;
unsigned long lastVisitorMs = 0;
bool loitering = false;
unsigned long lastLoiterBeep = 0;
unsigned long alarmTimerStart = 0;
bool tamperActive = false;
unsigned long lastDraw = 0;

// BLE
BLEServer*         bleServer = nullptr;
BLECharacteristic* txChar    = nullptr;
volatile bool bleConnected    = false;
volatile bool bleJustConnected = false;
volatile bool cmdReady = false;
char cmdBuf[32];

// ===================== BUZZER (non-blocking) =====================
struct Buzzer {
  bool active = false;
  bool forever = false;
  int remaining = 0;
  unsigned int onMs = 0, offMs = 0;
  bool isOn = false;
  unsigned long t = 0;
};
Buzzer bz;

void buzzerStop() {
  bz.active = false;
  bz.isOn = false;
  digitalWrite(PIN_BUZZER, LOW);
}

void beep(int count, unsigned int onMs, unsigned int offMs) {
  bz.active = true;
  bz.forever = false;
  bz.remaining = count;
  bz.onMs = onMs;
  bz.offMs = offMs;
  bz.isOn = true;
  bz.t = millis();
  digitalWrite(PIN_BUZZER, HIGH);
}

void beepForever(unsigned int onMs, unsigned int offMs) {
  beep(1, onMs, offMs);
  bz.forever = true;
}

void updateBuzzer() {
  if (!bz.active) return;
  unsigned long now = millis();
  if (bz.isOn) {
    if (now - bz.t >= bz.onMs) {
      digitalWrite(PIN_BUZZER, LOW);
      bz.isOn = false;
      bz.t = now;
      if (!bz.forever && --bz.remaining <= 0) bz.active = false;
    }
  } else if (now - bz.t >= bz.offMs) {
    digitalWrite(PIN_BUZZER, HIGH);
    bz.isOn = true;
    bz.t = now;
  }
}

// ===================== BLUETOOTH =====================
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) {
    bleConnected = true;
    bleJustConnected = true;
  }
  void onDisconnect(BLEServer* s) {
    bleConnected = false;
    s->startAdvertising();          // let the phone reconnect
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    String v = String(c->getValue().c_str());
    if (!cmdReady) {                // handled later in loop()
      strncpy(cmdBuf, v.c_str(), sizeof(cmdBuf) - 1);
      cmdBuf[sizeof(cmdBuf) - 1] = 0;
      cmdReady = true;
    }
  }
};

// Send a text line to the phone (split into 20-byte BLE packets)
void bleSend(const char* msg) {
  if (!bleConnected || txChar == nullptr) return;
  char line[96];
  snprintf(line, sizeof(line), "%s\n", msg);
  size_t len = strlen(line);
  for (size_t i = 0; i < len; i += 20) {
    size_t n = (len - i < 20) ? (len - i) : 20;
    txChar->setValue((uint8_t*)(line + i), n);
    txChar->notify();
    delay(10);
  }
}

void setupBle() {
  BLEDevice::init(BT_NAME);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  BLEService* svc = bleServer->createService(NUS_SERVICE_UUID);

  txChar = svc->createCharacteristic(NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  txChar->addDescriptor(new BLE2902());

  BLECharacteristic* rxChar = svc->createCharacteristic(
      NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("Bluetooth ready: \"%s\"\n", BT_NAME);
}

// ===================== STATE =====================
const char* stateName(State s) {
  switch (s) {
    case DISARMED: return "DISARMED";
    case ARMING:   return "ARMING";
    case ARMED:    return "ARMED";
    case ENTRY:    return "ENTER CODE";
    case ALERT:    return "INTRUDER!";
    case TAMPER:   return "TAMPER!";
  }
  return "?";
}

// Status line format:  STATE | mat:free | intr:0 | tamper:OK
void sendStatus() {
  char msg[80];
  snprintf(msg, sizeof(msg), "%s | mat:%s | intr:%d | tamper:%s",
           stateName(state), matPressed ? "PRESSED" : "free",
           intrusionCount, tamperActive ? "BROKEN" : "OK");
  bleSend(msg);
}

void setState(State s) {
  state = s;
  stateStart = millis();
  tapCount = 0;
  loitering = false;
  showVisitor = false;

  switch (s) {
    case DISARMED: buzzerStop(); beep(1, 60, 60);    break;
    case ARMING:   buzzerStop(); beep(1, 150, 100);  break;
    case ARMED:    buzzerStop(); beep(2, 80, 80);    break;
    case ENTRY:    beepForever(100, 900);            break;
    case ALERT:
      intrusionCount++;
      alarmTimerStart = millis();
      beepForever(250, 150);
      break;
    case TAMPER:   beepForever(600, 150);            break;
  }
  Serial.printf("[%lu] State -> %s\n", millis() / 1000, stateName(s));
}

// Push button
void userToggle() {
  switch (state) {
    case DISARMED: setState(ARMING); break;
    case TAMPER:   if (!tamperActive) setState(DISARMED); break;
    default:       setState(DISARMED); break;
  }
}

// Commands from the phone
void processBleCommand() {
  if (!cmdReady) return;
  String c = String(cmdBuf);
  cmdReady = false;
  c.trim();
  c.toUpperCase();
  Serial.printf("BT command: %s\n", c.c_str());

  if (c == "ARM") {
    if (state == DISARMED) { setState(ARMING); bleSend("OK: arming, leave now"); }
    else bleSend("ERR: already armed");
  } else if (c.startsWith("DISARM")) {
    String pin = c.substring(6);
    pin.trim();
    if (pin != BT_PIN)                         bleSend("ERR: wrong PIN");
    else if (state == TAMPER && tamperActive)  bleSend("ERR: fix tamper wire first");
    else { setState(DISARMED); bleSend("OK: disarmed"); }
  } else if (c == "STATUS") {
    sendStatus();
  } else if (c.length() > 0) {
    bleSend("Use: ARM | DISARM <pin> | STATUS");
  }
}

// Send status automatically whenever something changes
void updateBleStatus() {
  static State lastState = DISARMED;
  static bool  lastMat = false, lastTamper = false;
  static int   lastCount = -1;
  if (!bleConnected) return;
  if (bleJustConnected || state != lastState || matPressed != lastMat ||
      tamperActive != lastTamper || intrusionCount != lastCount) {
    if (bleJustConnected) { delay(300); bleJustConnected = false; }
    lastState = state; lastMat = matPressed;
    lastTamper = tamperActive; lastCount = intrusionCount;
    sendStatus();
  }
}

// ===================== MAT SENSOR =====================
#if USE_TOUCH_SENSOR
float readTouchAvg() {
  uint32_t sum = 0;
  for (int i = 0; i < 4; i++) sum += touchRead(PIN_MAT_TOUCH);
  return sum / 4.0;
}

void calibrateTouch() {
  float sum = 0;
  for (int i = 0; i < 64; i++) { sum += readTouchAvg(); delay(10); }
  touchBaseline = sum / 64.0;
  Serial.printf("Touch baseline: %.1f\n", touchBaseline);
}
#endif

bool readMatRaw() {
#if USE_TOUCH_SENSOR
  static bool st = false;
  float v = readTouchAvg();
  if (!st && v < touchBaseline * TOUCH_ON_RATIO)       st = true;
  else if (st && v > touchBaseline * TOUCH_OFF_RATIO)  st = false;
  if (!st) touchBaseline = touchBaseline * 0.995 + v * 0.005;
  return st;
#else
  return digitalRead(PIN_MAT_SWITCH) == LOW;
#endif
}

bool tapCodeActive() {
  return state == DISARMED || state == ENTRY || state == ALERT;
}

void handleTapPress(unsigned long now) {
  if (tapCount == 0) return;
  unsigned long gap = now - lastTapRelease;
  GapType g = GAP_BAD;
  if (gap <= SHORT_GAP_MAX_MS) g = GAP_SHORT;
  else if (gap >= LONG_GAP_MIN_MS && gap <= LONG_GAP_MAX_MS) g = GAP_LONG;
  if (g != SECRET_CODE[tapCount - 1]) tapCount = 0;
}

bool handleTapRelease(unsigned long now, unsigned long pressDur) {
  if (pressDur > TAP_MAX_PRESS_MS) { tapCount = 0; return false; }
  tapCount++;
  lastTapRelease = now;
  if (tapCount >= SECRET_GAPS + 1) { tapCount = 0; return true; }
  return false;
}

void onPress(unsigned long now) {
  switch (state) {
    case DISARMED:
      showVisitor = true;
      lastVisitorMs = now;
      beep(1, 80, 50);
      break;
    case ARMED:
      setState(ENTRY);
      break;
    default:
      break;
  }
  if (tapCodeActive()) handleTapPress(now);
}

void onRelease(unsigned long now) {
  unsigned long dur = now - pressStart;
  loitering = false;
  if (tapCodeActive() && handleTapRelease(now, dur)) {
    Serial.println("Secret tap code OK");
    if (state == DISARMED) setState(ARMING);
    else                   setState(DISARMED);
  }
}

void updateMat(unsigned long now) {
  bool raw = readMatRaw();
  if (raw != matRawLast) { matRawLast = raw; matRawChange = now; }
  if (raw != matPressed && now - matRawChange >= MAT_DEBOUNCE_MS) {
    matPressed = raw;
    if (matPressed) { pressStart = now; onPress(now); }
    else            { onRelease(now); }
  }
}

// ===================== BUTTON & TAMPER =====================
void updateButton(unsigned long now) {
  static bool lastRaw = HIGH, stable = HIGH;
  static unsigned long changed = 0;
  bool raw = digitalRead(PIN_BUTTON);
  if (raw != lastRaw) { lastRaw = raw; changed = now; }
  if (raw != stable && now - changed >= BTN_DEBOUNCE_MS) {
    stable = raw;
    if (stable == LOW) userToggle();
  }
}

void updateTamper(unsigned long now) {
#if USE_TAMPER_LOOP
  static bool lastRaw = false;
  static unsigned long changed = 0;
  bool raw = digitalRead(PIN_TAMPER) == HIGH;   // HIGH = loop broken
  if (raw != lastRaw) { lastRaw = raw; changed = now; }
  if (raw != tamperActive && now - changed >= TAMPER_DEBOUNCE_MS) {
    tamperActive = raw;
    Serial.println(tamperActive ? "Tamper: loop BROKEN" : "Tamper: loop OK");
    if (tamperActive && state != TAMPER) setState(TAMPER);
  }
#else
  (void)now;
#endif
}

// ===================== STATE MACHINE =====================
void updateStateMachine(unsigned long now) {
  switch (state) {
    case DISARMED:
      if (matPressed && now - pressStart >= LOITER_MS) {
        if (!loitering) { loitering = true; lastLoiterBeep = 0; bleSend("WARNING: someone waiting at door"); }
        if (lastLoiterBeep == 0 || now - lastLoiterBeep >= LOITER_REPEAT_MS) {
          lastLoiterBeep = now;
          beep(3, 120, 120);
        }
      }
      if (showVisitor && !matPressed && now - lastVisitorMs > VISITOR_MSG_MS) showVisitor = false;
      break;

    case ARMING:
      if (now - stateStart >= EXIT_DELAY_MS) setState(ARMED);
      break;

    case ENTRY:
      if (now - stateStart >= ENTRY_WINDOW_MS) setState(ALERT);
      break;

    case ALERT:
      if (matPressed) alarmTimerStart = now;
      if (now - alarmTimerStart >= ALARM_DURATION_MS) setState(ARMED);
      break;

    default:
      break;
  }
}

void updateLed(unsigned long now) {
  bool on = false;
  switch (state) {
    case DISARMED: on = false; break;
    case ARMING:   on = (now / 500) % 2; break;
    case ARMED:
    case ENTRY:    on = true; break;
    case ALERT:
    case TAMPER:   on = (now / 100) % 2; break;
  }
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

// ===================== OLED =====================
bool oledPresent() {
  Wire.beginTransmission(OLED_ADDR);
  return Wire.endTransmission() == 0;
}

void oledReinit() {
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, false, false);
}

void checkOled(unsigned long now) {
  static unsigned long lastCheck = 0;
  if (now - lastCheck < OLED_CHECK_MS) return;
  lastCheck = now;
  if (!oledPresent()) {
    oledOk = false;
    Serial.println("OLED NOT RESPONDING - check SDA/SCL/VCC wires");
    return;
  }
  bool wasOk = oledOk;
  oledReinit();
  if (!wasOk && oledOk) Serial.println("OLED reconnected");
}

void drawDisplay(unsigned long now) {
  if (!oledOk) return;
  char big[12] = "", l1[22] = "", l2[22] = "";
  unsigned long inState = now - stateStart;

  switch (state) {
    case DISARMED:
      if (loitering) {
        strcpy(big, "WAITING!");
        strcpy(l1, "Someone at the door");
        snprintf(l2, sizeof(l2), "Standing for %lus", (now - pressStart) / 1000);
      } else if (showVisitor) {
        strcpy(big, "VISITOR");
        strcpy(l1, "Step detected");
        snprintf(l2, sizeof(l2), "Intrusions: %d", intrusionCount);
      } else {
        strcpy(big, "DISARMED");
        strcpy(l1, "Tap code/btn/app arm");
        snprintf(l2, sizeof(l2), "Intrusions: %d", intrusionCount);
      }
      break;
    case ARMING:
      strcpy(big, "ARMING");
      snprintf(l1, sizeof(l1), "Leave in %lus", (EXIT_DELAY_MS - inState) / 1000 + 1);
      strcpy(l2, "Button = cancel");
      break;
    case ARMED:
      strcpy(big, "ARMED");
      strcpy(l1, "Mat is guarded");
      snprintf(l2, sizeof(l2), "Intrusions: %d", intrusionCount);
      break;
    case ENTRY:
      strcpy(big, "ENTER CODE");
      snprintf(l1, sizeof(l1), "Alarm in %lus", (ENTRY_WINDOW_MS - inState) / 1000 + 1);
      snprintf(l2, sizeof(l2), "Taps entered: %d", tapCount);
      break;
    case ALERT:
      strcpy(big, "INTRUDER!");
      if (matPressed) strcpy(l1, "Person ON the mat");
      else snprintf(l1, sizeof(l1), "Re-arm in %lus",
                    (ALARM_DURATION_MS - (now - alarmTimerStart)) / 1000 + 1);
      snprintf(l2, sizeof(l2), "Intrusions: %d", intrusionCount);
      break;
    case TAMPER:
      strcpy(big, "TAMPER!");
      strcpy(l1, tamperActive ? "Mat wire removed!" : "Wire OK again");
      strcpy(l2, "Press btn to reset");
      break;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("SMART DOORMAT");
  display.setCursor(104, 0);
  display.print(matPressed ? "[#]" : "[ ]");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(0, 16);
  display.print(big);

  display.setTextSize(1);
  display.setCursor(0, 36);
  display.print(l1);
  display.setCursor(0, 46);
  display.print(l2);
  display.setCursor(0, 56);
  display.print(bleConnected ? "BT: Connected" : "BT: Waiting...");
  display.display();
}

// ===================== SETUP & LOOP =====================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_MAT_SWITCH, INPUT_PULLUP);
#if USE_TAMPER_LOOP
  pinMode(PIN_TAMPER, INPUT_PULLUP);
#endif

  Wire.begin(21, 22);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!oledOk) Serial.println("OLED not found - check wiring/address");

  if (oledOk) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(0, 8);
    display.print("SMART");
    display.setCursor(0, 28);
    display.print("DOORMAT");
    display.setTextSize(1);
    display.setCursor(0, 52);
    display.print("Keep off the mat...");
    display.display();
  }

#if USE_TOUCH_SENSOR
  calibrateTouch();
#endif
  delay(1500);

  setupBle();
  delay(300);
  oledReinit();   // in case the OLED reset during Bluetooth start-up

  setState(DISARMED);
}

void loop() {
  unsigned long now = millis();
  processBleCommand();
  updateMat(now);
  updateButton(now);
  updateTamper(now);
  updateStateMachine(now);
  updateBuzzer();
  updateLed(now);
  updateBleStatus();
  checkOled(now);

  if (now - lastDraw >= 200) {
    lastDraw = now;
    drawDisplay(now);
  }
}
