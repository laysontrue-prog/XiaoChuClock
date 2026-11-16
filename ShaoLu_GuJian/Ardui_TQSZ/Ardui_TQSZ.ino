#include <OneButton.h>
#include "net.h"
#include <WiFi.h>
#include "common.h"
#include "PreferencesUtil.h"
#include "tftUtil.h"
#include "task.h"
#include "todo.h"
#include "photo.h"

/**
XiaoChu Clock  版本1.0
作者：抖音：小楚君cc
*/


unsigned int prevDisplay = 0; // 实况天气页面上次显示的时间
int synDataRestartTime = 60; // 同步NTP时间和天气信息时，超过多少秒就重启系统，防止网络不好时，傻等
OneButton myButton(BUTTON, true);


void setup() {
  Serial.begin(115200);
  // ===== 开机里程碑打印：卡在哪一步，串口最后一条就是答案 =====
  Serial.println();
  Serial.println("========================================");
  Serial.printf("[BOOT 1] 复位完成 芯片=%s 主频=%uMHz Flash=%uKB\n",
                ESP.getChipModel(), getCpuFrequencyMhz(), ESP.getFlashChipSize() / 1024);
  Serial.printf("[BOOT 2] 空闲内存 %u 字节\n", ESP.getFreeHeap());
  delay(150);

  // TFT初始化
  Serial.println("[BOOT 3] 开始 TFT 初始化 (SPI 引脚 MOSI=3 SCLK=2 CS=7 DC=4 RST=5)...");
  Serial.flush();
  tftInit();
  Serial.println("[BOOT 4] TFT 初始化完成 ✅");
  Serial.flush();

  // 显示系统启动文字
  drawText("系统启动中...");
  Serial.println("[BOOT 5] 已显示启动文字 ✅");
  Serial.flush();

  // 初始化相册的 JPEG 解码器
  photoInit();

  // 挂载 SPIFFS 并载入待办事项（点阵存在 SPIFFS 里）
  Serial.println("[BOOT 6] 挂载 SPIFFS（首次会格式化，可能要十几秒）...");
  Serial.flush();
  todoBegin();
  Serial.println("[BOOT 7] SPIFFS 挂载完成 ✅");
  Serial.flush();

  // 测试的时候，先写入WiFi信息，省的配网，生产环境请注释掉
  // setInfo4Test();
  // 查询是否有配置过Wifi，没有->进入Wifi配置页面（0），有->进入天气时钟页面（1）
  // 设置国家码（开放1~13信道）
  setupWiFiCountry();
  getWiFiCity();
  Serial.printf("[NVS] 读到配置: ssid=\"%s\" pass长度=%u city=\"%s\" loc=\"%s\"\n",
                ssid.c_str(), pass.length(), city.c_str(), location.c_str());
  // nvs中没有WiFi信息，进入配置页面
  if(ssid.length() == 0 || pass.length() == 0 || city.length() == 0 ){
    currentPage = SETTING; // 将页面置为配置页面
    Serial.println("[BOOT 8] 没有 WiFi 配置 -> 进入配网模式（发热点 + 显示二维码）");
    Serial.flush();
    wifiConfigBySoftAP(); // 开启SoftAP配置WiFi
    Serial.println("[BOOT 9] 热点与配网服务器已就绪 ✅");
    Serial.flush();
  }else{ // 有WiFi信息，连接WiFi后进入时钟页面
    currentPage = WEATHER; // 将页面置为时钟页面
    Serial.printf("[BOOT 8] 已有配置，连接 WiFi \"%s\"...\n", ssid.c_str());
    Serial.flush();
    // 连接WiFi,30秒超时重启并恢复出厂设置
    connectWiFi(30); 
    // 启动 HTTP 服务器，这样手机 APP 在正常使用时也能连上（/api/*）
    startServer();
    // 查询是否有城市id，如果没有，就利用city和adm查询出城市id，并保存为location
    if(location.equals("")){
      getCityID();
    }
    // 初始化一些列数据:NTP对时、实况天气、一周天气
    initDatas();
    // 绘制实况天气页面
    drawWeatherPage();
    // 多任务启动
    startRunner();
    // 初始化定时器，让查询天气的多线程任务在一小时后再使能
    startTimerQueryWeather();
    // 初始化按键监控
    myButton.attachClick(click);
    myButton.attachDoubleClick(doubleclick);
    myButton.attachLongPressStart(longclick);
    myButton.setPressMs(2000); //设置长按时间
    // myButton.setClickMs(300); //设置单击时间
    myButton.setDebounceMs(10); //设置消抖时长 
    Serial.println("[BOOT 9] 时钟已就绪 ✅");

    Serial.flush();
  }
  Serial.println("===== 启动流程结束，进入主循环 =====");
}

