#include <TaskScheduler.h>
#include "tftUtil.h"
#include "net.h"
#include "todo.h"
#include <WiFi.h>          // 待办页底部要显示 IP
#include "common.h"

enum CurrentPage currentPage = SETTING;
String scrollText[5]; // 轮播天气的信息
int currentIndex = 0; // 轮播索引
int currnetImgAnimIndex = 0; // 太空人动画索引
int tipsIndex = 0; // 获取数据时，动态文字的索引
NowWeather nowWeather; // 记录查询到的实况天气数据
FutureWeather futureWeather; // 记录查询到的七日天气数据

void drawWeatherContent();
void drawFutureWeatherPage();
void doScrollTextData(String win, int air, String text, String feelsLike, String vis);
void drawWeaImage(int icon);
void drawFutureWeaImage(int wea_img, int x, int y);
void drawCityAir(String city,int air);
void drawTHProgressBar(int temperature, int humidity);
void drawDateWeek();
void drawTitle();
String getWea(int icon);
String week(int tm_wday);
String monthDay(int tm_mon, int tm_mday);
void doHourMinuteSecond(int hour,int minute,int second,int *hourH,int *hourL,int *minuteH,int *minuteL,int *secondH,int *secondL);


//////////////////////////////// 多任务区域/////////////////////////////
void tAnimCallback();
void tScrollTextCallback();
void tQueryWeatherCallback();
void tQueryFutureWeatherCallback();
void tCheckWiFiCallback();
void tCheckTimeCallback();
Scheduler runner;
Task tAnim(30, TASK_FOREVER, &tAnimCallback, &runner, true); // 30毫秒播放一帧太空人动画
Task tScrollText(5000, TASK_FOREVER, &tScrollTextCallback, &runner, true); // 5秒轮播一条天气情况
Task tQueryWeather(60 * 60 * 1000, TASK_FOREVER, &tQueryWeatherCallback, &runner, false); // 60分钟查询一次实况天气情况
Task tQueryFutureWeather(71 * 60 * 1000, TASK_FOREVER, &tQueryFutureWeatherCallback, &runner, false); // 71分钟查询一次一周天气情况
Task tCheckWiFi(5 * 60 * 1000, TASK_FOREVER, &tCheckWiFiCallback, &runner, true); // 5分钟检查一次网络状态
Task tCheckTime(58 * 60 * 1000, TASK_FOREVER, &tCheckTimeCallback, &runner, true); // 58分钟进行一次NTP对时
// 启动runner
void startRunner(){
  runner.startNow();
}
// 执行runner
void executeRunner(){
  runner.execute();
}
// 切换到非实况天气页面时，失能太空人动画和轮播天气情况的线程
void disableAnimScrollText(){
  tAnim.disable();
  tScrollText.disable();
}
// 切换回实况天气页面时，再开启太空人动画和轮播天气情况的线程
void enableAnimScrollText(){
  tAnim.enable();
  tScrollText.enable();
}
////////////////////////////////////////////////////////////////////////


//////////////// 定时器相关区域//////////////////////////////////////////
// 定义定时器
hw_timer_t *timerQueryWeather = NULL;
hw_timer_t *timerShowTips = NULL;
void IRAM_ATTR onTimerQueryWeather() {
  // 使能查询天气的多线程任务
  tQueryWeather.enable();
  tQueryFutureWeather.enable();
} 
// 【重要修复】中断里绝对不能绘图！
// 原来这里直接调用 draw2LineText()，它会在中断上下文里 malloc 57.6KB 画布、
// 操作 SPI、并且和主循环抢同一个 clk 画布对象。中断里 malloc 与主循环的
// HTTP/JSON 分配并发操作堆会导致堆损坏；同时 clk 被中断打断会打乱绘制状态，
// 表现就是：文字偶尔跑到左上角、变成乱码、画不出来，严重时直接崩溃重启。
// 正确做法（和 DuduClock 2.0 一致的思路）：中断只置一个标志，真正绘图放到主循环。
volatile bool showTipsPending = false;
void IRAM_ATTR onTimerShowTips() {
  showTipsPending = true;   // 中断里只做这一件事
}

// 在主循环里调用：真正绘制"同步天气数据"的提示文字
void tipsTick(){
  if(!showTipsPending) return;
  showTipsPending = false;
  static const char* dots[6] = {".", "..", "...", "....", ".....", "......"};
  draw2LineText("同步天气数据", dots[tipsIndex]);
  tipsIndex++;
  if(tipsIndex == 6){
    tipsIndex = 0;
  }
}

// 停止"同步天气数据"提示定时器。
// ★ 必须连 showTipsPending 一起清掉：定时器停了不等于标志没了。
// 如果最后一次中断刚好把标志置为 true，而它还没被主循环消费，
// 那么等 drawWeatherPage() 把首页画完之后，主循环会再消费一次这个残留标志，
// 于是又画一遍提示 —— 首页上那些"没有定时任务重绘"的内容（标题、天气内容、
// 温湿度图标）就被擦掉且再也回不来。这就是烧录/配网后首页错乱、切页才恢复的原因。
void stopTipsTimer(){
  if(timerShowTips != NULL){
    timerEnd(timerShowTips);
    timerShowTips = NULL;
  }
  showTipsPending = false;
  tipsIndex = 0;
}
// 初始化定时器，让查询天气的多线程任务在一小时后再使能
void startTimerQueryWeather(){
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  // ESP32 Arduino core 3.x 定时器API
  timerQueryWeather = timerBegin(1000000);// 1us计数一次
  timerAttachInterrupt(timerQueryWeather, &onTimerQueryWeather);
  timerAlarm(timerQueryWeather, 3600000000ULL, false, 0); // 执行完一次后就取消这个定时器 (3600000000)3600秒，即60分钟后使能
  timerStart(timerQueryWeather);
#else
  timerQueryWeather = timerBegin(0, 80, true);// 1us计数一次
  timerAttachInterrupt(timerQueryWeather, &onTimerQueryWeather, true); 
  timerAlarmWrite(timerQueryWeather, 3600000000, false); // 执行完一次后就取消这个定时器 (3600000000)3600秒，即60分钟后使能
  timerAlarmEnable(timerQueryWeather); 
#endif
}
// 初始化定时器，在获取初始数据时，给用户提示
void startTimerShowTips(){
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  // ESP32 Arduino core 3.x 定时器API
  timerShowTips = timerBegin(1000000);// 1us计数一次
  timerAttachInterrupt(timerShowTips, &onTimerShowTips);
  timerAlarm(timerShowTips, 1000000ULL, true, 0); // 每秒执行一次
  timerStart(timerShowTips);
#else
  timerShowTips = timerBegin(1, 80, true);// 1us计数一次
  timerAttachInterrupt(timerShowTips, &onTimerShowTips, true); 
  timerAlarmWrite(timerShowTips, 1000000, true); // 每秒执行一次
  timerAlarmEnable(timerShowTips); 
#endif
}
///////////////////////////////////////////////////////////////////////


