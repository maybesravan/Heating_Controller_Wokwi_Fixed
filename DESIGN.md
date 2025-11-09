# Heating Controller – Design & Embedded Implementation

**Target platform:** ESP32 DevKit (Arduino core on Wokwi)  
**Sensors:** TMP36 analog temperature sensor (min viable), optional: NTC thermistor / DS18B20  
**Actuators:** “Heater” (represented by MOSFET→LED in Wokwi), Buzzer, Status LED  
**Comms:** BLE Advertising (current state)  
**Scheduling:** FreeRTOS task for periodic sampling and control

---

## 1) Minimum Sensors for Heating Detection & Control

**Required (minimum):**
- **Temperature sensor** (e.g., TMP36, or NTC thermistor with ADC): measures process temperature for feedback control.

**Recommended (robustness):**
- **Over-temperature cutoff / secondary sensor**: a second thermistor or thermal fuse to provide independent overheat protection.
- **Supply voltage monitor** (optional): to detect brownouts and disable heater safely.
- **Ambient/reference sensor** (optional): improves stability with changing ambient conditions.

**Why TMP36 here?**
- Easy analog interface on ESP32 (ADC).
- Linear °C conversion with a simple formula.
- Supported by Wokwi for quick simulation.

> In hardware, you can swap TMP36 for an NTC or DS18B20 with minor code changes.

---

## 2) Communication Protocol Recommendation

**Chosen:** **BLE Advertising** (Bluetooth Low Energy)
- **Justification:**  
  - **Low power** & ubiquitous on phones/PCs.  
  - **No pairing required** for basic presence/state broadcasting (advertising payload).  
  - **Simple field deployment**: apps (nRF Connect, LightBlue, etc.) can read state.  
  - **ESP32 support is mature** in Arduino core.

**Alternatives (trade‑offs):**
- **Wi‑Fi (HTTP/MQTT):** richer telemetry + OTA, but higher power and setup complexity.
- **UART/USB:** trivial during bring‑up, tethered.
- **RS‑485/Modbus:** good for industrial wiring, needs external transceiver.

---

## 3) Block Diagram

```
+-------------------------------+
|           ESP32 SoC           |
|                               |
|  +-----------+   +----------+ |
|  |  ADC      |   |  GPIO    | |
|  |  TMP36    |   | Heater   |----> MOSFET/Relay -> Heater (LED in Wokwi)
|  |  input    |   | Control  | |
|  +-----------+   +----------+ |
|        |                |     |
|   +----v----+       +---v---+ |
|   |  Filter |       |  LED  |----> Visual state (optional)
|   +----+----+       +---+---+ |
|        |                |     |
|   +----v----+           |     |
|   |  FSM     |<---------+     |
|   | (Idle/Heating/            |
|   |  Stabilizing/Target/Over) |
|   +----+----+                 |
|        |                      |
|   +----v-----+         +------+
|   |  Logger  |  USB    | BLE  |
|   | (Serial) |<------->| Adv. |
|   +----------+         +------+
+-------------------------------+
```

---

## 4) Future Roadmap

**Overheating protection:**
- Dual‑channel temperature sensing (process + chassis).  
- **Hardware failsafe:** thermal fuse / bimetal cutoff in series with heater.  
- **Software guard rails:**  
  - Max ramp rate, max on‑time watchdog.  
  - Latched fault state with manual reset.  
  - Sensor plausibility checks (stuck-at, open/short).

**Multiple heating profiles:**
- Profile = {target, ramp rate, hold time, hysteresis, max overshoot}.  
- Store in **NVS** (ESP32 flash).  
- Simple **BLE or Serial menu** to select/activate a profile.  
- Add **PID** controller option; start with hysteresis, upgrade to PI/PID for tighter control.

**Telemetry & UX:**
- BLE GATT service for live temperature/state.  
- Wi‑Fi + MQTT/HTTP + web dashboard.  
- OTA updates, secure provisioning (BLE/Wi‑Fi).

**Safety/Compliance:**
- Galvanic isolation (opto/MOSFET driver), proper creepage/clearance.  
- IEC/UL thermal tests, enclosure design, fire‑safe materials.

---

# Part 2: Embedded Implementation (Wokwi / ESP32 Arduino)

## Functional States
- **Idle:** Heater OFF, waiting for start or temperature below lower bound.
- **Heating:** Heater ON until `target - hysteresis_low` reached.
- **Stabilizing:** Narrow control around target for a settling interval.
- **Target Reached:** Within ±band for N seconds; maintain with hysteresis.
- **Overheat:** If `temp >= overheatThreshold` or sensor fault → heater OFF, alarm.

## Control Logic
- Read temperature at 10 Hz, filter with moving average.
- Compare against thresholds:
  - **Start heating** when temp < `target - hysteresis_low`.
  - **Stop heating** when temp > `target + hysteresis_high`.
- If within `±stabilityBand` for `stabilityTimeMs` → **Target Reached**.
- Any time temp >= `overheatThreshold` → **Overheat** (latched until reset).

## Timers/Tasks
- FreeRTOS task `controlTask` at 100 ms period handles: read → filter → FSM → outputs → log.
- Optional tone/LED feedback per state.

## Serial Log Format
CSV: `millis,tempC,state,heater`

Example:
```
12345,36.42,Heating,1
12445,36.55,Heating,1
...
```

## BLE Advertising
- Updates manufacturer data with ASCII state (e.g., `"HEAT:Heating"`).  
- Advertising is passive; no pairing needed.  
- Update when state changes or every ~2 s.

---

## Wokwi Bill of Materials (simulation)

- **esp32-devkit-v1** (controller)
- **tmp36** temperature sensor → ADC (GPIO 34)
- **led** as heater indicator → GPIO 25 via MOSFET symbol is optional; we drive LED directly in sim
- **buzzer** (active) → GPIO 27
- **led** status (onboard GPIO 2 or external) → GPIO 2

Wiring is encoded in `wokwi/diagram.json` and matches the pin defines in the code.

---

## Build & Run (Locally and Wokwi)

1. Open **Wokwi** → “New Project” → “ESP32 Arduino”.  
2. Replace the default `sketch.ino` with `src/main.ino`.  
3. Add files from `wokwi/diagram.json` (Wokwi supports adding the diagram).  
4. Click **Run**. Open **Serial Monitor** @ **115200** baud to see logs.  
5. Use a BLE scanner on your phone/PC to see advertisement text (e.g., `HEAT:Heating`).

**Tuning:** edit constants at the top of `main.ino`: `TARGET_TEMP_C`, `HYST_LOW`, `HYST_HIGH`, `STABILITY_BAND`, `OVERHEAT_C`, etc.

---

## Testing Checklist

- [ ] Cold start below target → goes **Heating** (LED ON).  
- [ ] Approaches target → **Stabilizing** then **Target Reached** (LED toggles).  
- [ ] Force high temp (adjust sensor in Wokwi) → **Overheat** (heater OFF, buzzer).  
- [ ] Serial CSV logs visible.  
- [ ] BLE advertising shows current state.

---

## License
MIT
