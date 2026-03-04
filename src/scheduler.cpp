#include "scheduler.h"
#include "globals.h"
#include "network.h"
#include <Arduino.h>

bool isScheduleLocked() {
  if (!sysConfig.globalEnabled) return false;

  int curDay = timeClient.getDay();
  int curTotal = timeClient.getHours() * 60 + timeClient.getMinutes();

  for (int checkDay = 0; checkDay < 7; checkDay++) {
    if (!sysConfig.schedule[checkDay].active) continue;

    int almTotal = sysConfig.schedule[checkDay].hour * 60 + sysConfig.schedule[checkDay].minute;
    int minUntilAlarm;

    if (checkDay == curDay) {
      minUntilAlarm = almTotal - curTotal;
      if (minUntilAlarm < 0) minUntilAlarm += 1440;
    } else if (checkDay == ((curDay + 1) % 7)) {
      minUntilAlarm = (1440 - curTotal) + almTotal;
    } else {
      continue;
    }

    if (minUntilAlarm <= 60) {
      scheduleErrorMsg = "Some settings cannot be modified because an alarm is within 1 hour";
      return true;
    }
  }

  return false;
}

void runAlarmLogic() {
  if (manualOverride) {
    digitalWrite(PUMP_PIN, HIGH);
  } else {
    int curDay   = timeClient.getDay();
    int curTotal = timeClient.getHours() * 60 + timeClient.getMinutes();
    int almTotal = sysConfig.schedule[curDay].hour * 60 + sysConfig.schedule[curDay].minute;
    int lgtTotal = almTotal - sysConfig.lightLeadMinutes;

    int lgtDay;
    if (lgtTotal < 0) {
      lgtDay = (curDay == 0) ? 6 : curDay - 1;
    } else {
      lgtDay = curDay;
    }
    if (lgtTotal < 0) {
      lgtTotal += 1440;
    }

    if (sysConfig.globalEnabled) {
      if (sysConfig.schedule[lgtDay].active && curTotal == lgtTotal && !lightTriggered) {
        triggerVoiceMonkey();
        lightTriggered = true;
      }

      if (sysConfig.schedule[curDay].active && curTotal == almTotal) {
        if (!alarmTriggered) {
          digitalWrite(PUMP_PIN, HIGH);
          delay(sysConfig.runDuration * 1000);
          digitalWrite(PUMP_PIN, LOW);
          alarmTriggered = true;
        }
      } else {
        alarmTriggered = false;
        if (curTotal != lgtTotal) {
          lightTriggered = false;
        }
      }
    } else {
      digitalWrite(PUMP_PIN, LOW);
      alarmTriggered = false;
      lightTriggered = false;
    }
  }
}