/////////// 绘制整个页面区域////////////////////////////////////////////
// 绘制一周天气页面
void drawFutureWeatherPage(){
  reflashTFT(); // 清空屏幕
  // 绘制标题
  clk.createSprite(240, 60);
  clk.loadFont(clock_other_title_35);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(penColor);
  clk.fillSprite(backFillColor);  
  clk.drawString("6日天气预报",120,30);
  clk.drawFastHLine(0,56,240,penColor);
  clk.drawFastHLine(0,58,240,penColor);
  clk.pushSprite(0,0);
  clk.deleteSprite();
  // 绘制天气图案
  drawFutureWeaImage(futureWeather.day1wea_img, 15, 65);
  drawFutureWeaImage(futureWeather.day2wea_img, 95, 65);
  drawFutureWeaImage(futureWeather.day3wea_img, 175, 65);
  drawFutureWeaImage(futureWeather.day4wea_img, 15, 195);
  drawFutureWeaImage(futureWeather.day5wea_img, 95, 195);
  drawFutureWeaImage(futureWeather.day6wea_img, 175, 195);
  // 绘制上部分文字
  clk.loadFont(clock_future_weather_20);
  clk.setTextDatum(CC_DATUM);
  clk.createSprite(240, 70);
  clk.setTextColor(penColor);
  clk.fillSprite(backFillColor);
  clk.drawString(futureWeather.day1date.substring(5),40,10);
  clk.drawString(futureWeather.day1wea,40,35);
  clk.drawString(String(futureWeather.day1tem_night) + "-" + String(futureWeather.day1tem_day) + "℃",40,60);
  clk.drawString(futureWeather.day2date.substring(5),120,10);
  clk.drawString(futureWeather.day2wea,120,35);
  clk.drawString(String(futureWeather.day2tem_night) + "-" + String(futureWeather.day2tem_day) + "℃",120,60);
  clk.drawString(futureWeather.day3date.substring(5),200,10);
  clk.drawString(futureWeather.day3wea,200,35);
  clk.drawString(String(futureWeather.day3tem_night) + "-" + String(futureWeather.day3tem_day) + "℃",200,60);
  clk.pushSprite(0,115);
  clk.deleteSprite();
  // 绘制下部分文字
  clk.createSprite(240, 70);
  clk.setTextColor(penColor);
  clk.fillSprite(backFillColor);
  clk.drawString(futureWeather.day4date.substring(5),40,10);
  clk.drawString(futureWeather.day4wea,40,35);
  clk.drawString(String(futureWeather.day4tem_night) + "-" + String(futureWeather.day4tem_day) + "℃",40,60);
  clk.drawString(futureWeather.day5date.substring(5),120,10);
  clk.drawString(futureWeather.day5wea,120,35);
  clk.drawString(String(futureWeather.day5tem_night) + "-" + String(futureWeather.day5tem_day) + "℃",120,60);
  clk.drawString(futureWeather.day6date.substring(5),200,10);
  clk.drawString(futureWeather.day6wea,200,35);
  clk.drawString(String(futureWeather.day6tem_night) + "-" + String(futureWeather.day6tem_day) + "℃",200,60);
  clk.pushSprite(0,245);
  clk.deleteSprite();
  clk.unloadFont();
}
// 绘制实况天气页面
void drawWeatherPage(){
  //清空屏幕
  reflashTFT();
  //绘制标题
  drawTitle();
  //绘制时间、日期、星期
  drawDateWeek();
  //绘制温湿度图标
  if(backColor == BACK_BLACK){
    tft.pushImage(7,235,40,40,temperature_black);
    tft.pushImage(5,275,40,40,humidity_black);
  }else{
    tft.pushImage(7,235,40,40,temperature);
    tft.pushImage(5,275,40,40,humidity);
  } 
  // 绘制天气相关内容
  drawWeatherContent();
  Serial.println("[DRAW] drawWeatherPage 绘制完成（标题/时间/日期/天气内容/图标）");
}
// 绘制计时器页面
// 绘制出厂设置页面
void drawResetPage(){
  //清空屏幕
  reflashTFT();
  // 绘制标题
  // 【防残留】先确保没有已存在的画布：createSprite 在 _created 为真时会直接返回旧缓冲区
  // （不清零），上一页的字就会残留在左上角。先删掉，才能拿到 calloc 出来的全新零缓冲区。
  if (clk.created()) clk.deleteSprite();
  clk.setColorDepth(8);
  clk.setTextWrap(false);          // 标题永不换行
  if (clk.createSprite(240, 60) == nullptr) return;
  clk.loadFont(clock_other_title_35);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(penColor);
  clk.fillSprite(backFillColor);  
  clk.drawString("还原设置",120,30);
  clk.drawFastHLine(0,56,240,penColor);
  clk.drawFastHLine(0,58,240,penColor);
  clk.pushSprite(0,0);
  clk.deleteSprite();
  // 绘制提示文字（同上：直接画到屏幕，不占画布内存）
  clk.unloadFont();
  tft.loadFont(clock_tips_28);
  tft.setTextDatum(CC_DATUM);
  tft.setTextColor(penColor);
  tft.drawString(F("长按3秒钟"), 120, 150);
  tft.drawString(F("恢复出厂设置"), 120, 200);
  tft.unloadFont();
}
// 绘制主题设置页面
void drawThemePage(){
  // 清空屏幕
  reflashTFT();
  // 绘制标题
  // 【防残留】同上：先释放旧画布，保证拿到的是全新零缓冲区
  if (clk.created()) clk.deleteSprite();
  clk.setColorDepth(8);
  clk.setTextWrap(false);          // 标题永不换行
  if (clk.createSprite(240, 60) == nullptr) return;
  clk.loadFont(clock_other_title_35);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(penColor);
    clk.fillSprite(backFillColor);      // 【关键】2.0 原码漏了这行：主题页不清画布，白主题下会变成一块纯黑
  clk.drawString("主题切换",120,30);
  clk.drawFastHLine(0,56,240,penColor);
  clk.drawFastHLine(0,58,240,penColor);
  clk.pushSprite(0,0);
  clk.deleteSprite();
  // 绘制提示文字
  // 【为什么不用画布】原来这里开的是 240x260 的 8bpp 画布（62.4KB），
  // 而本工程可用堆只有约 59KB，且不一定连续 —— 分配必然失败，提示文字就整块消失。
  // 平滑字体可以直接画到屏幕上，完全不需要画布。
  clk.unloadFont();
  tft.loadFont(clock_tips_28);
  tft.setTextDatum(CC_DATUM);
  tft.setTextColor(penColor);
  tft.drawString(F("长按3秒钟"), 120, 150);
  tft.drawString(backColor == BACK_BLACK ? F("切换白色主题") : F("切换黑色主题"), 120, 200);
  tft.unloadFont();
}
/////////////////////////////////////////////////////////////////////


