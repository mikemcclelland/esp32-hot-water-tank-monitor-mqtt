# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32 firmware that reads a DS18B20 waterproof temperature probe and reports the hot water tank temperature to Home Assistant via MQTT auto-discovery.

Hardware: Keyestudio ESP-32 (KS5016) + DS18B20 waterproof probe.

## Build System

PlatformIO with Arduino framework. Install via the VS Code PlatformIO extension or `pip install platformio`.

```bash
pio run                      # build
pio run -t upload            # build + flash
pio run -t monitor           # open serial monitor (115200 baud)
pio run -t upload -t monitor # flash + monitor in one step
```

## Wiring

Each probe screws into a pluggable terminal adapter (Yellow→DAT, Red→VCC, Black→GND). The adapter's 3 pins connect to the matching column on the ESP32 board (S→GPIO, V→3.3V, G→GND). The adapters have a built-in 4.7kΩ pull-up resistor.

| Sensor | GPIO | Board column |
|---|---|---|
| RHS Tank Top | 14 | Io14 (DIGITAL section) |
| RHS Tank Bottom | 33 | Io33 (ANALOG section) |
| LHS Tank Top | 32 | Io32 (ANALOG section) |
| LHS Tank Bottom | 5 | Io5 (DIGITAL section) |

## Configuration

All settings live in `include/config.h` (gitignored — do not commit real credentials).

- **Step 1 (sensor only):** flash as-is; open serial monitor to see temperatures
- **Step 2 (Home Assistant):** set `ENABLE_WIFI true`, fill in WiFi SSID/password and MQTT broker IP/credentials, reflash

## Architecture

`src/main.cpp` is a single-file firmware with two compile-time modes controlled by `#define ENABLE_WIFI` in `config.h`:

- **ENABLE_WIFI false** — reads DS18B20 every 30 s via OneWire/DallasTemperature, prints `Tank temp: X.X°C` to serial
- **ENABLE_WIFI true** — also connects WiFi, connects to Mosquitto MQTT on Home Assistant, publishes an MQTT auto-discovery config (retained) so HA automatically creates a `temperature` sensor entity, then publishes readings to the state topic

MQTT auto-discovery uses the `homeassistant/sensor/tank_monitor/temperature/config` topic with a retained payload — HA picks this up automatically without any manual sensor configuration.

## Home Assistant Setup (Step 2 prerequisites)

1. Install **Mosquitto broker** add-on (Settings → Add-ons)
2. Create a dedicated MQTT user (Settings → People → Users)
3. Enable MQTT integration in HA (Settings → Devices & Services → Add Integration → MQTT)
4. Fill broker IP, user, and password into `config.h`

The sensor will appear automatically in HA as "Hot Water Tank Temperature" once the ESP32 publishes the discovery payload.
