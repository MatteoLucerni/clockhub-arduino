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
extern const char* speaker_id;
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
extern String pendingAnnounceMsg;
extern bool blindTriggered;
extern bool blindManualActive;
extern int  blindManualDirection;
extern unsigned long blindRunStartMs;
extern unsigned long blindRunTotalMs;
extern unsigned long blindRunFullMs;
extern int  blindRunStartPos;
extern int  blindPositionPct;

// Sleep calculator state
extern int targetWakeH;
extern int targetWakeM;
extern bool showBedTimes;

// Timing
extern unsigned long lastDuckDNSUpdate;
extern int currentUTCOffset;

// Day names
extern const char* daysOfWeek[];