void loop() {
  myButton.tick();
  // HTTP 服务器常驻：任何页面都响应配网页面和手机 APP 的 /api/* 请求
  doClient();
  // 处理待办点阵的延迟落盘
  todoTick();
  // 推进待办"选中行右移"的小动效（非阻塞，每步只重画一行）
  todoAnimTick();
  // 开机同步数据时的提示文字（必须在主循环里画，不能在中断里画）
  tipsTick();
  // 每10秒打印一次状态心跳（方便随时抓串口定位问题）
  static uint32_t lastHeartbeat = 0;
  if(millis() - lastHeartbeat > 10000){
    lastHeartbeat = millis();
    Serial.printf("[心跳] WiFi状态=%d IP=%s ssid=\"%s\" pass长度=%u city=\"%s\" 空闲内存=%u\n",
                  WiFi.status(), WiFi.localIP().toString().c_str(),
                  ssid.c_str(), pass.length(), city.c_str(), ESP.getFreeHeap());
  }
  switch(currentPage){
    case SETTING:  // 配置页面
      break;
    case WEATHER:  // 天气时钟页面
      executeRunner();
      time_t now;
      time(&now);
      if(now != prevDisplay){ // 每秒更新一次时间显示
        prevDisplay = now;
        // 绘制时间、日期、星期
        drawDateWeek();
      }
      break;
    case TODO:  // 待办事项页面
      break;
    case PHOTO:  // 相册页面
      break;
    case FUTUREWEATHER:  // 未来天气页面
      executeRunner();
      break;
    case THEME:  // 黑白主题设置页面
      executeRunner();
      break;
    case RESET:  // 恢复出厂设置页面
      executeRunner();
      break;
    default:
      break;
  }
}

////////////////////////// 按键区///////////////////////
// 单击操作，用来切换各个页面
void click(){
  if(currentPage == TODO){
    // 待办事项页：单击移动选中框（选中项会同步到 APP）
    todoMoveCursor(1);
  }
}
void doubleclick(){
  switch(currentPage){
    case WEATHER:
      disableAnimScrollText();
      drawFutureWeatherPage();
      currentPage = FUTUREWEATHER;
      break;
    case FUTUREWEATHER:
      todoDrawPage();
      currentPage = TODO;
      break;
    case TODO:
      photoDrawPage();
      currentPage = PHOTO;
      break;
    case PHOTO:
      drawThemePage();
      currentPage = THEME;
      break;
    case THEME:
      drawResetPage();
      currentPage = RESET;
      break;
    case RESET:
      drawWeatherPage();
      enableAnimScrollText();
      currentPage = WEATHER;
      break;
    default:
      break;
  }
}
void longclick(){
  if(currentPage == RESET){
    Serial.println("恢复出厂设置");
    // 恢复出厂设置并重启
    clearWiFiCity();
    restartSystem("已恢复出厂设置", false);
  }else if(currentPage == THEME){
    Serial.println("更改主题");
    if(backColor == BACK_BLACK){ // 原先为黑色主题，改为白色
      backColor = BACK_WHITE;
      backFillColor = 0xFFFF;
      penColor = 0x0000;
    }else{
      backColor = BACK_BLACK;
      backFillColor = 0x0000;
      penColor = 0xFFFF;
    }
    // 将新的主题存入nvs
    setBackColor(backColor);
    // 返回实时天气页面
    drawWeatherPage();
    enableAnimScrollText();
    currentPage = WEATHER;
  }else if(currentPage == PHOTO){
    // 相册页：长按 3 秒切换到下一张照片
    photoNext();
  }else if(currentPage == TODO){
    // 待办事项页：长按把当前选中项标记为完成/未完成
    todoToggleCursor();
  }
}
////////////////////////////////////////////////////////


// 初始化一些列数据:NTP对时、实况天气、一周天气
void initDatas(){
  startTimerShowTips(); // 获取数据时，循环显示提示文字
  // 记录此时的时间，在同步数据时，超过一定的时间，就直接重启
  time_t start;
  time(&start);
  // 获取NTP并同步至RTC,第一次同步失败，就一直尝试同步
  getNTPTime();
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)){
    time_t end;
    time(&end);
    if((end - start) > synDataRestartTime){
      restartSystem("同步数据失败", true);
    }
    Serial.println("时钟对时失败...");
    getNTPTime();
  }
  //第一次查询实况天气,如果查询失败，就一直反复查询
  getNowWeather();
  while(!queryNowWeatherSuccess){
    time_t end;
    time(&end);
    if((end - start) > synDataRestartTime){
      restartSystem("同步数据失败", true);
    }
    getNowWeather();
  }
  // 空气质量接口 /v7/air/now 已被和风天气废弃（返回 403）。
  // 取不到就跳过，不再无限重试、也不再因它重启整机，
  // 否则设备会陷入"同步失败→重启→再同步失败"的死循环。
  getAir();
  for (int i = 0; i < 2 && !queryAirSuccess; i++) {
    delay(2000);
    getAir();
  }
  if (!queryAirSuccess) {
    Serial.println("[天气] 空气质量接口已废弃，跳过空气质量数据（不影响时钟和天气）");
  }
  //第一次查询一周天气,如果查询失败，就一直反复查询
  getFutureWeather();
  while(!queryFutureWeatherSuccess){
    time_t end;
    time(&end);
    if((end - start) > synDataRestartTime){
      restartSystem("同步数据失败", true);
    }
    getFutureWeather();
  }
  //结束循环显示提示文字的定时器
  // 必须用 stopTipsTimer()：它会把可能残留的 showTipsPending 标志一起清掉。
  // 否则等下面 drawWeatherPage() 画完首页，主循环会再消费一次这个残留标志，
  // 于是又画一遍提示 —— 首页的标题/天气内容/温湿度图标就被擦掉且再也回不来，
  // 只能靠双击切页重新调用 drawWeatherPage() 才恢复。
  stopTipsTimer();
  //将isStartQuery置为false,告诉系统，启动时查询天气已完成
  isStartQuery = false;
}

