# ClockHub — Arduino UNO R4 WiFi

Smart wake-up light/alarm system: weekly schedule, pump control, motorized blind,
Alexa announcements (VoiceMonkey), DuckDNS, web dashboard with PIN login, and
wireless ("OneTap OTA") firmware updates.

## Hardware / Toolchain

- Board: Arduino UNO R4 WiFi (Renesas RA4M1 + ESP32-S3 WiFi co-processor)
- PlatformIO env: `uno_r4_wifi` (platform `renesas-ra`, framework `arduino`)
- PlatformIO executable: `C:\Users\Matteo\.platformio\penv\Scripts\pio.exe`
- WiFi co-processor (ESP32-S3) connectivity firmware **must stay at 0.5.2** to
  match the PlatformIO toolchain. `framework-arduinorenesas-uno` **1.5.1** is the
  latest published (renesas-ra platform 1.8.0), and its OTAUpdate/Modem libraries
  target firmware **0.5.2** (`WIFI_FIRMWARE_LATEST_VERSION` in `WiFiS3/WiFi.h`).
  **Do NOT upgrade the board firmware past 0.5.2** (e.g. to 0.6.0 via Arduino IDE):
  `download()`/`verify()` still work, but the firmware-transfer step `update()`
  speaks the old protocol to the newer firmware and dies with an ESP32
  `uart_get_buffered_data_len(...) uart driver error`, never completing the flash.
  If it ever gets bumped, downgrade back to 0.5.2 in Arduino IDE (Tools → Firmware
  Updater) until PlatformIO ships a framework that targets the newer firmware.
- OTA call requirements (mirror the library's `OTA` / `OTANonBlocking` examples):
  `begin()`, `download()` and `update()` must all receive the **same destination
  path** (`"/update.bin"`) — where the co-processor writes the downloaded `.ota`.
  The no-arg variants give the download no storage target: it fails with
  `Error::Modem` (-26) and wedges the modem. The examples also run the OTA in a
  **clean network state** (right after WiFi connect, nothing else open); our
  `/OTA_APPLY` handler replicates that — it sends its HTTP response, **closes the
  browser socket** (which also avoids a retry/reboot loop), then **`server.end()` +
  `ntpUDP.stop()`** before calling the OTA, restoring them (`server.begin()` /
  `timeClient.begin()`) only on failure. With the :80 listener or NTP UDP socket
  still open, the apply step (which resets the modem) hangs. NB: the
  `uart_get_buffered_data_len(...) uart driver error` the ESP32 logs during the
  apply is **benign noise** — it shows up in the stock OTA example too, so don't
  chase it. We use the blocking sequence
  `begin(path)`→`download(url,path)`→`verify()`→`update(path)`→`reset()`.

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
src/storage.h/.cpp  — loadConfig(), saveConfig(), loadOneShot(), saveOneShot() (EEPROM)
src/time_utils.h/.cpp — formatTime(), getWakeTime(), getBedTime()
src/network.h/.cpp  — DuckDNS, VoiceMonkey/Alexa announcements, WiFi setup
src/scheduler.h/.cpp — isScheduleLocked(), isBlindClosingLocked(), runAlarmLogic(),
                       runOneShotLogic() (one-shot alarm: see "One-shot alarm" below)
src/web_server.h/.cpp — handleWebRequest() + dashboard HTML renderers
src/ota_manager.h/.cpp — OTA version check + apply (see "OTA" below)
src/root_ca.h        — CA cert for raw.githubusercontent.com (OTA downloads)
src/config.h         — AUTO-GENERATED, gitignored (see "Credentials" below)
```

## Credentials — handle with care

- `.env` (gitignored) holds real secrets: WiFi, VoiceMonkey API token, DuckDNS
  token, ACCESS_PIN, static IP config.
- `extra_scripts/load_env.py` runs pre-build and generates `src/config.h`
  (also gitignored) from `.env`, plus `FIRMWARE_VERSION` (git short SHA, or
  `FW_VERSION` env var set by CI, or `"dev"`).
- **Never print, log, or commit the contents of `.env` or `src/config.h`.**
  When discussing the CI workflow's `DOTENV_CONTENTS` secret, never echo its
  value.

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

## One-shot alarm

- `OneShotAlarm` (`src/types.h`) is a separate struct from `Config`, persisted at
  its own EEPROM address (`ONESHOT_ADDR = 300` in `storage.cpp`, with its own
  `checkKey`) — adding/changing one-shot fields never touches the weekly
  schedule's EEPROM layout or `checkKey`.
- Unlike the weekly schedule (which compares minute-of-day + day-of-week), the
  one-shot stores an **absolute epoch** (`triggerEpoch`, same reference as
  `timeClient.getEpochTime()`) computed once at arm time
  (`now + hours*60+minutes minutes`). This sidesteps all midnight/day-of-week
  edge cases for a "fire once, N minutes from now" event.
- `runOneShotLogic()` (`scheduler.cpp`) runs every `loop()` and auto-disarms
  once the trigger has passed and every enabled action (pump/light/blind) has
  run; it reuses `sysConfig.runDuration` and `sysConfig.blindOpenDuration`
  rather than having its own durations. Its pump branch skips (without
  marking done) while `manualOverride` is active, so it never fights a manual
  pump run for `PUMP_PIN` — it fires as soon as the override is lifted, since
  `nowEpoch` only moves forward.
- `pumpDone`/`lightDone`/`blindDone` live **inside** `OneShotAlarm` (persisted
  by `saveOneShot()` right after each fires), not as bare RAM globals — a
  reboot mid-cycle must not replay an action that already completed.
- Pump activation (`startPumpRun()`/`updatePumpRun()` in `scheduler.cpp`) is a
  non-blocking `millis()`-based timer, shared by `runAlarmLogic()` and
  `runOneShotLogic()` (mirroring the existing blind motor state machine
  pattern) instead of `delay()`. Starting a run while one is already active
  just resets the timer, so a weekly alarm and a one-shot landing on the same
  pump trigger in the same `loop()` tick coalesce into one continuous run
  instead of blocking twice back-to-back.
- `isScheduleLocked()`/`isBlindClosingLocked()` check the one-shot's lock
  window **before** the weekly `if (!sysConfig.globalEnabled) return false;`
  early-return, so the one-shot still locks settings/OTA even when "System
  Enabled" (the weekly toggle) is off.
- The dashboard "One-Shot Alarm" card has its own `<form>`/route
  (`/ARM_ONESHOT`, `/CANCEL_ONESHOT`), separate from `/SAVE_ALL`, since arming
  is a distinct action from saving settings. `/ARM_ONESHOT` is rejected while
  already armed (prevents a stale page/duplicate request from silently
  clobbering a pending one-shot); `/CANCEL_ONESHOT` is a no-op when not armed,
  and is never blocked by the lock it itself creates.

## Conventions

- **Everything user-facing — dashboard text, button labels, confirm()
  dialogs, error messages, code comments — is in English.** The only
  exception is `pendingAnnounceMsg` strings sent to Alexa via VoiceMonkey,
  which should match whatever language the Alexa device is configured for
  (currently Italian was changed to English per user request — confirm before
  reverting).
- Toggle-style boolean settings (System Enabled, Pump/Light/Blind Enabled,
  per-day "active") use the `.switch`/`.slider` CSS toggle, built via the
  `toggleSwitch()` helper — not raw `<input type="checkbox">`.