/////////// 多任务协程回调区域 ////////////////////////////////////////
// NTP对时
void tCheckTimeCallback(){
  getNTPTime();
}
// 检查Wifi状态,如果失败，重新连接
void tCheckWiFiCallback(){
  Serial.println("开始检查网络状态");
  checkWiFiStatus();
}
// 绘制太空人任务
void tAnimCallback(){
  int x=170, y=240 , w=70;
  if(backColor == BACK_BLACK){
    switch(currnetImgAnimIndex){
      case 0:
        tft.pushImage(x, y, w, w,yuhangyuan0_black);
        break;
      case 1:
        tft.pushImage(x, y, w, w,yuhangyuan1_black);
        break;
      case 2:
        tft.pushImage(x, y, w, w,yuhangyuan2_black);
        break;
      case 3:
        tft.pushImage(x, y, w, w,yuhangyuan3_black);
        break;
      case 4:
        tft.pushImage(x, y, w, w,yuhangyuan4_black);
        break;
      case 5:
        tft.pushImage(x, y, w, w,yuhangyuan5_black);
        break;
      case 6:
        tft.pushImage(x, y, w, w,yuhangyuan6_black);
        break;
      case 7:
        tft.pushImage(x, y, w, w,yuhangyuan7_black);
        break;
      case 8:
        tft.pushImage(x, y, w, w,yuhangyuan8_black);
        break;
      case 9:
        tft.pushImage(x, y, w, w,yuhangyuan9_black);
        break;
      default:
        currnetImgAnimIndex = 9;
        break;
    }
  }else{
    switch(currnetImgAnimIndex){
      case 0:
        tft.pushImage(x, y, w, w,yuhangyuan0);
        break;
      case 1:
        tft.pushImage(x, y, w, w,yuhangyuan1);
        break;
      case 2:
        tft.pushImage(x, y, w, w,yuhangyuan2);
        break;
      case 3:
        tft.pushImage(x, y, w, w,yuhangyuan3);
        break;
      case 4:
        tft.pushImage(x, y, w, w,yuhangyuan4);
        break;
      case 5:
        tft.pushImage(x, y, w, w,yuhangyuan5);
        break;
      case 6:
        tft.pushImage(x, y, w, w,yuhangyuan6);
        break;
      case 7:
        tft.pushImage(x, y, w, w,yuhangyuan7);
        break;
      case 8:
        tft.pushImage(x, y, w, w,yuhangyuan8);
        break;
      case 9:
        tft.pushImage(x, y, w, w,yuhangyuan9);
        break;
      default:
        currnetImgAnimIndex = 9;
        break;
    }
  }
  currnetImgAnimIndex += 1;
  if(currnetImgAnimIndex == 10){
    currnetImgAnimIndex = 0;
  } 
}
// 轮播ScrollText任务
void tScrollTextCallback(){
  if(scrollText[currentIndex]){
    clk.setColorDepth(8);
    clk.loadFont(city_wea_24);
    for(int pos = 24; pos>0 ; pos--){
      clk.createSprite(160, 24);
      clk.setTextColor(penColor);
      clk.fillSprite(backFillColor);
      clk.setTextDatum(CC_DATUM);
      clk.drawString(scrollText[currentIndex],80,pos+12);
      clk.pushSprite(5,102);
    }
    clk.deleteSprite();
    clk.unloadFont();
    if(currentIndex>=4){
      currentIndex = 0;
    }else{
      currentIndex += 1;
    }
    // Serial.print("当前轮播第");Serial.print(currentIndex);Serial.println("条信息");
  }
}
// 查询天气和空气质量
void tQueryWeatherCallback(){
  getNowWeather();
  getAir();
  if(queryNowWeatherSuccess || queryAirSuccess){
    Serial.println("更新实时天气信息");
    // 如果是在实况天气页面，则绘制天气相关内容
    if(currentPage == WEATHER){
      drawWeatherContent();
    }
  }
}
// 查询未来天气
void tQueryFutureWeatherCallback(){
  getFutureWeather();
  if(queryFutureWeatherSuccess){ // 查询一周天气成功，再继续下面的操作
    Serial.println("更新一周天气信息");
    // 如果是在一周天气页面，则绘制一周天气相关内容
    if(currentPage == FUTUREWEATHER){
      drawFutureWeatherPage();
    }
  }
}
/////////////////////////////////////////////////////////////////////


