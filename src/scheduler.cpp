#include "scheduler.h"
#include "globals.h"
#include "network.h"
#include <Arduino.h>

// direction: 1 = open (backward), -1 = close (forward), 0 = stop
static void setMotor(int direction) {
  if (direction == 1) {
    digitalWrite(MOTOR_IN1, LOW);
    digitalWrite(MOTOR_IN2, HIGH);
    digitalWrite(MOTOR_ENA, HIGH);
  } else if (direction == -1) {
    digitalWrite(MOTOR_IN1, HIGH);
    digitalWrite(MOTOR_IN2, LOW);
    digitalWrite(MOTOR_ENA, HIGH);
  } else {
    digitalWrite(MOTOR_ENA, LOW);
    digitalWrite(MOTOR_IN1, LOW);
    digitalWrite(MOTOR_IN2, LOW);
  }
}

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
  // Manual blind override takes priority
  if (blindManualActive) {
    setMotor(blindManualDirection);
  } else {
    setMotor(0);
  }

  if (manualOverride) {
    digitalWrite(PUMP_PIN, HIGH);
  } else {
    int curDay   = timeClient.getDay();
    int curTotal = timeClient.getHours() * 60 + timeClient.getMinutes();
    int almTotal = sysConfig.schedule[curDay].hour * 60 + sysConfig.schedule[curDay].minute;

    // Light lead time
    int lgtTotal = almTotal - sysConfig.lightLeadMinutes;
    int lgtDay;
    if (lgtTotal < 0) {
      lgtDay = (curDay == 0) ? 6 : curDay - 1;
      lgtTotal += 1440;
    } else {
      lgtDay = curDay;
    }

    // Blind lead time
    int bldTotal = almTotal - sysConfig.blindLeadMinutes;
    int bldDay;
    if (bldTotal < 0) {
      bldDay = (curDay == 0) ? 6 : curDay - 1;
      bldTotal += 1440;
    } else {
      bldDay = curDay;
    }

    if (sysConfig.globalEnabled) {
      // Trigger light (VoiceMonkey)
      if (sysConfig.schedule[lgtDay].active && curTotal == lgtTotal && !lightTriggered) {
        triggerVoiceMonkey();
        lightTriggered = true;
      }

      // Trigger blind opening
      if (!blindManualActive && sysConfig.schedule[bldDay].active && curTotal == bldTotal && !blindTriggered) {
        setMotor(1);
        delay(sysConfig.blindOpenDuration * 1000);
        setMotor(0);
        blindTriggered = true;
      }

      // Trigger pump (water alarm)
      if (sysConfig.schedule[curDay].active && curTotal == almTotal) {
        if (!alarmTriggered) {
          digitalWrite(PUMP_PIN, HIGH);
          delay(sysConfig.runDuration * 1000);
          digitalWrite(PUMP_PIN, LOW);
          alarmTriggered = true;
        }
      } else {
        alarmTriggered = false;
        if (curTotal != lgtTotal) lightTriggered = false;
        if (curTotal != bldTotal) blindTriggered = false;
      }
    } else {
      digitalWrite(PUMP_PIN, LOW);
      alarmTriggered = false;
      lightTriggered = false;
      blindTriggered = false;
    }
  }
}
