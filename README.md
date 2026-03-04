# ClockHub Arduino

A smart wake-up light system built on the **Arduino UNO R4 WiFi**.

The device runs a local web server for schedule configuration, triggers a VoiceMonkey Alexa announcement before wake-up time, and activates a pump (connected to a grow light or similar device) at the scheduled alarm. Network presence is maintained via DuckDNS dynamic DNS.

## Features

- Weekly alarm schedule with per-day enable/disable
- Configurable pump run duration
- Configurable light lead time (VoiceMonkey fires N minutes before alarm)
- Sleep cycle calculator (suggests optimal wake and bedtimes)
- PIN-protected web interface served directly from the Arduino
- Static LAN IP for reliable local access
- Automatic DuckDNS updates every 5 minutes
- All settings persisted to EEPROM (survive power cycles)

## Hardware

| Component | Details |
|-----------|---------|
| Microcontroller | Arduino UNO R4 WiFi |
| Output pin | Pin 7 (pump / relay control) |
| Power | USB or DC barrel jack |

## Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- Python 3.x (bundled with PlatformIO)
- A 2.4 GHz WiFi network
- A [VoiceMonkey](https://voicemonkey.io/) account (for Alexa announcements)
- A [DuckDNS](https://www.duckdns.org/) account (for remote access)

## Setup

### 1. Clone the repository

```bash
git clone https://github.com/YOUR_USERNAME/clockhub-arduino.git
cd clockhub-arduino
```

### 2. Create your credentials file

```bash
cp .env.example .env
```

Open `.env` in any text editor and fill in your real values:

```bash
WIFI_SSID=YourNetworkName
WIFI_PASS=YourPassword
API_TOKEN=your_voicemonkey_api_token
MONKEY_ID=your_voicemonkey_device_id
DUCK_TOKEN=your-duckdns-token
DUCK_DOMAIN=your-subdomain
ACCESS_PIN=123456
ARDUINO_IP=192.168.1.100
GATEWAY_IP=192.168.1.1
SUBNET_MASK=255.255.255.0
DNS_IP=8.8.8.8
```

The `.env` file is gitignored and will never be committed.

### 3. Build and upload

```bash
pio run --target upload
```

On every build, `extra_scripts/load_env.py` reads `.env` and auto-generates `src/config.h`. You never need to edit `config.h` manually.

### 4. Access the web interface

Open a browser and navigate to the IP address you set in `ARDUINO_IP`. Enter your `ACCESS_PIN` when prompted.

## How Credentials Work

```
.env  ← you edit this
  |
  └─> extra_scripts/load_env.py  (runs at build time)
        |
        └─> src/config.h  (auto-generated, gitignored)
              |
              └─> src/main.cpp, src/network.cpp  (#include "config.h")
```

`.env` is the single source of truth for all credentials. `src/config.h` is regenerated on every `pio run` and is never committed to git.

## Project Structure

```
clockhub-arduino/
├── .env.example              # Credentials template (copy to .env)
├── .gitignore
├── platformio.ini
├── README.md
├── extra_scripts/
│   └── load_env.py           # Reads .env, generates src/config.h
└── src/
    ├── main.cpp              # setup(), loop(), global definitions (~55 lines)
    ├── config.h              # Auto-generated (gitignored)
    ├── config.h.example      # config.h template for reference
    ├── types.h               # DaySetting, Config structs and constants
    ├── globals.h             # extern declarations for all shared globals
    ├── storage.h / .cpp      # EEPROM: loadConfig(), saveConfig()
    ├── time_utils.h / .cpp   # formatTime(), getWakeTime(), getBedTime()
    ├── network.h / .cpp      # updateDuckDNS(), triggerVoiceMonkey(), setupWiFi()
    ├── scheduler.h / .cpp    # isScheduleLocked(), runAlarmLogic()
    └── web_server.h / .cpp   # HTTP server, route dispatch, HTML rendering
```

## Web Interface Endpoints

| Endpoint | Description |
|----------|-------------|
| `/` | PIN page (unauthenticated) or main dashboard (with valid `pin=` param) |
| `/SET_SCHEDULE` | Save weekly alarm schedule |
| `/SET_GLOBAL` | Update pump duration and system enable |
| `/SET_LIGHT_CONFIG` | Update light lead time |
| `/SET_SLEEP_DELAY` | Update falling-asleep delay |
| `/CALC_BED` | Calculate optimal bedtimes |
| `/TOGGLE` | Manual pump on/off override |
| `/CHECK_LOCK` | Lock state query (`0` or `1`, used by dashboard JS polling) |

### PIN authentication

The PIN page uses a custom JavaScript masking layer (`type="text"` + `inputmode="numeric"`) so characters are never visually displayed — only bullet points appear. The form submits automatically 1.5 seconds after the last keypress, without revealing the PIN length. After 5 failed attempts the interface locks for 5 minutes.

## Contributing

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/my-feature`
3. Commit your changes — make sure `.env` and `src/config.h` are **not** included
4. Open a pull request

## License

MIT License
