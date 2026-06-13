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

  Serial.println("[OTA] begin()..."); Serial.flush();
  int ret = ota.begin();
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
  // .ota has been fetched (modem-side EXTENDED_MODEM_TIMEOUT = 60s). On connectivity
  // firmware 0.6.0 the non-blocking startDownload()/downloadProgress() path starves
  // the modem's AT-command handler during the background download, so progress polls
  // time out (Error::Modem == -26) and wedge the modem. The blocking call avoids that
  // and, if it fails, returns a clean error code instead of leaving the modem stuck.
  Serial.println("[OTA] download()..."); Serial.flush();
  int size = ota.download(OTA_FIRMWARE_URL);
  Serial.println("[OTA] download() -> " + String(size)); Serial.flush();
  if (size <= 0) {
    otaErrorMsg = "OTA download failed (" + String(size) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  Serial.println("[OTA] verify()..."); Serial.flush();
  ret = ota.verify();
  Serial.println("[OTA] verify() -> " + String(ret)); Serial.flush();
  if (ret != OTAUpdate::OTA_ERROR_NONE) {
    otaErrorMsg = "OTA verify failed (" + String(ret) + ")";
    otaState = OTA_ERROR;
    return false;
  }

  Serial.println("[OTA] update()..."); Serial.flush();
  ret = ota.update();
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
