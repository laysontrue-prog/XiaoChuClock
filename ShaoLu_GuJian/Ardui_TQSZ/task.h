#ifndef __TASK_H
#define __TASK_H

#include "common.h"

void startTimerShowTips();
void tipsTick();
void stopTipsTimer();
void startTimerQueryWeather();
extern String scrollText[5];
extern hw_timer_t *timerQueryWeather;
extern hw_timer_t *timerShowTips;
extern NowWeather nowWeather;
extern FutureWeather futureWeather;
extern enum CurrentPage currentPage;
void drawWeatherContent();
void drawFutureWeatherPage();
void drawWeatherPage();
void drawResetPage();
void drawThemePage();
void drawDateWeek();
String week(int tm_wday);
String monthDay(int tm_mon, int tm_mday);
void startRunner();
void executeRunner();
void disableAnimScrollText();
void enableAnimScrollText();

#endif

