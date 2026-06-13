#include "ota_manager.h"
#include "globals.h"
#include "config.h"
#include "root_ca.h"
#include "ota_diag_ca.h"
#include <WiFiS3.h>
#include <OTAUpdate.h>

static const char* OTA_HOST = "raw.githubusercontent.com";
static const char* OTA_VERSION_PATH = "/MatteoLucerni/clockhub-arduino/ota-releases/version.txt";
static const char* OTA_FIRMWARE_URL = "https://raw.githubusercontent.com/MatteoLucerni/clockhub-arduino/ota-releases/firmware.ota";

// TEMP DIAGNOSTIC: known-good 29KB .ota on CloudFront, from Arduino's official example.
static const char* OTA_DIAG_URL = "https://downloads.arduino.cc/ota/UNOR4WIFI_Animation.ota";

// Performs a simple HTTPS GET and returns the response body (Content-Length aware).
static String httpsGet(const char* host, const char* path) {
  WiFiSSLClient client;
  if (!client.connect(host, 443)) return "";

  client.println("GET " + String(path) + " HTTP/1.1");
  client.println("Host: " + String(host));
  client.println("Connection: close");
  client.println();

  unsigned long start = millis();
  while (client.connected() && !client.available()) {
    if (millis() - start > 5000) {
      client.stop();
      return "";
    }
  }

  int contentLength = -1;
  while (client.connected() || client.available()) {
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
  while (client.connected() || client.available()) {
    if (client.available()) {
      body += (char)client.read();
      if (contentLength >= 0 && (int)body.length() >= contentLength) break;
    }
  }

  client.stop();
  body.trim();
  return body;
}

void checkForUpdateIfNeeded() {
  if (lastOtaCheck != 0 && millis() - lastOtaCheck < OTA_CHECK_INTERVAL) return;
  lastOtaCheck = millis();

  String latest = httpsGet(OTA_HOST, OTA_VERSION_PATH);
  if (latest.length() == 0) return;

  otaLatestVersion = latest;
  otaUpdateAvailable = (latest != String(FIRMWARE_VERSION));
}

bool startOtaUpdate() {
  OTAUpdate ota;

  modem.debug(Serial, 2);

  Serial.println("[OTA] begin()..."); Serial.flush();
  int ret = ota.begin();
  Serial.println("[OTA] begin() -> " + String(ret)); Serial.flush();
  if (ret != OTAUpdate::OTA_ERROR_NONE) {
    otaErrorMsg = "OTA begin failed (" + String(ret) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  Serial.println("[OTA] setCACert()..."); Serial.flush();
  ret = ota.setCACert(diag_root_ca); // TEMP DIAGNOSTIC: CA for downloads.arduino.cc, not raw.githubusercontent.com
  Serial.println("[OTA] setCACert() -> " + String(ret)); Serial.flush();
  if (ret != OTAUpdate::OTA_ERROR_NONE) {
    otaErrorMsg = "OTA CA cert failed (" + String(ret) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  // The blocking ota.download() (AT+OTADOWNLOAD) never returns and wedges the
  // modem on connectivity firmware 0.6.0 when fetching from raw.githubusercontent.com,
  // regardless of payload size. startDownload()/downloadProgress() (AT+OTADOWNLOADSTART,
  // requires modem fw >= 0.5.0) works correctly.
  Serial.println("[OTA] startDownload()..."); Serial.flush();
  int size = ota.startDownload(OTA_DIAG_URL); // TEMP DIAGNOSTIC: 29KB file on CloudFront instead of OTA_FIRMWARE_URL
  Serial.println("[OTA] startDownload() -> " + String(size)); Serial.flush();
  if (size <= 0) {
    otaErrorMsg = "OTA download failed (" + String(size) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  modem.noDebug(); // avoid flooding Serial during progress polling
  Serial.println("[OTA] downloadProgress() poll..."); Serial.flush();
  int progress = 0;
  int lastPrinted = -1;
  unsigned long pollStart = millis();
  unsigned long lastHeartbeat = millis();
  while (progress >= 0 && progress < size && millis() - pollStart < 180000) {
    progress = ota.downloadProgress();
    if (progress != lastPrinted || millis() - lastHeartbeat > 5000) {
      Serial.println("[OTA] downloadProgress() -> " + String(progress) + " / " + String(size));
      Serial.flush();
      lastPrinted = progress;
      lastHeartbeat = millis();
    }
    delay(200);
  }
  modem.debug(Serial, 2);
  Serial.println("[OTA] downloadProgress() final -> " + String(progress) + " / " + String(size)); Serial.flush();

  Serial.println("[OTA] verify()..."); Serial.flush();
  ret = ota.verify();
  Serial.println("[OTA] verify() -> " + String(ret)); Serial.flush();

  // TEMP DIAGNOSTIC STOP: this downloaded Arduino's demo .ota, not our firmware.
  // Never call update()/reset() here - it would flash unrelated firmware onto the device.
  Serial.println("[OTA] DIAGNOSTIC STOP - not calling update()/reset()"); Serial.flush();
  otaErrorMsg = "Diagnostic run complete (see Serial)";
  otaState = OTA_ERROR;
  return false;

  Serial.println("[OTA] update()..."); Serial.flush();
  ret = ota.update();
  Serial.println("[OTA] update() -> " + String(ret)); Serial.flush();
  if (ret != OTAUpdate::OTA_ERROR_NONE) {
    otaErrorMsg = "OTA update failed (" + String(ret) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  Serial.println("[OTA] reset()..."); Serial.flush();
  ota.reset(); // Reboots the device into the new firmware; does not return.
  Serial.println("[OTA] reset() returned (unexpected!)"); Serial.flush();
  return true;
}
