// SMART HOME - single Arduino Uno (D0/D1 used, so NO Serial in this sketch)
// Keypad (4x3) + I2C LCD + magnetic switch + ultrasonic + servo + relay fan

#include <Wire.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------- Keypad (4 rows x 3 columns) ----------
const byte ROWS = 4, COLS = 3;
char keys[ROWS][COLS] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}        // * = clear, # = enter
};
byte rowPins[ROWS] = {2, 3, 4, 5};   // D2-D5
byte colPins[COLS] = {6, 7, 8};      // D6-D8
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ---------- Pins ----------
#define PIN_YELLOW  0    // D0  object-close LED
#define PIN_INDOOR  1    // D1  indoor light LED
#define PIN_TRIG    9    // D9  ultrasonic TRIG
#define PIN_ECHO    10   // D10 ultrasonic ECHO
#define PIN_MAGNET  11   // D11 magnetic switch to GND
#define PIN_RELAY   12   // D12 relay (fan)
#define PIN_SERVO   13   // D13 door servo
#define PIN_LDR     A0
#define PIN_TEMP    A1
#define PIN_POT     A2
#define PIN_ALARM   A3   // buzzer + red LED (shared)

// ---------- Settings ----------
const String CORRECT_CODE = "1234";          // opens the door, disarms, and ends a lockout
const bool AUTO_ARM = true;                  // armed automatically at startup
const int  MAX_TRIES = 3;                    // wrong codes before lockout
const bool OPEN_DOOR_ON_UNLOCK = false;

// *** IMPORTANT: Set to false for LM35 (real hardware), or true for TMP36 (Tinkercad) ***
const bool TEMP_IS_TMP36 = false;            

const int  RELAY_ON = HIGH;                  // use LOW if your relay module is active-low
const int  MOTION_DISTANCE_CM = 15;          // closer than this = yellow LED

// Tuned for your range: ~950 (no light) to ~1010 (light)
const int  LDR_THRESHOLD = 970;              // turns ON below 970 (Dark / ~950)
const int  LDR_HYST      = 20;               // turns OFF above 990 (Bright / ~1010)

const int  SERVO_CLOSED = 0;
const int  SERVO_OPEN   = 90;
const unsigned long DOOR_OPEN_TIME = 5000;
const unsigned long STARTUP_IGNORE = 3000;   // no alarm during the first 3 s
const unsigned long BEEP_TIME      = 250;    // alarm beep on/off time
const unsigned long PAGE_TIME      = 2000;
const int BEEP_FREQ = 800;                   // Hz

// ---------- State ----------
String inputCode = "", msg = "";
bool securityOn = AUTO_ARM, alarmActive = false, doorOpen = false, lockout = false;
bool lightOverride = false, motionNow = false, fanOn = false, indoorOn = false;
bool beepState = false;
int wrongTries = 0, hitCount = 0, distanceCm = 999, tempThreshold = 25;
int tempX10 = 250, ldrVal = 0, servoAngle = SERVO_CLOSED;
unsigned long msgTimer = 0, usTimer = 0, doorTimer = 0, servoTimer = 0, climateTimer = 0;
String lastLine0 = "", lastLine1 = "";

void setup() {
  digitalWrite(PIN_RELAY, !RELAY_ON);        // fan off before the pin becomes an output
  pinMode(PIN_RELAY, OUTPUT);

  pinMode(PIN_YELLOW, OUTPUT);
  pinMode(PIN_INDOOR, OUTPUT);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_MAGNET, INPUT_PULLUP);         // switch closed = LOW, broken = HIGH
  pinMode(PIN_SERVO, OUTPUT);
  pinMode(PIN_ALARM, OUTPUT);

  keypad.setDebounceTime(10);

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart Home Ready");
}

void loop() {
  handleKeypad();
  handleDoor();
  servoRefresh();
  readUltrasonic();
  handleSecurity();
  handleLighting();
  handleClimate();
  updateDisplay();
}

