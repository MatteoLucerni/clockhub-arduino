#include "ota_manager.h"
#include "globals.h"
#include "config.h"
#include "root_ca.h"
#include <WiFiS3.h>
#include <OTAUpdate.h>

static const char* OTA_HOST = "raw.githubusercontent.com";
static const char* OTA_VERSION_PATH = "/MatteoLucerni/clockhub-arduino/ota-releases/version.txt";
static const char* OTA_FIRMWARE_URL = "https://raw.githubusercontent.com/MatteoLucerni/clockhub-arduino/ota-releases/firmware.ota";

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

  int ret = ota.begin();
  if (ret != OTAUpdate::OTA_ERROR_NONE) {
    otaErrorMsg = "OTA begin failed (" + String(ret) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  ret = ota.setCACert(root_ca);
  if (ret != OTAUpdate::OTA_ERROR_NONE) {
    otaErrorMsg = "OTA CA cert failed (" + String(ret) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  int size = ota.download(OTA_FIRMWARE_URL);
  if (size <= 0) {
    otaErrorMsg = "OTA download failed (" + String(size) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  ret = ota.verify();
  if (ret != OTAUpdate::OTA_ERROR_NONE) {
    otaErrorMsg = "OTA verify failed (" + String(ret) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  ret = ota.update();
  if (ret != OTAUpdate::OTA_ERROR_NONE) {
    otaErrorMsg = "OTA update failed (" + String(ret) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  ota.reset(); // Reboots the device into the new firmware; does not return.
  return true;
}
