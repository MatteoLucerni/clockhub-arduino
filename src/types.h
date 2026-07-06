#pragma once
#include <stdint.h>

const int PUMP_PIN = 7;
const int MOTOR_ENA = 9;
const int MOTOR_IN1 = 8;
const int MOTOR_IN2 = 6;
const unsigned long DUCKDNS_INTERVAL = 300000;
const unsigned long OTA_CHECK_INTERVAL = 30UL * 60UL * 1000UL; // 30min
const unsigned long OTA_DASHBOARD_RECHECK_MS = 60000UL;
const unsigned long HTTP_REQUEST_TIMEOUT_MS = 2000UL;
const unsigned long AUTH_COOKIE_MAX_AGE_SEC = 30UL * 24UL * 3600UL;

struct DaySetting {
  bool active;
  int hour;
  int minute;
};

struct Config {
  bool globalEnabled;
  bool pumpEnabled;
  int runDuration;
  bool lightEnabled;
  int lightLeadMinutes;
  int fallingAsleepMinutes;
  DaySetting schedule[7];
  bool blindEnabled;
  int blindLeadMinutes;
  int blindOpenDuration;
  int blindCloseDuration;
  uint8_t motorSlowdown[6];
  int checkKey;
};

const int ONESHOT_CHECK_KEY = 24683;

struct OneShotAlarm {
  bool armed;
  unsigned long triggerEpoch;   // same time reference as timeClient.getEpochTime()
  bool pumpEnabled;
  bool lightEnabled;
  bool blindEnabled;
  int lightLeadMinutes;
  int blindLeadMinutes;
  // Persisted (not just RAM) so a reboot mid-cycle doesn't replay an action
  // that already fired before the reboot.
  bool pumpDone;
  bool lightDone;
  bool blindDone;
  int checkKey;
};