// 根据计数器当前的数值进行绘制
// 绘制天气相关内容
void drawWeatherContent(){
  //将显示温湿度文字的地方先置为白色，防止温度刷新时，屏幕有残影
  clk.createSprite(57, 80);
  clk.fillSprite(backFillColor);  
  clk.pushSprite(113,230);
  clk.deleteSprite();
  //绘制温湿度条
  drawTHProgressBar(nowWeather.temp,nowWeather.humidity);
  //绘制城市、空气质量
  drawCityAir(city,nowWeather.air);
  //绘制天气图案和文字
  drawWeaImage(nowWeather.icon);
  //处理轮播字条数据(风向win,空气指数air,天气实况text,体感温度feelsLike,能见度vis)
  doScrollTextData(nowWeather.win, nowWeather.air, nowWeather.text, nowWeather.feelsLike, nowWeather.vis);
}
//处理轮播字条数据(风向win,空气指数air,天气实况text,体感温度feelsLike,能见度vis)
void doScrollTextData(String win, int air, String text, String feelsLike, String vis){
  scrollText[0] = "";
  scrollText[1] = "";
  scrollText[2] = "";
  scrollText[3] = "";
  scrollText[4] = "";
  scrollText[0] = "今日" + text;
  scrollText[1] = win;
  scrollText[2] = feelsLike;
  scrollText[3] = vis;
  scrollText[4] += "空气指数";
  scrollText[4] += air;
}
//绘制天气图案和文字
void drawWeaImage(int icon){
  String s = getWea(icon);
  if(backColor == BACK_BLACK){
    if(s.equals("雪")){
      tft.pushImage(180,55,50,50,xue_black);
    }else if(s.equals("雷")){
      tft.pushImage(180,55,50,50,lei_black);
    }else if(s.equals("沙尘")){
      tft.pushImage(180,55,50,50,shachen_black);
    }else if(s.equals("雾")){
      tft.pushImage(180,55,50,50,wu_black);
    }else if(s.equals("冰雹")){
      tft.pushImage(180,55,50,50,bingbao_black);
    }else if(s.equals("多云")){
      tft.pushImage(180,55,50,50,yun_black);
    }else if(s.equals("雨")){
      tft.pushImage(180,55,50,50,yu_black);
    }else if(s.equals("阴")){
      tft.pushImage(180,55,50,50,yin_black);
    }else if(s.equals("晴")){
      tft.pushImage(180,55,50,50,qing_black);
    }
  }else{
    if(s.equals("雪")){
      tft.pushImage(180,55,50,50,xue);
    }else if(s.equals("雷")){
      tft.pushImage(180,55,50,50,lei);
    }else if(s.equals("沙尘")){
      tft.pushImage(180,55,50,50,shachen);
    }else if(s.equals("雾")){
      tft.pushImage(180,55,50,50,wu);
    }else if(s.equals("冰雹")){
      tft.pushImage(180,55,50,50,bingbao);
    }else if(s.equals("多云")){
      tft.pushImage(180,55,50,50,yun);
    }else if(s.equals("雨")){
      tft.pushImage(180,55,50,50,yu);
    }else if(s.equals("阴")){
      tft.pushImage(180,55,50,50,yin);
    }else if(s.equals("晴")){
      tft.pushImage(180,55,50,50,qing);
    }
  }
  clk.setColorDepth(8);
  clk.setTextDatum(CC_DATUM);
  clk.loadFont(city_wea_24);
  clk.createSprite(60, 28);
  clk.fillSprite(backFillColor);
  clk.fillRoundRect(0,0,60,32,0,0x4208);// 进度条填充(x方向,y方向,长度,高度,圆角,颜色)
  clk.setTextColor(0xFFFF);
  clk.drawString(s,29,15);
  clk.pushSprite(172,120);
  clk.deleteSprite();
  clk.unloadFont(); 
}
//绘制一周天气图案
void drawFutureWeaImage(int wea_img, int x, int y){
  String s = getWea(wea_img);
  if(backColor == BACK_BLACK){
    if(s.equals("雪")){
      tft.pushImage(x,y,50,50,xue_black);
    }else if(s.equals("雷")){
      tft.pushImage(x,y,50,50,lei_black);
    }else if(s.equals("沙尘")){
      tft.pushImage(x,y,50,50,shachen_black);
    }else if(s.equals("雾")){
      tft.pushImage(x,y,50,50,wu_black);
    }else if(s.equals("冰雹")){
      tft.pushImage(x,y,50,50,bingbao_black);
    }else if(s.equals("多云")){
      tft.pushImage(x,y,50,50,yun_black);
    }else if(s.equals("雨")){
      tft.pushImage(x,y,50,50,yu_black);
    }else if(s.equals("阴")){
      tft.pushImage(x,y,50,50,yin_black);
    }else if(s.equals("晴")){
      tft.pushImage(x,y,50,50,qing_black);
    }
  }else{
    if(s.equals("雪")){
      tft.pushImage(x,y,50,50,xue);
    }else if(s.equals("雷")){
      tft.pushImage(x,y,50,50,lei);
    }else if(s.equals("沙尘")){
      tft.pushImage(x,y,50,50,shachen);
    }else if(s.equals("雾")){
      tft.pushImage(x,y,50,50,wu);
    }else if(s.equals("冰雹")){
      tft.pushImage(x,y,50,50,bingbao);
    }else if(s.equals("多云")){
      tft.pushImage(x,y,50,50,yun);
    }else if(s.equals("雨")){
      tft.pushImage(x,y,50,50,yu);
    }else if(s.equals("阴")){
      tft.pushImage(x,y,50,50,yin);
    }else if(s.equals("晴")){
      tft.pushImage(x,y,50,50,qing);
    }
  } 
}
//绘制城市、空气质量
void drawCityAir(String city,int air){
  clk.setColorDepth(8);
  clk.setTextDatum(CC_DATUM);
  //城市
  clk.loadFont(city_wea_24);
  clk.createSprite(100, 32);
  clk.fillSprite(backFillColor);
  clk.setTextColor(0xFC60);
  clk.drawString(city,50,17); 
  clk.pushSprite(0,60);
  clk.deleteSprite();
  clk.unloadFont();
  //空气质量， ≤50优（绿色），≤100良（蓝），≤150中（橙），≤200差（红）
  clk.loadFont(city_wea_24);
  String level;
  uint16_t color;
  if(air <= 50){
    color = 0x0E27;
    level = "优";
  }else if(air <= 100){
    color = 0x2C3E;
    level = "良";
  }else if(air <= 150){
    color = 0xFC60;
    level = "中";
  }else{
    color = TFT_RED;
    level = "差";
  }
  clk.createSprite(60, 32);
  clk.fillSprite(backFillColor);
  clk.fillRoundRect(0,0,60,32,7,color);// 进度条填充(x方向,y方向,长度,高度,圆角,颜色)
  clk.setTextColor(0xFFFF);
  clk.drawString(level,30,17);
  clk.pushSprite(105,60);
  clk.deleteSprite();
  clk.unloadFont();
}
// 绘制温湿度进度条和温湿度数据
void drawTHProgressBar(int temperature, int humidity){
  int temperatureLen,humidityLen;
  int totalLen = 63;
  uint16_t temperatureColor;
  //计算湿度要显示的长度
  humidityLen = (humidity / 100.0) * totalLen;
  //计算温度要显示的长度以及要显示的颜色
  if(temperature >= 40){ // 超过40度，红色高温预警
    temperatureLen = totalLen;
    temperatureColor = TFT_RED;
  }else if(temperature >= 35){ // 超过37度，橙色高温预警
    temperatureLen = totalLen - (40 - temperature);
    temperatureColor = 0xFC60;
  }else if(temperature > 0 && temperature < 35){
    temperatureLen = totalLen - (40 - temperature);
    temperatureColor = 0x2C3E;
  }else if(temperature <= 0){
    temperatureLen = totalLen - (40 - temperature);
    if(temperatureLen < 0){
      temperatureLen = 0;
    }
    temperatureColor = TFT_DARKGREY;
  }
  // Serial.print("temperatureLen:");Serial.println(temperatureLen);
  // Serial.print("humidityLen:");Serial.println(humidityLen);
  //绘制温度条
  clk.setColorDepth(8);
  clk.createSprite(65, 9); // 创建布局大小 宽x高
  clk.fillSprite(backFillColor);
  clk.drawRoundRect(0,0,65,9,2,penColor); // 画进度条外边(x方向,y方向,长度,高度,圆角,颜色)
  clk.drawRoundRect(1,0,63,9,2,penColor); // 画进度条外边(x方向,y方向,长度,高度,圆角,颜色)
  clk.drawFastHLine(1,1,61,penColor);
  clk.drawFastHLine(1,7,61,penColor);
  clk.fillRoundRect(1,2,temperatureLen,5,2,temperatureColor);// 进度条填充(x方向,y方向,长度,高度,圆角,颜色)
  clk.pushSprite(47,250); // 布局坐标
  clk.deleteSprite();
  //绘制湿度条
  clk.setColorDepth(8);
  clk.createSprite(65, 9); // 创建布局大小 宽x高
  clk.fillSprite(backFillColor);
  clk.drawRoundRect(0,0,65,9,2,penColor); // 画进度条外边(x方向,y方向,长度,高度,圆角,颜色)
  clk.drawRoundRect(1,0,63,9,2,penColor); // 画进度条外边(x方向,y方向,长度,高度,圆角,颜色)
  clk.drawFastHLine(1,1,61,penColor);
  clk.drawFastHLine(1,7,61,penColor);
  clk.fillRoundRect(1,2,humidityLen,5,2,0x0E27);// 进度条填充(x方向,y方向,长度,高度,圆角,颜色)
  clk.pushSprite(47,290); // 布局坐标
  clk.deleteSprite();
  //显示温湿度数据
  tft.loadFont(city_wea_24);
  tft.setTextColor(penColor,backFillColor);
  if(temperature < 0){
    tft.drawNumber(temperature,115,245);
  }else{
    tft.drawNumber(temperature,127,245);
  } 
  tft.drawString("℃",149,245);
  if(humidity >= 100){
    tft.drawNumber(humidity,115,285);
  }else{
    tft.drawNumber(humidity,127,285);
  }  
  tft.drawString("%",157,285);
  tft.unloadFont();
}
// 绘制时间、日期、星期
void drawDateWeek(){
  // 获取RTC时间
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)){
    Serial.println("获取RTC时间失败");
    return;
  }
  clk.setColorDepth(8);
  //中部时、分、秒
  int hourH,hourL,minuteH,minuteL,secondH,secondL;
  doHourMinuteSecond(timeinfo.tm_hour,timeinfo.tm_min,timeinfo.tm_sec,&hourH,&hourL,&minuteH,&minuteL,&secondH,&secondL);
  //时、分
  clk.createSprite(165, 60);
  clk.setTextColor(penColor);
  clk.fillSprite(backFillColor);
  clk.loadFont(clock_num_big_64);
  clk.setTextDatum(CC_DATUM);
  clk.drawNumber(hourH,20,32);
  clk.drawNumber(hourL,55,32);
  clk.drawString(":",80,32);
  clk.setTextColor(0xFC60);
  clk.drawNumber(minuteH,110,32);
  clk.drawNumber(minuteL,145,32);
  clk.unloadFont();
  clk.pushSprite(5,135);
  clk.deleteSprite();
  //秒
  clk.createSprite(55, 40);
  clk.fillSprite(backFillColor);
  clk.loadFont(clock_num_mid_35);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(TFT_RED);
  clk.drawNumber(secondH,12,20);
  clk.drawNumber(secondL,32,20);
  clk.unloadFont();
  clk.pushSprite(180,155);
  clk.deleteSprite();
  //底部日期、星期
  clk.loadFont(city_wea_24);
  //星期
  clk.createSprite(58, 25);
  clk.setTextColor(penColor);
  clk.fillSprite(backFillColor);
  clk.setTextDatum(CC_DATUM);  
  clk.drawString(week(timeinfo.tm_wday),27,16);
  clk.pushSprite(123,202);
  clk.deleteSprite();
  //月日
  clk.createSprite(98, 25);
  clk.setTextColor(penColor);
  clk.fillSprite(backFillColor);
  clk.setTextDatum(CC_DATUM);
  clk.drawString(monthDay(timeinfo.tm_mon,timeinfo.tm_mday),49,16);
  clk.pushSprite(5,202);
  clk.deleteSprite();
  clk.unloadFont();
}
// 绘制标题
void drawTitle(){
  clk.createSprite(240, 51);
  clk.setTextColor(penColor);
  clk.fillSprite(backFillColor);
  clk.loadFont(clock_title_45);
  clk.setTextDatum(CC_DATUM);
  clk.drawString("XChu Clock",120,35);
  clk.unloadFont();
  clk.drawFastHLine(0,48,240,penColor);
  clk.drawFastHLine(0,50,240,penColor);
  clk.pushSprite(0,0);
  clk.deleteSprite();
}
// 根据icon获得天气状况
String getWea(int icon){
  String s = "";
  switch (icon){
    case 100:
    case 150:
      s = "晴";
      break;
    case 104:
      s = "阴";
      break;
    case 300:
    case 301:
    case 305:
    case 306:
    case 307:
    case 308:
    case 309:
    case 310:
    case 311:
    case 312:
    case 313:
    case 314:
    case 315:
    case 316:
    case 317:
    case 318:
    case 350:
    case 351:
    case 399:
      s = "雨";
      break;
    case 101:
    case 102:
    case 103:
    case 151:
    case 152:
    case 153:
      s = "多云";
      break;
    case 304:
      s = "冰雹";
      break;
    case 500:
    case 501:
    case 502:
    case 509:
    case 510:
    case 511:
    case 512:
    case 513:
    case 514:
    case 515:
      s = "雾";
      break;
    case 503:
    case 504:
    case 507:
    case 508:
      s = "沙尘";
      break;
    case 302:
    case 303:
      s = "雷";
      break;
    case 400:
    case 401:
    case 402:
    case 403:
    case 404:
    case 405:
    case 406:
    case 407:
    case 408:
    case 409:
    case 410:
    case 456:
    case 457:
    case 499:
      s = "雪";
      break;
    default:
      s = "晴";
      break;  
  }
  return s;
}
//处理星期
String week(int tm_wday){
  String wk[7] = {"日","一","二","三","四","五","六"};
  String s = "周" + wk[tm_wday];
  return s;
}
//处理月日
String monthDay(int tm_mon, int tm_mday){
  String s = "";
  s = s + (tm_mon + 1);
  s = s + "月" + tm_mday + "日";
  return s;
}
//处理时分秒
void doHourMinuteSecond(int hour,int minute,int second,int *hourH,int *hourL,int *minuteH,int *minuteL,int *secondH,int *secondL){
  *hourH = hour / 10;
  *hourL = hour % 10;
  *minuteH = minute / 10;
  *minuteL = minute % 10;
  *secondH = second / 10;
  *secondL = second % 10;
}


