#pragma once
#include <stdint.h>

const int PUMP_PIN = 7;
const int MOTOR_ENA = 9;
const int MOTOR_IN1 = 8;
const int MOTOR_IN2 = 6;
const unsigned long DUCKDNS_INTERVAL = 300000;
const unsigned long OTA_CHECK_INTERVAL = 6UL * 60UL * 60UL * 1000UL; // 6h

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
