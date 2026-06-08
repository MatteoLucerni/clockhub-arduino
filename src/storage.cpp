#include "storage.h"
#include "globals.h"
#include <EEPROM.h>

struct BlindPosStore { int checkKey; int positionPct; };
static const int BLIND_POS_ADDR     = 256;
static const int BLIND_POS_CHECK_KEY = 54321;

void loadConfig() {
  EEPROM.get(0, sysConfig);
  if (sysConfig.checkKey != 12352) {
    sysConfig.globalEnabled = true;
    sysConfig.runDuration = 10;
    sysConfig.lightEnabled = true;
    sysConfig.lightLeadMinutes = 30;
    sysConfig.fallingAsleepMinutes = 15;
    sysConfig.blindEnabled = true;
    sysConfig.blindLeadMinutes = 30;
    sysConfig.blindOpenDuration = 150;
    sysConfig.blindCloseDuration = 150;
    // Last 30% slowdown: power at 70%, 75%, 80%, 85%, 90%, 95% of run
    uint8_t defaultSlowdown[6] = {80, 75, 70, 60, 40, 25};
    for (int i = 0; i < 6; i++) sysConfig.motorSlowdown[i] = defaultSlowdown[i];
    sysConfig.checkKey = 12352;
    // Sun/Sat 10:00, Mon–Fri 08:30, all active
    for (int i = 0; i < 7; i++) {
      bool isWeekend = (i == 0 || i == 6);
      sysConfig.schedule[i] = {true, isWeekend ? 10 : 8, isWeekend ? 0 : 30};
    }
    EEPROM.put(0, sysConfig);
  }
}

void saveConfig() {
  EEPROM.put(0, sysConfig);
}

void loadBlindPosition() {
  BlindPosStore store;
  EEPROM.get(BLIND_POS_ADDR, store);
  blindPositionPct = (store.checkKey == BLIND_POS_CHECK_KEY) ? store.positionPct : -1;
}

void saveBlindPosition() {
  BlindPosStore store = { BLIND_POS_CHECK_KEY, blindPositionPct };
  EEPROM.put(BLIND_POS_ADDR, store);
}
