#include <Arduino.h>
#include <ESP32Servo.h>

// ---------------------------------------------------------
// ESP32-S3 Pin Definitions
// ---------------------------------------------------------
const int IN_C_PIN = 16;     // center
const int IN_U_PIN = 21;     // up
const int IN_D_PIN = 38;     // down
const int IN_L_PIN = 18;     // left
const int IN_R_PIN = 17;     // right
const int LED_R_PIN = 8;    // common-anode red LED, active LOW
const int ESC_PWM_PIN = 15; // -> comparator U6 -> J5 ESC signal

// ---------------------------------------------------------
// ESC parameters
// ---------------------------------------------------------
const int ESC_MIN_US = 1000;      // assumed minimum / idle command
const int ESC_MAX_US = 2000;      // assumed ESC full-scale reference
const int ESC_TEST_MAX_US = 2000; // deliberately lower test limit
const int THROTTLE_STEP_US = 100;

// A button press or release is only confirmed when the button's signal
// level remains stable and consistent for at least 50 ms
const unsigned long DEBOUNCE_MS = 50;
const unsigned long ARM_HOLD_MS = 1000;
const unsigned long RAMP_INTERVAL_MS = 20;
const unsigned long STARTUP_WAIT_MS = 5000;

Servo esc;

// More direct than the original 0..255: a pulse width, in microseconds.
int throttleUs = ESC_MIN_US;       // desired output
int actualPulseUs = ESC_MIN_US;    // smoothly ramped output
bool enabled = false;              // check if it's able to change the throttle

unsigned long timeOffset = 0;
unsigned long startMs = 0;
unsigned long lastRampMs = 0;
unsigned long lastPrintMs = 0;

// ---------------------------------------------------------
// Button state and debouncing
// ---------------------------------------------------------
struct Button {
  int pin; // pin number
  bool rawPressed = false; // Initial press state, records the most recently read button state from the IO.
  bool pressed = false; // After debouncing, determine whether the button has indeed been pressed.
  bool justPressed = false; // Depends on bool 'pressed'
  bool justReleased = false; // Depends on bool 'pressed'
  unsigned long rawChangedMs = 0; // Raw changing time, use to record the time when the IO state last changed
  unsigned long pressedMs = 0; // Use to calculate how long a button has been held down
};

// Setup new button node with pin number
Button upButton{IN_U_PIN};
Button downButton{IN_D_PIN};
Button leftButton{IN_L_PIN};
Button rightButton{IN_R_PIN};
Button centerButton{IN_C_PIN};
bool centerWasHeldAtBoot = false;

// Setup the initial state of the buttons
void beginButton(Button &b) {
  pinMode(b.pin, INPUT_PULLUP); // All the buttons are active LOW
  b.rawPressed = (digitalRead(b.pin) == LOW); // Read the initial button state. If LOW, rawPressde = true
  b.pressed = b.rawPressed; // b.pressed is the debouncing value compare to b.rawPressed
  b.rawChangedMs = millis(); // Record the time when the button got initialized
  b.pressedMs = b.rawChangedMs; // Initialize the press timestamp
}

// Applied Debouncing Check
// loop() keeps calling this func to check the curr button's signal
// A button press or release is only confirmed when its signal level 
// remains stable and consistent for at least 50ms.
void updateButton(Button &b, unsigned long now) {
  // initiates the status from the last updating
  b.justPressed = false;
  b.justReleased = false;
  // reads the button's signal value (not debouncing yet)
  bool raw = (digitalRead(b.pin) == LOW);

  // the button's signal now is different from its signal before
  if (raw != b.rawPressed) {
    b.rawPressed = raw;
    b.rawChangedMs = now;
  }

  // only when the button's new signal is stable for 50ms
  // then the button is considered either be pressed or released
  if (raw != b.pressed && now - b.rawChangedMs >= DEBOUNCE_MS) {
    b.pressed = raw;

    // raw is true when the button is pressed
    if (raw) {
      b.justPressed = true;
      b.pressedMs = now;
    } else {
      // or the button is released
      b.justReleased = true;
    }
  }
}

// ---------------------------------------------------------
// Original-style throttle functions
// ---------------------------------------------------------

// increases the width of the pulse (period of the pulse)
void incThrottle() {
  if (!enabled) 
    return;
 
  throttleUs += THROTTLE_STEP_US;
  if (throttleUs > ESC_TEST_MAX_US)
    throttleUs = ESC_TEST_MAX_US;
  Serial.printf("UP: target = %d us\n", throttleUs);
}

// decreases the width of the pulse
void decThrottle() {
  if (!enabled) 
    return;

  throttleUs -= THROTTLE_STEP_US;
  if (throttleUs < ESC_MIN_US) 
    throttleUs = ESC_MIN_US;
  Serial.printf("DOWN: target = %d us\n", throttleUs);
}

// resets the throttle to minimum (ESC_MIN_US = 1000 us)
void rstThrottle() {
  enabled = false;
  throttleUs = ESC_MIN_US;
  actualPulseUs = ESC_MIN_US;
  // output the ctrl pulse
  esc.writeMicroseconds(ESC_MIN_US);
  digitalWrite(LED_R_PIN, HIGH); // red indicator OFF
}

