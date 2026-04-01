#include "time_utils.h"
#include "globals.h"

// Returns the epoch (UTC) of the last Sunday of `month` at 01:00 UTC for `year`.
static unsigned long lastSundayEpoch(int year, int month) {
  static const int daysInMonth[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));

  unsigned long days = 0;
  for (int y = 1970; y < year; y++) {
    bool ly = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    days += ly ? 366 : 365;
  }
  for (int m = 0; m < month - 1; m++) {
    days += daysInMonth[m] + (m == 1 && leap ? 1 : 0);
  }
  int lastDay = daysInMonth[month - 1] + (month == 2 && leap ? 1 : 0);
  unsigned long lastDayAbs = days + lastDay - 1;
  int dow = (lastDayAbs + 4) % 7; // 0=Sun, 1=Mon, …
  unsigned long lastSunday = lastDayAbs - dow;

  return lastSunday * 86400UL + 3600UL; // 01:00 UTC
}

int getItalyUTCOffset(unsigned long epochUTC) {
  // Approximate year (corrected below if needed)
  int year = 1970;
  unsigned long rem = epochUTC;
  while (true) {
    bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    unsigned long sy = (leap ? 366UL : 365UL) * 86400UL;
    if (rem < sy) break;
    rem -= sy;
    year++;
  }

  unsigned long dstStart = lastSundayEpoch(year, 3);  // last Sun March  01:00 UTC
  unsigned long dstEnd   = lastSundayEpoch(year, 10); // last Sun October 01:00 UTC

  return (epochUTC >= dstStart && epochUTC < dstEnd) ? 7200 : 3600;
}

String formatTime(int h, int m) {
  char buffer[6];
  sprintf(buffer, "%02d:%02d", h, m);
  return String(buffer);
}

String getWakeTime(float hoursFromNow) {
  int totalMinToAdd = (int)(hoursFromNow * 60) + sysConfig.fallingAsleepMinutes;
  int currentTotalMin = (timeClient.getHours() * 60) + timeClient.getMinutes();
  int resultMin = (currentTotalMin + totalMinToAdd) % 1440;
  return formatTime(resultMin / 60, resultMin % 60);
}

String getBedTime(int h, int m, float sleepHours) {
  int totalMinToSub = (int)(sleepHours * 60) + sysConfig.fallingAsleepMinutes;
  int targetTotalMin = (h * 60) + m;
  int resultMin = (targetTotalMin - totalMinToSub);
  while (resultMin < 0) resultMin += 1440;
  return formatTime(resultMin / 60, resultMin % 60);
}