// true = magnetic switch connected (calm), false = broken (trigger)
bool magnetOk() {
  return digitalRead(PIN_MAGNET) == LOW;
}

// ---------- 1. Keypad + door ----------
void handleKeypad() {
  char k = keypad.getKey();
  if (!k) return;

  if (k >= '0' && k <= '9') {
    if (inputCode.length() < 8) inputCode += k;
  }
  else if (k == '*') { inputCode = ""; msg = ""; }
  else if (k == '#') {
    if (inputCode == CORRECT_CODE) {
      if (lockout) {                         // correct code ends the lockout
        lockout = false;
        alarmActive = false;
        securityOn = false;
        wrongTries = 0;
        msg = "System normal";
        if (OPEN_DOOR_ON_UNLOCK) { doorOpen = true; doorTimer = millis(); }
      } else {                               // open the door and disarm
        securityOn = false;
        alarmActive = false;
        doorOpen = true;
        doorTimer = millis();
        wrongTries = 0;
        msg = "Access granted";
      }
    }
    else if (inputCode == "1") {             // arm / disarm
      if (lockout) msg = "Locked out";
      else if (securityOn) { securityOn = false; alarmActive = false; msg = "Security OFF"; }
      else if (magnetOk()) { securityOn = true; msg = "System armed"; }
      else msg = "Switch is open";
    }
    else if (inputCode == "2") {             // manual indoor light
      if (lockout) msg = "Locked out";
      else { lightOverride = !lightOverride; msg = lightOverride ? "Light ovr ON" : "Light ovr OFF"; }
    }
    else if (inputCode == "3") {             // reset alarm
      if (lockout) msg = "Locked out";
      else { alarmActive = false; msg = "Alarm reset"; }
    }
    else {                                   // wrong code
      msg = "Wrong code";
      if (!lockout) {
        wrongTries++;
        if (wrongTries >= MAX_TRIES) {
          lockout = true;
          alarmActive = false;
          doorOpen = false;
          msg = "LOCKED OUT";
        }
      }
    }
    msgTimer = millis();
    inputCode = "";
  }
}

void handleDoor() {
  if (doorOpen && millis() - doorTimer >= DOOR_OPEN_TIME) doorOpen = false;
  servoAngle = doorOpen ? SERVO_OPEN : SERVO_CLOSED;
}

// One servo pulse every 20 ms (no Servo.h needed)
void servoRefresh() {
  if (millis() - servoTimer < 20) return;
  servoTimer = millis();
  int us = map(servoAngle, 0, 180, 544, 2400);
  digitalWrite(PIN_SERVO, HIGH);
  delayMicroseconds(us);
  digitalWrite(PIN_SERVO, LOW);
}

// ---------- 2. Ultrasonic (yellow LED only) ----------
void readUltrasonic() {
  if (millis() - usTimer < 100) return;
  usTimer = millis();

  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long d = pulseIn(PIN_ECHO, HIGH, 30000UL);
  distanceCm = (d == 0) ? 999 : (int)(d * 0.034 / 2);

  if (distanceCm < MOTION_DISTANCE_CM) { if (hitCount < 3) hitCount++; }
  else hitCount = 0;
  motionNow = (hitCount >= 3);
}

// ---------- 3. Security ----------
void handleSecurity() {
  digitalWrite(PIN_YELLOW, motionNow);

  // Magnetic switch broken while armed -> alarm
  bool started = millis() > STARTUP_IGNORE;
  if (securityOn && started && !lockout && !magnetOk()) alarmActive = true;

  // Alarm / lockout: buzzer and red LED pulse together on one pin
  bool sounding = alarmActive || lockout;
  bool beep = sounding && ((millis() / BEEP_TIME) % 2 == 0);
  if (beep != beepState) {
    beepState = beep;
    if (beep) tone(PIN_ALARM, BEEP_FREQ); else noTone(PIN_ALARM);
  }
}

