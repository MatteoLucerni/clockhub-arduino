# ClockHub — Arduino UNO R4 WiFi

Smart wake-up light/alarm system: weekly schedule, pump control, motorized blind,
Alexa announcements (VoiceMonkey), DuckDNS, web dashboard with PIN login, and
wireless ("OneTap OTA") firmware updates.

## Hardware / Toolchain

- Board: Arduino UNO R4 WiFi (Renesas RA4M1 + ESP32-S3 WiFi co-processor)
- PlatformIO env: `uno_r4_wifi` (platform `renesas-ra`, framework `arduino`)
- PlatformIO executable: `C:\Users\Matteo\.platformio\penv\Scripts\pio.exe`
- WiFi co-processor firmware on this device is **0.4.1** — below the 0.5.0
  threshold required for `OTAUpdate::startDownload()`/`downloadProgress()`.
  Any OTA-related code must use the **blocking** `download()`/`verify()`/
  `update()`/`reset()` sequence.

### Common commands

```sh
pio run -e uno_r4_wifi              # build
pio run -e uno_r4_wifi -t upload    # build + flash via USB (close any open
                                     # Serial Monitor first — Windows locks COM3)
```

## Project structure

```
src/main.cpp        — setup() + loop() + global definitions
src/types.h         — DaySetting, Config structs, pin numbers, timing constants
src/globals.h       — extern declarations for all shared globals
src/storage.h/.cpp  — loadConfig(), saveConfig() (EEPROM)
src/time_utils.h/.cpp — formatTime(), getWakeTime(), getBedTime()
src/network.h/.cpp  — DuckDNS, VoiceMonkey/Alexa announcements, WiFi setup
src/scheduler.h/.cpp — isScheduleLocked(), isBlindClosingLocked(), runAlarmLogic()
src/web_server.h/.cpp — handleWebRequest() + dashboard HTML renderers
src/ota_manager.h/.cpp — OTA version check + apply (see "OTA" below)
src/root_ca.h        — CA cert for raw.githubusercontent.com (OTA downloads)
src/config.h         — AUTO-GENERATED, gitignored (see "Credentials" below)
```

`tools/mock_server.py` is a Python port of the dashboard for testing the UI
without hardware — keep it in sync whenever `web_server.cpp` HTML/CSS/routes
change (same CSS classes, same route names, same confirm() text, etc.).

## Credentials — handle with care

- `.env` (gitignored) holds real secrets: WiFi, VoiceMonkey API token, DuckDNS
  token, ACCESS_PIN, static IP config.
- `extra_scripts/load_env.py` runs pre-build and generates `src/config.h`
  (also gitignored) from `.env`, plus `FIRMWARE_VERSION` (git short SHA, or
  `FW_VERSION` env var set by CI, or `"dev"`).
- **Never print, log, or commit the contents of `.env` or `src/config.h`.**
  When discussing the CI workflow's `DOTENV_CONTENTS` secret, never echo its
  value.

## Local UI testing (no hardware needed)

```sh
python tools/mock_server.py [port]   # default port 8080, binds 0.0.0.0
```

Reads `ACCESS_PIN` from `.env` (falls back to `1234`). Open
`http://127.0.0.1:<port>/?pin=<ACCESS_PIN>`. Includes a "Dev Tools" card to
time-travel the simulated clock (test alarm-window locking) and to simulate
OTA update-available / apply-failure scenarios.

After UI changes, verify by running the mock server and curling/driving the
relevant routes — don't just eyeball the HTML string in the source.

## OTA update system ("OneTap")

- On push to `main`, `.github/workflows/ota-build.yml` builds the firmware,
  converts `firmware.bin` → `firmware.ota` (board `UNOR4WIFI`, via vendored
  `tools/ota_tools/{lzss.py,lzss.so,bin2ota.py}`), and force-pushes
  `firmware.ota` + `version.txt` to the orphan branch `ota-releases`.
- The device polls `ota-releases/version.txt` every `OTA_CHECK_INTERVAL`
  (`src/types.h`, currently 30 min) and compares it to the compiled-in
  `FIRMWARE_VERSION`.
- Dashboard "Firmware" card shows the update badge; applying it
  (`/OTA_APPLY`) is a single explicit tap + JS `confirm()`, then **blocks**
  `loop()` for the download/verify/update before the device reboots itself.
  It's disabled while `isScheduleLocked()` or `blindManualActive`.
- If `setCACert`/TLS starts failing, regenerate `src/root_ca.h` per the
  comment at the top of that file.

## Conventions

- **Everything user-facing — dashboard text, button labels, confirm()
  dialogs, error messages, code comments — is in English.** The only
  exception is `pendingAnnounceMsg` strings sent to Alexa via VoiceMonkey,
  which should match whatever language the Alexa device is configured for
  (currently Italian was changed to English per user request — confirm before
  reverting).
- Keep `src/web_server.cpp` and `tools/mock_server.py` in sync: same CSS
  (including the `.switch`/`.slider` toggle styles), same route names, same
  card layout/order.
- Toggle-style boolean settings (System Enabled, Pump/Light/Blind Enabled,
  per-day "active") use the `.switch`/`.slider` CSS toggle, built via the
  `toggleSwitch()` (C++) / `toggle_switch()` (Python) helpers — not raw
  `<input type="checkbox">`.
