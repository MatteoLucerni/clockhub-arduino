#pragma once

const int PUMP_PIN = 7;
const unsigned long DUCKDNS_INTERVAL = 300000;

struct DaySetting {
  bool active;
  int hour;
  int minute;
};

struct Config {
  bool globalEnabled;
  int runDuration;
  int lightLeadMinutes;
  int fallingAsleepMinutes;
  DaySetting schedule[7];
  int checkKey;
};
