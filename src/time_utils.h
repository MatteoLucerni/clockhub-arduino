#pragma once

#include <Arduino.h>

String formatTime(int h, int m);
String getWakeTime(float hoursFromNow);
String getBedTime(int h, int m, float sleepHours);
