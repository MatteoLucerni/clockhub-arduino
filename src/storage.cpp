#include "storage.h"
#include "globals.h"
#include <EEPROM.h>

void loadConfig() {
  EEPROM.get(0, sysConfig);
  if (sysConfig.checkKey != 12347) {
    sysConfig.globalEnabled = true;
    sysConfig.runDuration = 10;
    sysConfig.lightEnabled = true;
    sysConfig.lightLeadMinutes = 30;
    sysConfig.fallingAsleepMinutes = 15;
    sysConfig.blindEnabled = true;
    sysConfig.blindLeadMinutes = 5;
    sysConfig.blindOpenDuration = 55;
    sysConfig.blindCloseDuration = 55;
    sysConfig.checkKey = 12347;
    for (int i = 0; i < 7; i++) {
      sysConfig.schedule[i] = {false, 7, 30};
    }
    EEPROM.put(0, sysConfig);
  }
}

void saveConfig() {
  EEPROM.put(0, sysConfig);
}
