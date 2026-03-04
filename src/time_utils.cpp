#include "time_utils.h"
#include "globals.h"

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
