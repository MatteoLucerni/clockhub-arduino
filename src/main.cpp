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
#include "ota_manager.h"

const char ssid[]       = WIFI_SSID;
const char pass[]       = WIFI_PASS;
const char* api_token   = API_TOKEN;
const char* monkey_id   = MONKEY_ID;
const char* speaker_id  = SPEAKER_ID;
const char* duck_token  = DUCK_TOKEN;
const char* duck_domain = DUCK_DOMAIN;
const char* access_pin  = ACCESS_PIN;

WiFiServer   server(80);
WiFiUDP      ntpUDP;
NTPClient    timeClient(ntpUDP, "pool.ntp.org", 0);

Config sysConfig;
bool   alarmTriggered = false;
bool   lightTriggered = false;
bool   manualOverride = false;
String scheduleErrorMsg = "";
String pendingAnnounceMsg = "";
bool   blindTriggered = false;
bool   blindManualActive = false;
int    blindManualDirection = 0;
unsigned long blindRunStartMs = 0;
unsigned long blindRunTotalMs = 0;
unsigned long blindRunFullMs  = 0;
int    blindRunStartPos = 0;
int    blindPositionPct = -1;

OneShotAlarm oneShot;

int  targetWakeH = 8;
int  targetWakeM = 30;
bool showBedTimes = false;

unsigned long lastDuckDNSUpdate = 0;
int           currentUTCOffset  = 3600;
unsigned long lastWebActivityMs = 0;

OtaState      otaState = OTA_IDLE;
bool          otaUpdateAvailable = false;
String        otaLatestVersion = "";
String        otaErrorMsg = "";
unsigned long lastOtaCheck = 0;
String        otaCheckNote = "";
unsigned long otaLastCheckEpoch = 0;

const char* daysOfWeek[] = {
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

void setup() {
  Serial.begin(115200);
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  pinMode(MOTOR_ENA, OUTPUT);
  pinMode(MOTOR_IN1, OUTPUT);
  pinMode(MOTOR_IN2, OUTPUT);
  digitalWrite(MOTOR_ENA, LOW);
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, LOW);
  loadConfig();
  loadBlindPosition();
  loadOneShot();
  setupWiFi();
  updateDuckDNS();
  lastDuckDNSUpdate = millis();
}

void loop() {
  timeClient.update();
  unsigned long utcEpoch = timeClient.getEpochTime() - (unsigned long)currentUTCOffset;
  currentUTCOffset = getItalyUTCOffset(utcEpoch);
  timeClient.setTimeOffset(currentUTCOffset);
  updateDuckDNSIfNeeded();
  checkForUpdateIfNeeded();
  handleWebRequest();
  if (pendingAnnounceMsg.length() > 0) {
    announceVoiceMonkey(pendingAnnounceMsg.c_str());
    pendingAnnounceMsg = "";
  }
  runAlarmLogic();
  runOneShotLogic();
}
