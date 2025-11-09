/*
 * ESP32 Heating Controller (Wokwi)
 * States: Idle, Heating, Stabilizing, TargetReached, Overheat
 * Sensors: TMP36 on ADC (GPIO 34)
 * Actuators: Heater LED (GPIO 25), Buzzer (GPIO 27), Status LED (GPIO 2)
 * BLE Advertising: current state in manufacturer data
 * Scheduler: FreeRTOS periodic task (100 ms)
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

// ---------- PIN MAP ----------
static const int PIN_TMP36   = 34;  // ADC1_CH6
static const int PIN_HEATER  = 25;  // Heater driver (LED in Wokwi)
static const int PIN_BUZZER  = 27;  // Active buzzer
static const int PIN_LED     = 2;   // Onboard LED

// ---------- CONTROL CONSTANTS ----------
static const float TARGET_TEMP_C       = 50.0f;  // target temperature
static const float HYST_LOW            = 2.0f;   // heater ON below target - HYST_LOW
static const float HYST_HIGH           = 1.0f;   // heater OFF above target + HYST_HIGH
static const float STABILITY_BAND      = 0.5f;   // +/- band for "Target Reached"
static const uint32_t STABILITY_TIME_MS= 5000;   // time inside band to declare target
static const float OVERHEAT_C          = 65.0f;  // absolute cutoff

// Filtering
static const int FILTER_WINDOW = 10;             // moving average window (10 samples @ 100ms = 1s)

// ---------- BLE ----------
BLEAdvertising* pAdvertising = nullptr;
BLEServer* pServer = nullptr;
static const char* BLE_DEVICE_NAME = "ESP32-HeatCtrl";
// We use manufacturer data to broadcast the state text
std::string lastAdvPayload;

// ---------- FSM ----------
enum class HeatState {
  Idle = 0,
  Heating,
  Stabilizing,
  TargetReached,
  Overheat
};

const char* stateToStr(HeatState s) {
  switch (s) {
    case HeatState::Idle:           return "Idle";
    case HeatState::Heating:        return "Heating";
    case HeatState::Stabilizing:    return "Stabilizing";
    case HeatState::TargetReached:  return "Target";
    case HeatState::Overheat:       return "Overheat";
    default:                        return "Unknown";
  }
}

// ---------- GLOBALS ----------
volatile bool heaterOn = false;
HeatState state = HeatState::Idle;
float tempC = 25.0f;
float tempBuf[FILTER_WINDOW] = {0};
int bufIdx = 0;
int bufCount = 0;
uint32_t insideBandSince = 0;
bool faultLatched = false;
uint32_t lastAdvUpdate = 0;

// ---------- HELPERS ----------

// Read TMP36 temperature from ADC
float readTempTMP36() {
  int raw = analogRead(PIN_TMP36);            // 0..4095
  float v = (raw / 4095.0f) * 3.3f;           // ESP32 ADC ref ~3.3V
  // TMP36: 10 mV / °C with 500 mV offset at 0°C
  float t = (v - 0.5f) * 100.0f;              // °C
  return t;
}

// Simple moving average filter
float filterTemp(float newVal) {
  tempBuf[bufIdx] = newVal;
  bufIdx = (bufIdx + 1) % FILTER_WINDOW;
  if (bufCount < FILTER_WINDOW) bufCount++;
  float sum = 0.f;
  for (int i = 0; i < bufCount; i++) sum += tempBuf[i];
  return sum / bufCount;
}

void setHeater(bool on) {
  heaterOn = on && !faultLatched;
  digitalWrite(PIN_HEATER, heaterOn ? HIGH : LOW);
}

void beep(uint16_t ms) {
  digitalWrite(PIN_BUZZER, HIGH);
  delay(ms);
  digitalWrite(PIN_BUZZER, LOW);
}

// Update BLE advertising with current state
void updateAdvertising(const char* stateStr) {
  // Rate limit to ~2 s to avoid spamming
  uint32_t now = millis();
  if (now - lastAdvUpdate < 2000 && lastAdvPayload == stateStr) return;
  lastAdvUpdate = now;

  std::string payload = std::string("HEAT:") + stateStr;
  if (payload == lastAdvPayload) return;
  lastAdvPayload = payload;

  BLEAdvertisementData advData;
  // Manufacturer data is arbitrary (max ~24 bytes typical)
  advData.setManufacturerData(String(payload.c_str()));
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->start(); // restart to apply new data (safe on ESP32)
}

// Log CSV to Serial
void logCSV() {
  Serial.print(millis());
  Serial.print(',');
  Serial.print(tempC, 2);
  Serial.print(',');
  Serial.print(stateToStr(state));
  Serial.print(',');
  Serial.println(heaterOn ? 1 : 0);
}

// ---------- CONTROL TASK ----------
void controlTask(void* pv) {
  const TickType_t period = pdMS_TO_TICKS(100); // 100 ms
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&lastWake, period);

    // 1) Read + filter
    float t = readTempTMP36();
    tempC = filterTemp(t);

    // 2) Fault/Overheat checks
    // Basic sensor plausibility: TMP36 ~ -40..125C typical
    bool sensorFault = isnan(tempC) || tempC < -50.f || tempC > 150.f;
    if (sensorFault) faultLatched = true;
    if (tempC >= OVERHEAT_C) faultLatched = true;

    // 3) FSM transitions
    if (faultLatched) {
      state = HeatState::Overheat;
      setHeater(false);
      // short periodic beep (non-blocking alternative would be better;
      // keep it short here since we run every 100 ms)
      digitalWrite(PIN_BUZZER, (millis() / 250) % 2);
      digitalWrite(PIN_LED, (millis() / 250) % 2);
    } else {
      // Clear buzzer in normal states
      digitalWrite(PIN_BUZZER, LOW);

      switch (state) {
        case HeatState::Idle:
        case HeatState::TargetReached:
        case HeatState::Stabilizing: {
          // Start heating if too low
          if (tempC < TARGET_TEMP_C - HYST_LOW) {
            state = HeatState::Heating;
            setHeater(true);
            insideBandSince = 0;
          } else {
            // Check if we're in the target band
            float diff = fabsf(tempC - TARGET_TEMP_C);
            if (diff <= STABILITY_BAND) {
              if (insideBandSince == 0) insideBandSince = millis();
              if (millis() - insideBandSince >= STABILITY_TIME_MS) {
                state = HeatState::TargetReached;
                setHeater(false); // maintain by hysteresis
              } else {
                state = HeatState::Stabilizing;
              }
            } else {
              insideBandSince = 0;
              // Either above or below band; heater by hysteresis
              if (tempC > TARGET_TEMP_C + HYST_HIGH) {
                setHeater(false);
              } else if (tempC < TARGET_TEMP_C - HYST_LOW) {
                setHeater(true);
                state = HeatState::Heating;
              }
            }
          }
          break;
        }
        case HeatState::Heating: {
          if (tempC >= TARGET_TEMP_C + HYST_HIGH) {
            setHeater(false);
            state = HeatState::Stabilizing;
            insideBandSince = 0;
          }
          break;
        }
        case HeatState::Overheat:
        default:
          // handled above
          break;
      }

      // LED heartbeat/state cue
      bool led = false;
      switch (state) {
        case HeatState::Idle:          led = (millis() / 1000) % 2; break;      // 1 Hz blink
        case HeatState::Heating:       led = true; break;                        // solid on
        case HeatState::Stabilizing:   led = (millis() / 200) % 2; break;        // fast blink
        case HeatState::TargetReached: led = (millis() / 1500) % 2; break;       // slow blink
        case HeatState::Overheat:      led = (millis() / 250) % 2; break;        // alarm blink
      }
      digitalWrite(PIN_LED, led ? HIGH : LOW);
    }

    // 4) BLE adv + Serial log
    updateAdvertising(stateToStr(state));
    logCSV();
  }
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_HEATER, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_TMP36, INPUT);

  // ADC config
  analogReadResolution(12); // 0..4095
  // Default attenuation is fine for 0..3.3V for simulation

  // BLE init
  BLEDevice::init(BLE_DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x06);  // helps iOS
  pAdvertising->setMinPreferred(0x12);
  updateAdvertising(stateToStr(state));

  // Start control task
  xTaskCreatePinnedToCore(controlTask, "controlTask", 4096, nullptr, 1, nullptr, 1);

  // Power-on beep
  beep(80);
}

// ---------- LOOP ----------
void loop() {
  // Nothing: all work in FreeRTOS task
  vTaskDelay(pdMS_TO_TICKS(1000));
}

// ---------- Utilities ----------
// Optional serial command to clear fault (type 'R' to reset overheat latch)
void serialEvent() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'R' || c == 'r') {
      faultLatched = false;
      state = HeatState::Idle;
      setHeater(false);
      insideBandSince = 0;
      Serial.println("# Fault reset");
      beep(40);
    }
  }
}