// ---------- 4. Indoor lighting ----------
void handleLighting() {
  analogRead(PIN_LDR); // Dummy read to settle ADC multiplexer

  // Average 8 readings: the useful range is narrow, so smooth out noise
  long sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(PIN_LDR);
  ldrVal = sum / 8;

  static bool darkDetected = false;

  // Turns ON when no light (< 970), turns OFF when light is present (> 990)
  if (ldrVal < LDR_THRESHOLD) darkDetected = true;
  else if (ldrVal > LDR_THRESHOLD + LDR_HYST) darkDetected = false;

  indoorOn = darkDetected || lightOverride;
  digitalWrite(PIN_INDOOR, indoorOn);
}

// ---------- 5. Climate (relay fan) ----------
void handleClimate() {
  // Read potentiometer and map from 0 to 50 degC
  analogRead(PIN_POT);
  tempThreshold = map(analogRead(PIN_POT), 0, 1023, 0, 50);

  // Sample temperature every 500ms for a stable display
  if (millis() - climateTimer >= 500) {
    climateTimer = millis();

    // Dummy read + settling delay so A0/A2 don't bleed into A1 (Temp)
    analogRead(PIN_TEMP);
    delayMicroseconds(100);

    // Average 8 readings to eliminate noise spikes
    long sumAdc = 0;
    for (int i = 0; i < 8; i++) {
      sumAdc += analogRead(PIN_TEMP);
    }
    long mv = (sumAdc / 8) * 5000L / 1023L;
    tempX10 = TEMP_IS_TMP36 ? (int)(mv - 500) : (int)mv;   // tenths of a degree C
    if (tempX10 < 0) tempX10 = 0;                          // clamp negative noise
  }

  // Directly turn ON when temp > Set, and OFF when temp <= Set
  if (tempX10 > tempThreshold * 10) {
    fanOn = true;
  } else {
    fanOn = false;
  }

  digitalWrite(PIN_RELAY, fanOn ? RELAY_ON : !RELAY_ON);
}

// ---------- 6. LCD (16x2) ----------
void printLine(byte row, String s) {
  while (s.length() < 16) s += ' ';
  if (s.length() > 16) s = s.substring(0, 16);

  if (row == 0 && s != lastLine0) { lcd.setCursor(0, 0); lcd.print(s); lastLine0 = s; }
  else if (row == 1 && s != lastLine1) { lcd.setCursor(0, 1); lcd.print(s); lastLine1 = s; }
}

void updateDisplay() {
  if (msg.length() > 0 && millis() - msgTimer >= 3000) msg = "";

  bool page = (millis() / PAGE_TIME) % 2;
  if (inputCode.length() > 0) page = false;

  if (lockout) {
    printLine(0, "SYSTEM LOCKED");
    if (inputCode.length() > 0)  printLine(1, "Code:" + inputCode);
    else if (msg.length() > 0)   printLine(1, msg);
    else                         printLine(1, "Enter code");
    return;
  }

  if (!page) printLine(0, String("Sec:") + (securityOn ? "ARM" : "OFF") + " Door:" + (doorOpen ? "OPN" : "CLS"));
  else       printLine(0, String("T:") + String(tempX10 / 10.0, 1) + "C Set:" + tempThreshold + "C");

  if (alarmActive)                  printLine(1, "ALARM: WINDOW");
  else if (inputCode.length() > 0)  printLine(1, "Code:" + inputCode);
  else if (msg.length() > 0)        printLine(1, msg);
  else if (!page) {
    String dist = (distanceCm >= 999) ? String("--") : String(distanceCm);
    printLine(1, String("SW:") + (magnetOk() ? "OK  " : "OPEN") + " D:" + dist + "cm");
  } else {
    // Shows Fan state, Lamp state, AND live LDR reading (L:xxx) so you can see it live!
    printLine(1, String("F:") + (fanOn ? "ON " : "OFF") + " L:" + (indoorOn ? "ON " : "OF ") + ldrVal);
  }
}