// The immediate max mode
void setThrottle() {
  if (!enabled)
    return;

  throttleUs = ESC_MAX_US;
  digitalWrite(LED_R_PIN, LOW); // red indicator ON
}

// resets the time offset
void rstTime() {
  timeOffset = millis();
  Serial.println("CENTER: time reset");
}

// the most important control func
// reads the 5 buttons
// inc, dec, rst throttles
// control the enable value
// when to reset the timer
void processButtons(unsigned long now) {
  // Debouncing check
  updateButton(upButton, now);
  updateButton(downButton, now);
  updateButton(leftButton, now);
  updateButton(rightButton, now);
  updateButton(centerButton, now);

  // Immediate software stop on either LEFT or RIGHT; this is NOT a
  // physical emergency cutoff. Ignore enable commands while held.
  if (digitalRead(IN_R_PIN) == LOW) {
    if (enabled) 
      Serial.println("STOP: RIGHT pressed");

    rstThrottle();
    return;
  }

  // increase the throttle to max gradually
  if (digitalRead(IN_L_PIN) == LOW) {
    if (enabled){
      Serial.println("MAX: LEFT pressed");
      setThrottle();
    } else
      Serial.println("MAX FALSE: need to be enabled");
  }

  // Handle the center button when it is released.
  // If the center button was held during boot,
  // ignore its first release to prevent accidental enabling.
  if (centerButton.justReleased) {
    if (centerWasHeldAtBoot) {
      centerWasHeldAtBoot = false;
    } 
    // the button should be pressed for 2s to change the enable status
    else if (now - centerButton.pressedMs >= ARM_HOLD_MS) {
      
      // If it was enable before, disable it
      if (enabled) {
        rstThrottle();
        Serial.println("Control DISABLED");
      } 

      // Cannot be enabled in the first 5 seconds after the boot
      // Up and Down should also be released
      else if (now - startMs >= STARTUP_WAIT_MS &&
                 !upButton.pressed && !downButton.pressed) {
        throttleUs = ESC_MIN_US;
        actualPulseUs = ESC_MIN_US;
        esc.writeMicroseconds(ESC_MIN_US);
        enabled = true;
        digitalWrite(LED_R_PIN, LOW); // red indicator ON
        Serial.println("Control ENABLED at minimum throttle");
      } 
      
      else {
        Serial.println("Cannot enable yet: wait for startup / release UP and DOWN");
      }
    } 
    
    // Short press reset the timer
    else {
      rstTime();
    }
  }

  if (!enabled || centerButton.pressed) 
    return;

  // Change the throttle only if it's enabled
  if (downButton.justPressed) 
    decThrottle();
  else if (upButton.justPressed) 
    incThrottle();
}

// Update the throttle gradually
void updateESC(unsigned long now) {

  // The throttle can be changed each 20ms
  if (now - lastRampMs < RAMP_INTERVAL_MS) 
    return;

  lastRampMs = now;

  if (!enabled) {
    actualPulseUs = ESC_MIN_US;
  } else if (actualPulseUs < throttleUs) {
    actualPulseUs += THROTTLE_STEP_US;

    if (actualPulseUs > throttleUs) 
      actualPulseUs = throttleUs;
  } else {
    actualPulseUs = throttleUs; // slowing down is immediate
  }
  esc.writeMicroseconds(actualPulseUs);
}

// ---------------------------------------------------------
// Setup
// ---------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(LED_R_PIN, OUTPUT);
  digitalWrite(LED_R_PIN, HIGH);

  beginButton(upButton);
  beginButton(downButton);
  beginButton(leftButton);
  beginButton(rightButton);
  beginButton(centerButton);
  centerWasHeldAtBoot = centerButton.pressed;

  // Start control signal low; after attach, promptly send minimum pulse.
  pinMode(ESC_PWM_PIN, OUTPUT);
  digitalWrite(ESC_PWM_PIN, LOW);

  // Send a pulse each 20ms(50Hz)
  esc.setPeriodHertz(50);

  // Bind the ESC control object to a specified ESP32-S3 GPIO
  esc.attach(ESC_PWM_PIN, ESC_MIN_US, ESC_MAX_US);
  
  if (!esc.attached()) {
    Serial.println("ERROR: ESP32Servo attach failed");
    // If not attached, it will never start the program
    while (true) 
      delay(1000);
  }

  esc.writeMicroseconds(ESC_MIN_US);
  rstThrottle();

  startMs = millis();
  timeOffset = startMs;
  lastRampMs = startMs;
  Serial.println("ESC CONTROL READY - test signal with ESC POWER OFF first");
  Serial.printf("Hold CENTER %lu ms, release -> enable/disable; short CENTER -> reset time", ARM_HOLD_MS);
  Serial.printf("UP/DOWN +/- %d us; LEFT/RIGHT = software stop", THROTTLE_STEP_US);
}

// ---------------------------------------------------------
// Loop
// ---------------------------------------------------------
void loop() {
  unsigned long now = millis();
  processButtons(now);
  updateESC(now);

  if (now - lastPrintMs >= 500) {
    lastPrintMs = now;
    Serial.printf("time=%lu ms, enabled=%d, target=%d us, actual=%d us\n",
                  now - timeOffset, enabled ? 1 : 0, throttleUs, actualPulseUs);
  }
  delay(1);
}
