# ESP32 Hot Water Tank Monitor (MQTT)

ESP32 firmware that reads four DS18B20 waterproof temperature probes across two hot water tanks and reports them to Home Assistant via MQTT auto-discovery. No manual HA sensor configuration needed — the device announces itself on first boot.

## Features

- **4 probes** — two tanks × top/bottom, giving a stratification picture of each tank
- **MQTT auto-discovery** — appears as a *Hot Water Tanks* device in HA automatically
- **Web UI** — browser-based status page and MQTT settings editor at the device IP
- **OTA updates** — reflash over WiFi with `pio run -t upload --upload-port=<IP>`
- **LCD display** — optional 16×2 I²C LCD; shows live temps and rotates to the IP address every 5 s
- **Offline mode** — set `ENABLE_WIFI false` to test sensors over serial with no network needed
- **NVS persistence** — MQTT credentials survive reflashes; change them via the web UI without touching `config.h`

## Hardware

| Component | Part |
|---|---|
| Microcontroller | Keyestudio ESP-32 (KS5016) |
| Temperature sensors | DS18B20 waterproof probe × 4 |
| Probe adapters | Pluggable terminal adapter boards (DAT/VCC/GND, built-in 4.7 kΩ pull-up) |
| Display (optional) | 16×2 I²C LCD, address 0x27 |

No additional resistors or components needed — the adapters include the DS18B20 pull-up.

## Wiring

### Temperature probes

Each probe screws into a pluggable terminal adapter:

| Probe wire | Adapter terminal |
|---|---|
| Yellow | DAT |
| Red | VCC |
| Black | GND |

The adapter's three pins then connect to the matching column on the ESP32 board:

| Sensor | GPIO | Board column |
|---|---|---|
| RHS Tank Top | 14 | Io14 (DIGITAL section) |
| RHS Tank Bottom | 33 | Io33 (ANALOG section) |
| LHS Tank Top | 32 | Io32 (ANALOG section) |
| LHS Tank Bottom | 5 | Io5 (DIGITAL section) |

| Adapter pin | Board pin |
|---|---|
| S (signal) | GPIO column (e.g. Io14) |
| V (power) | 3.3V column |
| G (ground) | GND column |

### LCD display (optional)

| LCD pin | ESP32 pin |
|---|---|
| SDA | GPIO 21 |
| SCL | GPIO 22 |
| VCC | 5V |
| GND | GND |

The firmware auto-detects the LCD on startup and runs without it if not present.

## Setup

### Step 1 — Sensor only (no network)

Flash as-is and open the serial monitor to verify all probes are detected:

```
GPIO 14 (RHS Tank Top):    1 sensor(s)
GPIO 33 (RHS Tank Bottom): 1 sensor(s)
GPIO 32 (LHS Tank Top):    1 sensor(s)
GPIO  5 (LHS Tank Bottom): 1 sensor(s)

RHS Tank Top   : 22.5°C  (72.5°F)
RHS Tank Bottom: 21.0°C  (69.8°F)
LHS Tank Top   : 58.0°C  (136.4°F)
LHS Tank Bottom: 52.0°C  (125.6°F)
```

### Step 2 — WiFi + Home Assistant

1. Copy `include/config.h.example` to `include/config.h` and fill in your credentials
2. Set `ENABLE_WIFI true`
3. Flash and verify the device connects and the *Hot Water Tanks* device appears in HA

## Configuration

Copy `include/config.h.example` to `include/config.h` (gitignored — never committed):

```cpp
#define ENABLE_WIFI true

#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

#define MQTT_HOST "192.168.1.42"   // Home Assistant IP
#define MQTT_PORT 1883
#define MQTT_USER "mqtt_devices"
#define MQTT_PASS "your-mqtt-password"

#define READ_INTERVAL_MS 30000     // sensor poll interval (ms)
```

MQTT credentials can also be changed at runtime via the web UI — they persist in NVS and survive reflashes.

## Build & Flash

Requires [PlatformIO](https://platformio.org/) (VS Code extension or `pip install platformio`).

```bash
pio run                          # build only
pio run -t upload                # build + flash (USB)
pio run -t monitor               # open serial monitor (115200 baud)
pio run -t upload -t monitor     # flash + monitor in one step

# OTA (WiFi) — get the IP from the serial output or LCD
pio run -t upload --upload-port=192.168.1.x
```

## Home Assistant

### Prerequisites

1. **Mosquitto broker** add-on installed and running (Settings → Add-ons)
2. An MQTT user added in the Mosquitto add-on **Configuration → Logins** section
3. MQTT integration enabled (Settings → Devices & Services → Add Integration → MQTT)

### Auto-discovery

On first connect the ESP32 publishes a retained discovery payload for each probe. HA creates the *Hot Water Tanks* device automatically — no manual entity configuration needed. The four sensors appear under **Settings → Devices & Services → MQTT → Hot Water Tanks**.

## Web Interface

Once connected, the device hosts a small web UI at its IP address:

- **`/`** — live temperature table, MQTT status, auto-refreshes every 30 s
- **`/settings`** — edit MQTT host/port/user/password; saving restarts the device

## License

MIT
