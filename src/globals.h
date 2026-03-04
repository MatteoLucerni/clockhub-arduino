#pragma once

#include <WiFiS3.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "types.h"

// Credential constants
extern const char ssid[];
extern const char pass[];
extern const char* api_token;
extern const char* monkey_id;
extern const char* duck_token;
extern const char* duck_domain;
extern const char* access_pin;

// Network objects
extern WiFiServer server;
extern WiFiUDP ntpUDP;
extern NTPClient timeClient;

// Application state
extern Config sysConfig;
extern bool alarmTriggered;
extern bool lightTriggered;
extern bool manualOverride;
extern String scheduleErrorMsg;

// Sleep calculator state
extern int targetWakeH;
extern int targetWakeM;
extern bool showBedTimes;

// Timing
extern unsigned long lastDuckDNSUpdate;

// Day names
extern const char* daysOfWeek[];
