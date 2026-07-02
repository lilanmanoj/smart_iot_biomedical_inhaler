# Smart IoT Biomedical Inhaler

A PlatformIO + ESP32 Arduino project for a smart biomedical inhaler device. The ESP32 starts in Wi-Fi setup mode when no stored credentials are found, provides a browser-based setup form, stores Wi-Fi settings in non-volatile memory, and publishes sensor readings to Firebase Realtime Database.

## Overview

This firmware supports:

- ESP32 DevKit V1 as the main controller
- YF-S201C fluid flow sensor input
- Honeywell MPRLS0025PA00001A pressure sensor input (optional via build flag)
- Wi-Fi setup portal using AP mode and a simple HTML form
- Persistent Wi-Fi credential storage using Preferences
- Firebase Realtime Database publishing with placeholder credentials
- Pass/fail evaluation using configurable flow and pressure thresholds
- Non-blocking LED status signaling for pass/fail results

## Features

- Blue LED blinks during setup-connect mode and stays solid after successful Wi-Fi connection
- AP mode fallback when Wi-Fi credentials are missing or connection fails
- Browser-based Wi-Fi configuration page
- Real-time sensor publish to Firebase with a `passed` flag
- Configurable thresholds for flow rate and pressure
- Optional pressure sensor support via `PRESSURE_SENSOR_ENABLED`

## Dependencies

- PlatformIO
- ESP32 Arduino framework
- Firebase ESP Client by mobizt
- ArduinoJson

## Requirements

- ESP32 DevKit V1 board
- Fluid flow sensor YF-S201C
- Optional pressure sensor Honeywell MPRLS0025PA00001A
- Firebase Realtime Database project and API credentials
- PlatformIO CLI or IDE

## Pin Mapping

- `FLOW_PIN`: GPIO27 (flow sensor pulse input)
- `PRESSURE_PIN`: GPIO39 (pressure sensor ADC input)
- `LED_BLUE`: GPIO33 (Power/status LED blink/steady)
- `LED_GREEN`: GPIO25 (valid inhale indicator)
- `LED_RED`: GPIO32 (invalid inhale indicator)

## Setup

1. Copy `.env.example` to `.env`:

```bash
cp .env.example .env
```

2. Update `.env` with your values:

```env
WIFI_SETUP_SSID=YourSetupSSID
WIFI_SETUP_PASSWORD=YourSetupPassword
FIREBASE_DATABASE_URL=https://your-project.firebaseio.com/
FIREBASE_API_KEY=YOUR_API_KEY
FLOW_RATE_THRESHOLD=2.5
PRESSURE_THRESHOLD=5.0
PRESSURE_SENSOR_ENABLED=1
```

3. Ensure `.env` is ignored by Git. It is already added to `.gitignore`.

4. Load environment variables before building:

```bash
export $(grep -v '^#' .env | xargs)
```

## Build and Run

Upload the firmware to the ESP32:

```bash
pio run -e esp32-devkit-v1 -t upload
```

Monitor serial output:

```bash
pio device monitor -e esp32-devkit-v1
```

## Notes

- If `PRESSURE_SENSOR_ENABLED=0`, the firmware skips pressure sensor reads and only evaluates the flow threshold.
- Replace the placeholder Firebase URL and API key in `.env` with your actual Firebase configuration.
- Update the flow sensor calibration constant in `src/main.cpp` if needed for accurate readings.

## File structure

- `platformio.ini` — build configuration and environment mappings
- `src/main.cpp` — main firmware logic
- `.env.example` — environment variable template
- `.gitignore` — excludes `.pio` and `.env`
