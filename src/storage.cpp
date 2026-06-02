#include "storage.h"
#include "globals.h"
#include <EEPROM.h>

void loadConfig() {
  EEPROM.get(0, sysConfig);
  if (sysConfig.checkKey != 12351) {
    sysConfig.globalEnabled = true;
    sysConfig.runDuration = 10;
    sysConfig.lightEnabled = true;
    sysConfig.lightLeadMinutes = 5;
    sysConfig.fallingAsleepMinutes = 15;
    sysConfig.blindEnabled = true;
    sysConfig.blindLeadMinutes = 5;
    sysConfig.blindOpenDuration = 145;
    sysConfig.blindCloseDuration = 145;
    // Last 20% slowdown: power at 80%, 85%, 90%, 95% of run
    uint8_t defaultSlowdown[4] = {80, 60, 40, 20};
    for (int i = 0; i < 4; i++) sysConfig.motorSlowdown[i] = defaultSlowdown[i];
    sysConfig.checkKey = 12351;
    // Sun/Sat 11:00, Mon–Fri 08:30, all active
    for (int i = 0; i < 7; i++) {
      bool isWeekend = (i == 0 || i == 6);
      sysConfig.schedule[i] = {true, isWeekend ? 11 : 8, isWeekend ? 0 : 30};
    }
    EEPROM.put(0, sysConfig);
  }
}

void saveConfig() {
  EEPROM.put(0, sysConfig);
}
