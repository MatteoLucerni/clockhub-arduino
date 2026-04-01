#include <WiFiS3.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include "config.h"
#include "types.h"
#include "globals.h"
#include "storage.h"
#include "network.h"
#include "scheduler.h"
#include "web_server.h"
#include "time_utils.h"

// ─── Global definitions (declared extern in globals.h) ────────────────────────

const char ssid[]       = WIFI_SSID;
const char pass[]       = WIFI_PASS;
const char* api_token   = API_TOKEN;
const char* monkey_id   = MONKEY_ID;
const char* duck_token  = DUCK_TOKEN;
const char* duck_domain = DUCK_DOMAIN;
const char* access_pin  = ACCESS_PIN;

WiFiServer   server(80);
WiFiUDP      ntpUDP;
NTPClient    timeClient(ntpUDP, "pool.ntp.org", 0); // offset set dynamically

Config sysConfig;
bool   alarmTriggered = false;
bool   lightTriggered = false;
bool   manualOverride = false;
String scheduleErrorMsg = "";

int  targetWakeH = 8;
int  targetWakeM = 30;
bool showBedTimes = false;

unsigned long lastDuckDNSUpdate = 0;
int           currentUTCOffset  = 3600; // CET default, updated automatically

const char* daysOfWeek[] = {
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

// ─── Arduino entry points ─────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  loadConfig();
  setupWiFi();
  updateDuckDNS();
  lastDuckDNSUpdate = millis();
}

void loop() {
  timeClient.update();
  // Derive UTC from current epoch (subtract previously applied offset), then recompute DST
  unsigned long utcEpoch = timeClient.getEpochTime() - (unsigned long)currentUTCOffset;
  currentUTCOffset = getItalyUTCOffset(utcEpoch);
  timeClient.setTimeOffset(currentUTCOffset);
  updateDuckDNSIfNeeded();
  handleWebRequest();
  runAlarmLogic();
}