////////////////////////////////////////////////////////////////////////
/////////////// 待办事项页面绘制区域 ////////////////////////////////////
// 说明：待办的文字点阵由 APP 渲染好后推送（见 todo.cpp），
//       这里只负责把点阵贴到屏上 + 画勾选框和中划线。
//       绘制代码放在 task.cpp 而不是 todo.cpp，是为了让点阵字库数组
//       只被这一个编译单元引用 —— 否则每多一个 .cpp 引用就会多占一份
//       Flash（city_wea_24 一份就 690KB）。
////////////////////////////////////////////////////////////////////////

// ---------- 主题相关配色 ----------
// 选中行的底色：黑主题用稍亮的深灰（保证白字依然清晰），白主题用浅灰
// （新设计改成"左侧竖杠 + 整行右移"表示选中，已不再铺底色，这里留给别处备用）
static uint16_t selBgColor() { return backColor == BACK_BLACK ? 0x2124 : 0xCE79; }
// 已完成条目的文字色：设计稿要求灰色（#666 那一档），两套主题分别微调保证对比度
static uint16_t doneColor()  { return backColor == BACK_BLACK ? 0x7BEF : 0x6B4D; }
static const uint16_t TICK_BG = 0x0E27;

// ---------- 4bpp 点阵贴图 ----------
static uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {
  int32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * (255 - a)) / 255;
  int32_t g = (((fg >>  5) & 0x3F) * a + ((bg >>  5) & 0x3F) * (255 - a)) / 255;
  int32_t b = (( fg        & 0x1F) * a + ((bg        & 0x1F) * (255 - a))) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void blitTodoBmp(int16_t x, int16_t y, const uint8_t *data, uint16_t fg, uint16_t bg) {
  uint16_t pal[16];
  for (int i = 0; i < 16; i++) pal[i] = blend565(fg, bg, (uint8_t)(i * 17));
  pal[0] = bg;
  for (int row = 0; row < TODO_BMP_H; row++) {
    const uint8_t *p = data + row * TODO_BMP_ROW;
    int start = 0, prev = -1;
    for (int i = 0; i < TODO_BMP_W; i++) {
      int v = (i & 1) ? (p[i >> 1] & 0x0F) : (p[i >> 1] >> 4);
      if (v != prev) {
        if (prev > 0) clk.drawFastHLine(x + start, y + row, i - start, pal[prev]);
        prev = v; start = i;
      }
    }
    if (prev > 0) clk.drawFastHLine(x + start, y + row, TODO_BMP_W - start, pal[prev]);
  }
}

