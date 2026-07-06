#include "ota_manager.h"
#include "globals.h"
#include "config.h"
#include "root_ca.h"
#include <WiFiS3.h>
#include <OTAUpdate.h>

static const char* OTA_HOST = "raw.githubusercontent.com";
static const char* OTA_VERSION_PATH = "/MatteoLucerni/clockhub-arduino/ota-releases/version.txt";
static const char* OTA_FIRMWARE_URL = "https://raw.githubusercontent.com/MatteoLucerni/clockhub-arduino/ota-releases/firmware.ota";

// Destination path on the WiFi co-processor's storage where the .ota is written
// before it is applied. begin(), download() and update() MUST all reference the
// same path; the no-path variants give the download no storage target and fail
// with Error::Modem (-26), wedging the modem. Matches the official OTA examples.
static const char* OTA_DOWNLOAD_PATH = "/update.bin";

// Performs a simple HTTPS GET for the version marker (Content-Length aware,
// status-checked, deadline-bounded) and validates the body so a CDN error
// page or truncated response can never be mistaken for a version string.
static bool fetchLatestVersion(String& latest) {
  WiFiSSLClient client;
  client.setCACert(root_ca);
  if (!client.connect(OTA_HOST, 443)) return false;

  client.println("GET " + String(OTA_VERSION_PATH) + " HTTP/1.0");
  client.println("Host: " + String(OTA_HOST));
  client.println("Connection: close");
  client.println();

  unsigned long start = millis();
  while (client.connected() && !client.available()) {
    if (millis() - start > 5000) {
      client.stop();
      return false;
    }
    delay(10);
  }

  String statusLine = client.readStringUntil('\n');
  if (statusLine.indexOf(" 200") < 0) {
    client.stop();
    return false;
  }

  int contentLength = -1;
  while (millis() - start < 8000) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) {
      contentLength = line.substring(line.indexOf(':') + 1).toInt();
    }
  }

  String body = "";
  while ((client.connected() || client.available()) && millis() - start < 8000) {
    if (client.available()) {
      body += (char)client.read();
      if (contentLength >= 0 && (int)body.length() >= contentLength) break;
      if (body.length() > 64) break;
    }
  }

  client.stop();
  body.trim();
  if (body.length() == 0 || body.length() > 40) return false;
  for (unsigned int i = 0; i < body.length(); i++) {
    char c = body.charAt(i);
    if (!isAlphaNumeric(c) && c != '.' && c != '-' && c != '_') return false;
  }
  latest = body;
  return true;
}

bool otaCheckBusy() {
  return blindManualActive || manualOverride || digitalRead(PUMP_PIN) == HIGH;
}

void checkForUpdateNow(int attempts) {
  lastOtaCheck = millis();

  String latest = "";
  bool ok = false;
  for (int i = 0; i < attempts && !ok; i++) {
    if (i > 0) delay(500);
    ok = fetchLatestVersion(latest);
  }

  otaLastCheckEpoch = timeClient.getEpochTime();
  if (!ok) {
    otaCheckNote = "Update check failed. Check the connection and retry.";
    return;
  }

  otaCheckNote = "";
  otaLatestVersion = latest;
  otaUpdateAvailable = (latest != String(FIRMWARE_VERSION));
}

void checkForUpdateIfNeeded() {
  if (lastOtaCheck != 0 && millis() - lastOtaCheck < OTA_CHECK_INTERVAL) return;
  if (otaCheckBusy()) return;
  checkForUpdateNow(1);
}

bool startOtaUpdate() {
  OTAUpdate ota;

  Serial.println("[OTA] begin()..."); Serial.flush();
  int ret = ota.begin(OTA_DOWNLOAD_PATH);
  Serial.println("[OTA] begin() -> " + String(ret)); Serial.flush();
  if (ret != OTAUpdate::OTA_ERROR_NONE) {
    otaErrorMsg = "OTA begin failed (" + String(ret) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  Serial.println("[OTA] setCACert()..."); Serial.flush();
  ret = ota.setCACert(root_ca);
  Serial.println("[OTA] setCACert() -> " + String(ret)); Serial.flush();
  if (ret != OTAUpdate::OTA_ERROR_NONE) {
    otaErrorMsg = "OTA CA cert failed (" + String(ret) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  // Blocking download: a single AT+OTADOWNLOAD that returns only once the whole
  // .ota has been written to OTA_DOWNLOAD_PATH on the co-processor's storage
  // (modem-side EXTENDED_MODEM_TIMEOUT = 60s, no polling). Passing the destination
  // path is mandatory — without it the download has no storage target, returns
  // Error::Modem (-26) and wedges the modem. We use the blocking call (vs the
  // non-blocking startDownload/downloadProgress) because the dashboard already warns
  // about a ~1 min freeze; on failure it returns a clean error code.
  int size = 0;
  ret = -1;
  for (int attempt = 1; attempt <= 2; attempt++) {
    if (attempt > 1) delay(2000);
    Serial.println("[OTA] download() attempt " + String(attempt) + "..."); Serial.flush();
    size = ota.download(OTA_FIRMWARE_URL, OTA_DOWNLOAD_PATH);
    Serial.println("[OTA] download() -> " + String(size)); Serial.flush();
    if (size <= 0) continue;
    Serial.println("[OTA] verify()..."); Serial.flush();
    ret = ota.verify();
    Serial.println("[OTA] verify() -> " + String(ret)); Serial.flush();
    if (ret == OTAUpdate::OTA_ERROR_NONE) break;
  }
  if (size <= 0) {
    otaErrorMsg = "OTA download failed (" + String(size) + ")";
    otaState = OTA_ERROR;
    return false;
  }
  if (ret != OTAUpdate::OTA_ERROR_NONE) {
    otaErrorMsg = "OTA verify failed (" + String(ret) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  Serial.println("[OTA] update()..."); Serial.flush();
  ret = ota.update(OTA_DOWNLOAD_PATH);
  Serial.println("[OTA] update() -> " + String(ret)); Serial.flush();
  if (ret != OTAUpdate::OTA_ERROR_NONE) {
    otaErrorMsg = "OTA update failed (" + String(ret) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  Serial.println("[OTA] reset() - rebooting into new firmware..."); Serial.flush();
  ota.reset(); // Reboots the device into the new firmware; does not return.
  Serial.println("[OTA] reset() returned (unexpected!)"); Serial.flush();
  return true;
}
