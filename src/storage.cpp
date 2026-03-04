#include "storage.h"
#include "globals.h"
#include <EEPROM.h>

void loadConfig() {
  EEPROM.get(0, sysConfig);
  if (sysConfig.checkKey != 12345) {
    sysConfig.globalEnabled = true;
    sysConfig.runDuration = 10;
    sysConfig.lightLeadMinutes = 30;
    sysConfig.fallingAsleepMinutes = 15;
    sysConfig.checkKey = 12345;
    for (int i = 0; i < 7; i++) {
      sysConfig.schedule[i] = {false, 7, 30};
    }
    EEPROM.put(0, sysConfig);
  }
}

void saveConfig() {
  EEPROM.put(0, sysConfig);
}
