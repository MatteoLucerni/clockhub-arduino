#include "scheduler.h"
#include "globals.h"
#include "network.h"
#include "storage.h"
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
// Slowdown threshold is always the last 30% of fullMs (the complete open/close duration),
// regardless of how short the actual run is (e.g. partial travel from mid-position).
static uint8_t calcMotorPWM(unsigned long elapsed, unsigned long runMs, unsigned long fullMs) {
  if (runMs == 0 || elapsed >= runMs) return 0;
  unsigned long timeFromEnd = runMs - elapsed;
  unsigned long slowdownMs  = fullMs * 3 / 10;  // 30% of full duration
  unsigned long segMs       = fullMs / 20;       // 5% of full duration per segment
  if (segMs == 0 || timeFromEnd > slowdownMs) return 255;
  uint8_t seg;
  if      (timeFromEnd < segMs)         seg = 5;  // 95-100%
  else if (timeFromEnd < segMs * 2UL)   seg = 4;  // 90-95%
  else if (timeFromEnd < segMs * 3UL)   seg = 3;  // 85-90%
  else if (timeFromEnd < segMs * 4UL)   seg = 2;  // 80-85%
  else if (timeFromEnd < segMs * 5UL)   seg = 1;  // 75-80%
  else                                  seg = 0;  // 70-75%
  return (uint8_t)((uint32_t)sysConfig.motorSlowdown[seg] * 255 / 100);
}

// Returns the estimated blind position (0=closed, 100=open, -1=unknown).
// If the motor is currently running, interpolates from the run start position.
int currentBlindPosition() {
  if (!blindManualActive || blindRunTotalMs == 0 || blindRunStartPos < 0)
    return blindPositionPct;
  unsigned long elapsed = millis() - blindRunStartMs;
  if (elapsed >= blindRunTotalMs) return (blindManualDirection == 1) ? 100 : 0;
  int pos;
  if (blindManualDirection == 1)
    pos = blindRunStartPos + (int)((long)(100 - blindRunStartPos) * (long)elapsed / (long)blindRunTotalMs);
  else
    pos = blindRunStartPos - (int)((long)blindRunStartPos       * (long)elapsed / (long)blindRunTotalMs);
  return constrain(pos, 0, 100);
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

bool isBlindClosingLocked() {
  if (!sysConfig.globalEnabled) return false;

  int curDay   = timeClient.getDay();
  int curTotal = timeClient.getHours() * 60 + timeClient.getMinutes();

  for (int checkDay = 0; checkDay < 7; checkDay++) {
    if (!sysConfig.schedule[checkDay].active) continue;

    int almTotal = sysConfig.schedule[checkDay].hour * 60 + sysConfig.schedule[checkDay].minute;
    int minUntilAlarm;

    if (checkDay == curDay) {
      minUntilAlarm = almTotal - curTotal;
      if (minUntilAlarm < 0) minUntilAlarm += 1440;
      // Block within 30 min before OR after today's alarm
      // (high value near 1440 means we just passed the alarm)
      if (minUntilAlarm <= 30 || minUntilAlarm >= 1410) {
        return true;
      }
    } else if (checkDay == ((curDay + 1) % 7)) {
      minUntilAlarm = (1440 - curTotal) + almTotal;
      // Block only within 30 min before next day's alarm
      if (minUntilAlarm <= 30) {
        return true;
      }
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
      blindPositionPct     = (blindManualDirection == 1) ? 100 : 0;
      saveBlindPosition();
      blindManualActive    = false;
      blindManualDirection = 0;
    } else {
      setMotor(blindManualDirection, calcMotorPWM(elapsed, blindRunTotalMs, blindRunFullMs));
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
        int startPos  = (blindPositionPct == -1) ? 0 : blindPositionPct;
        int remainPct = 100 - startPos;
        if (remainPct > 0) {
          blindRunStartPos     = startPos;
          blindManualActive    = true;
          blindManualDirection = 1;
          blindRunStartMs      = millis();
          blindRunFullMs       = (unsigned long)sysConfig.blindOpenDuration * 1000UL;
          blindRunTotalMs      = blindRunFullMs * (unsigned long)remainPct / 100UL;
        }
        blindTriggered = true;
      }

      // Trigger pump (water alarm)
      if (sysConfig.pumpEnabled && sysConfig.schedule[curDay].active && curTotal == almTotal) {
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