// 点阵里文字的左右边界（中划线只划文字那一段）
static void todoInkExtent(const uint8_t *data, int16_t *x0, int16_t *x1) {
  int lo = TODO_BMP_W, hi = -1;
  for (int row = 0; row < TODO_BMP_H; row++) {
    const uint8_t *p = data + row * TODO_BMP_ROW;
    for (int i = 0; i < TODO_BMP_W; i++) {
      int v = (i & 1) ? (p[i >> 1] & 0x0F) : (p[i >> 1] >> 4);
      if (v > 40) { if (i < lo) lo = i; if (i > hi) hi = i; }
    }
  }
  if (hi < 0) { *x0 = 0; *x1 = 0; } else { *x0 = lo; *x1 = hi; }
}

// 强调色（设计稿里的那道青色）：
// 深色主题用亮青 #00FFFF（和设计稿一致），
// 浅色主题用深青 #008594 —— 亮青在白底上几乎看不见，必须压暗。
static uint16_t accentColor() {
  return backColor == BACK_BLACK ? 0x07FF : 0x0432;
}

// ---------- 绘制 ----------
// xoff：选中行动效的当前位移（0..TODO_SEL_SHIFT）
static void todoDrawRow(uint8_t pos, uint8_t r, int8_t xoff) {
  uint8_t  slot = todoOrder[pos];
  TodoMeta &m   = todoMeta[slot];
  int16_t  y    = (int16_t)r * TODO_ROWH;
  bool     sel  = (todoCursor >= 0 && pos == (uint8_t)todoCursor);
  uint16_t acc  = accentColor();
  uint16_t rbg  = backFillColor;      // 不再铺选中底色：改用"竖杠 + 整行右移"表达选中

  // 选中：左侧一道青色竖杠（和整行一起右移，形成"被推到旁边"的小动效）
  if (sel) {
    clk.fillRoundRect(2 + xoff, y + 5, TODO_SEL_BAR_W, TODO_ROWH - 10, 1, acc);
  }

  uint16_t fg = m.done ? doneColor() : penColor;

  // 复选框：空心圆 = 未完成，实心圆 = 已完成（换成设计稿的圆形，不再用方块）
  int16_t cx = 15 + xoff, cy = y + TODO_ROWH / 2;
  if (m.done) {
    clk.fillCircle(cx, cy, 7, acc);
  } else {
    clk.drawCircle(cx, cy, 7, acc);
    clk.drawCircle(cx, cy, 6, acc);   // 画两圈凑出 2px 的边框
  }

  blitTodoBmp(TODO_TEXT_X + xoff, y, todoBmpRow, fg, rbg);      // 用当前行的点阵缓冲

  if (m.done) {
    int16_t x0, x1;
    todoInkExtent(todoBmpRow, &x0, &x1);
    if (x1 > x0) clk.drawFastHLine(TODO_TEXT_X + xoff + x0, y + TODO_BMP_H / 2, x1 - x0 + 1, fg);
  }
}

