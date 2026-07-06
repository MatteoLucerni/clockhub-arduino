#pragma once

// Checks the published version marker and updates otaUpdateAvailable/otaLatestVersion.
// Rate-limited to OTA_CHECK_INTERVAL; call from loop().
void checkForUpdateIfNeeded();

void checkForUpdateNow(int attempts);
bool otaCheckBusy();

// Downloads, verifies and applies the latest .ota firmware, then reboots the device.
// On success this does not return (device resets). On failure, sets otaState/otaErrorMsg
// and returns false so the caller can render the dashboard with the error.
bool startOtaUpdate();
