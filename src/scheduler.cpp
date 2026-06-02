#include "scheduler.h"
#include "globals.h"
#include "network.h"
#include <Arduino.h>

// direction: 1 = open (backward), -1 = close (forward), 0 = stop
// power: 0-255 PWM value
static void setMotor(int direction, uint8_t power = 255) {
  if (direction == 1) {
    digitalWrite(MOTOR_IN1, LOW);
    digitalWrite(MOTOR_IN2, HIGH);
    analogWrite(MOTOR_ENA, power);
  } else if (direction == -1) {
    digitalWrite(MOTOR_IN1, HIGH);
    digitalWrite(MOTOR_IN2, LOW);
    analogWrite(MOTOR_ENA, power);
  } else {
    analogWrite(MOTOR_ENA, 0);
    digitalWrite(MOTOR_IN1, LOW);
    digitalWrite(MOTOR_IN2, LOW);
  }
}

// Returns PWM (0-255) for the current moment in a timed run.
// First 80%: full power. Last 20%: 4 slowdown sub-segments of 5% each.
static uint8_t calcMotorPWM(unsigned long elapsed, unsigned long total) {
  if (total == 0 || elapsed >= total) return 0;
  unsigned long pct = elapsed * 100UL / total; // 0..99
  if (pct < 80) return 255;
  uint8_t seg = (uint8_t)((pct - 80) / 5); // 0..3
  if (seg > 3) seg = 3;
  return (uint8_t)((uint32_t)sysConfig.motorSlowdown[seg] * 255 / 100);
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
  // Non-blocking blind motor state machine
  if (blindManualActive) {
    unsigned long elapsed = millis() - blindRunStartMs;
    if (elapsed >= blindRunTotalMs) {
      setMotor(0);
      blindManualActive    = false;
      blindManualDirection = 0;
    } else {
      setMotor(blindManualDirection, calcMotorPWM(elapsed, blindRunTotalMs));
    }
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
      if (sysConfig.lightEnabled && sysConfig.schedule[lgtDay].active && curTotal == lgtTotal && !lightTriggered) {
        triggerVoiceMonkey();
        lightTriggered = true;
      }

      // Trigger blind opening (non-blocking: start the state machine)
      if (sysConfig.blindEnabled && !blindManualActive && sysConfig.schedule[bldDay].active && curTotal == bldTotal && !blindTriggered) {
        blindManualActive    = true;
        blindManualDirection = 1;
        blindRunStartMs      = millis();
        blindRunTotalMs      = (unsigned long)sysConfig.blindOpenDuration * 1000UL;
        blindTriggered       = true;
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
