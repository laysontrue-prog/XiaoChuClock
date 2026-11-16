#ifndef __NET_H
#define __NET_H
#include "common.h"

// 注意：这里只做前向声明，不要 #include <WebServer.h>！
// TFT_eSPI 会 #define FS_NO_GLOBALS，若它先于 WebServer.h 被包含，
// 全局的 FS / File 类型就没了，WebServer.h 会编译不过。
class WebServer;
extern WebServer server;

void wifiConfigBySoftAP(void);
void startServer(void);
void doClient(void);
void connectWiFi(int timeOut_s);
void setupWiFiCountry(void);
void getNowWeather(void);
void getFutureWeather(void);
void getAir(void);
void getNTPTime(void);
void getCityID(void);
void checkWiFiStatus(void);
void restartSystem(String msg, bool endTips);
extern bool queryNowWeatherSuccess;
extern bool queryFutureWeatherSuccess;
extern bool queryAirSuccess;
extern String ssid;
extern String pass;
extern String city;
extern String adm;
extern String location;
extern bool isStartQuery;

#endif