// ---------- 选中行动效 ----------
// 设计稿的交互：切换选中项时，被选中那一行会往右挪一点点，
// 左侧同时出现一道青色竖杠。这里做成非阻塞的小状态机，
// 由主循环的 todoAnimTick() 逐步推进，每步只重画那一行，开销很小。
static int8_t  selAnimRow     = -1;   // 正在右移的显示行号（-1 = 没有动效）
static int8_t  selAnimOff     = 0;    // 当前位移
static int32_t selAnimNext    = 0;    // 下一步的时间点
static int8_t  selAnimLastCur = -2;   // 上一次的光标，用来检测"光标变了"

// 单独重画某一行（动效推进用）
static void todoRedrawRow(uint8_t r) {
  clk.setColorDepth(8);
  if (clk.createSprite(240, TODO_ROWH) == nullptr) return;   // 内存不足就跳过，绝不画空画布
  clk.fillSprite(backFillColor);
  uint8_t pos = todoTop + r;
  if (pos < todoCount) {
    todoSlotBmpLoad(todoOrder[pos]);
    int8_t off = 0;
    if (pos == (uint8_t)todoCursor) {
      off = (selAnimRow == (int8_t)r) ? selAnimOff : TODO_SEL_SHIFT;
    }
    todoDrawRow(pos, 0, off);
  }
  clk.pushSprite(0, TODO_LIST_Y + r * TODO_ROWH);
  clk.deleteSprite();
}

