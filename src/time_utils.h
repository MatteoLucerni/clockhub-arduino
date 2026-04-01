#pragma once

#include <Arduino.h>

String formatTime(int h, int m);
String getWakeTime(float hoursFromNow);
String getBedTime(int h, int m, float sleepHours);

// Returns UTC offset in seconds for Italy (Europe/Rome):
// 7200 during CEST (last Sun March → last Sun October), else 3600 (CET).
int getItalyUTCOffset(unsigned long epochUTC);
