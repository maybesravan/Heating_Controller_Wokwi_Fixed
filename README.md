# ESP32 Heating Controller (Wokwi)

A minimal heating controller demo for **ESP32 (Arduino)** with:
- Temperature FSM (Idle / Heating / Stabilizing / Target / Overheat)
- Hysteresis control
- Serial CSV logging
- **BLE advertising** of the current state
- Buzzer / LED feedback
- FreeRTOS task for periodic control

## Quick Start (Wokwi)
1. Create a new **ESP32 Arduino** project on Wokwi.
2. Copy **`src/main.ino`** into the sketch.
3. Add **`wokwi/diagram.json`** to define wiring (Wokwi → Project Files → Add).
4. Click **Run**, open **Serial Monitor** @ **115200**.
5. Scan BLE advertising with nRF Connect / LightBlue (`HEAT:<State>`).

## Hardware (simulated)
- ESP32 DevKit v1
- TMP36 → GPIO 34 (ADC)
- Heater LED → GPIO 25
- Buzzer → GPIO 27
- Status LED → GPIO 2 (onboard)

## Tuning
Edit constants at the top of `main.ino`:
- `TARGET_TEMP_C` (default 50 °C)
- `HYST_LOW`, `HYST_HIGH` (2 °C / 1 °C)
- `STABILITY_BAND` (±0.5 °C), `STABILITY_TIME_MS` (5000 ms)
- `OVERHEAT_C` (65 °C)

## Building Locally
Use Arduino IDE:
- Board: **ESP32 Dev Module**
- Port: your ESP32 COM port
- Upload, then open Serial Monitor @ 115200.

## License
MIT