void todoAnimTick() {
  if (selAnimRow < 0) return;
  if ((int32_t)(millis() - selAnimNext) < 0) return;
  uint8_t r = (uint8_t)selAnimRow;
  selAnimOff++;
  if (selAnimOff >= TODO_SEL_SHIFT) {
    selAnimOff = TODO_SEL_SHIFT;
    selAnimRow = -1;                        // 已经到位，动效结束
  } else {
    selAnimNext = millis() + TODO_ANIM_MS;  // 还有下一步
  }
  todoRedrawRow(r);
}

void todoDrawList() {
  clk.setColorDepth(8);
  // 重要：这里原来一次开 240x208 的画布（约50KB，是全程序最大的一笔内存），
  // 内存紧张时 createSprite 会失败，而失败后继续往空画布画就会崩溃重启。
  // 改成只开「一行」高的画布逐行推送：240x26 约 6.2KB，峰值内存降到 1/8。
  if (clk.createSprite(240, TODO_ROWH) == nullptr) {
    return;   // 内存不足就跳过这次重绘，绝不往空画布画
  }

  // 光标变了 -> 给新的选中行启动"右移"动效（第一帧先画在位移 0，然后逐帧滑到 4）
  if (todoCursor != selAnimLastCur) {
    selAnimLastCur = todoCursor;
    if (todoCursor >= 0 && todoCursor >= (int8_t)todoTop &&
        todoCursor < (int8_t)(todoTop + TODO_ROWS)) {
      selAnimRow  = (int8_t)(todoCursor - (int8_t)todoTop);
      selAnimOff  = 0;
      selAnimNext = millis() + TODO_ANIM_MS;
    } else {
      selAnimRow = -1;
    }
  }

  if (todoCount == 0) {
    clk.fillSprite(backFillColor);
    clk.loadFont(clock_other_title_35);
    clk.setTextDatum(CC_DATUM);
    clk.setTextColor(doneColor());
    clk.drawString("暂无待办", 120, TODO_ROWH / 2);
    clk.unloadFont();
    for (uint8_t r = 0; r < TODO_ROWS; r++) {
      clk.pushSprite(0, TODO_LIST_Y + r * TODO_ROWH);
    }
    clk.deleteSprite();
    return;
  }

  for (uint8_t r = 0; r < TODO_ROWS; r++) {
    clk.fillSprite(backFillColor);
    uint8_t pos = todoTop + r;
    if (pos < todoCount) {
      todoSlotBmpLoad(todoOrder[pos]);   // 逐行从 SPIFFS 读点阵（省 52KB 内存）
      int8_t off = 0;
      if (pos == (uint8_t)todoCursor) {
        off = (selAnimRow == (int8_t)r) ? selAnimOff : TODO_SEL_SHIFT;
      }
      todoDrawRow(pos, 0, off);          // 在单行画布里从 y=0 开始画
    }
    clk.pushSprite(0, TODO_LIST_Y + r * TODO_ROWH);
  }
  clk.deleteSprite();

  // 右侧滚动指示条直接画在屏幕上（单行画布里做裁剪不划算）
  if (todoCount > TODO_ROWS) {
    int16_t barH = (int16_t)(TODO_ROWH * TODO_ROWS) * TODO_ROWS / todoCount;
    if (barH < 12) barH = 12;
    int16_t barY = (int16_t)(TODO_ROWH * TODO_ROWS - barH) * todoTop / (todoCount - TODO_ROWS);
    tft.fillRoundRect(237, TODO_LIST_Y + barY, 3, barH, 1, accentColor());
  }
}

void todoDrawFooter() {
  clk.setColorDepth(8);
  if (clk.createSprite(240, 20) == nullptr) return;   // 内存不足就跳过，绝不画空画布
  clk.fillSprite(backFillColor);
  // ★ 底部那条 2px 青线画在页脚画布的最上面两行（对应屏幕 y=300..301）。
  //   千万不要画在 y=298：列表最后一行的画布是 26px 高、从 y=274 推上去的，
  //   正好覆盖到 y=299 —— 那样只要重画一次列表（移动光标、选中行动效推进），
  //   这条线就会被擦掉，表现为"青线闪一下然后消失"。
  clk.fillRect(0, 0, 240, 2, accentColor());
  clk.loadFont(clock_future_weather_20);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(accentColor());
  // 设计稿：底部只居中显示设备 IP（原来左边还有"完成数/总数"，按新设计去掉了）
  clk.drawString(WiFi.localIP().toString(), 120, 11);
  clk.unloadFont();
  clk.pushSprite(0, 300);
  clk.deleteSprite();
}

void todoDrawPage() {
  reflashTFT();

  // 标题 + 一道 2px 青色横线（设计稿）
  clk.setColorDepth(8);
  if (clk.createSprite(240, 56) != nullptr) {
    clk.fillSprite(backFillColor);
    clk.loadFont(clock_other_title_35);
    clk.setTextDatum(CC_DATUM);
    clk.setTextColor(penColor);
    clk.drawString("待办事项", 120, 28);
    clk.unloadFont();
    clk.fillRect(0, 52, 240, 2, accentColor());
    clk.pushSprite(0, 0);
    clk.deleteSprite();
  }

  // 日期 + 星期：居中、青色（设计稿里日期就是青色）
  clk.setColorDepth(8);
  if (clk.createSprite(240, 30) != nullptr) {
    clk.fillSprite(backFillColor);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      clk.loadFont(city_wea_24);
      clk.setTextDatum(CC_DATUM);
      clk.setTextColor(accentColor());
      clk.drawString(monthDay(timeinfo.tm_mon, timeinfo.tm_mday) + " " + week(timeinfo.tm_wday), 120, 15);
      clk.unloadFont();
    }
    clk.pushSprite(0, 58);
    clk.deleteSprite();
  }

  todoDrawList();

  // 底部：青线 + IP 都在页脚画布里画（见 todoDrawFooter 的说明），
  // 这样列表无论重画多少次都不会把这根线擦掉。
  todoDrawFooter();
}
