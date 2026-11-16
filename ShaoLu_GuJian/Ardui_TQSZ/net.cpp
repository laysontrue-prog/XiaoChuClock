#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "ArduinoZlib.h"
#include "common.h"
#include "PreferencesUtil.h"
#include "tftUtil.h"
#include "task.h"
#include "api.h"
#include "arduino.h"

void sendNTPpacket(IPAddress &address);
void startAP();
void startServer();
void scanWiFi();
void handleNotFound();
void handleRoot();
void handleConfigWifi();
void restartSystem(String msg, bool endTips);
String urlEncode(const String& text);

bool queryNowWeatherSuccess = false;
bool queryFutureWeatherSuccess = false;
bool queryAirSuccess = false;

// Wifi相关
String ssid;  //WIFI名称
String pass;  //WIFI密码
String city;  // 城市
String adm; // 上级城市区划
String location; // 城市ID
String WifiNames; // 根据搜索到的wifi生成的option字符串
// SoftAP相关
const char *APssid = "XiaoChu Clock";
IPAddress staticIP(192, 168, 1, 1);
IPAddress gateway(192, 168, 1, 254);
IPAddress subnet(255, 255, 255, 0);
WebServer server(80);
// 查询天气超时时间(ms)
int queryTimeout = 6000;
// 是否是刚启动时查询天气
bool isStartQuery = true; 
// 天气接口相关
static HTTPClient httpClient;
String data = "";
uint8_t *outbuf;

// 开启SoftAP进行配网
void wifiConfigBySoftAP(){
  // 开启AP模式，如果开启失败，重启系统
  startAP();
  // 扫描WiFi,并将扫描到的WiFi组成option选项字符串
  scanWiFi();
  // 启动服务器
  startServer();
  // 显示配置网络页面
  tft.pushImage(0, 0, 240, 320, QRcode);
}

// 处理服务器请求
void doClient(){
  server.handleClient();
}
// 处理404情况的函数'handleNotFound'
void handleNotFound(){
  handleRoot();//访问不存在目录则返回配置页面
}
// 处理网站根目录的访问请求
void handleRoot(){
  // STA 模式下 WifiNames 还没扫过，第一次访问时补扫一次，保证下拉框可用
  if (WifiNames.length() == 0) scanWiFi();
  server.send(200,"text/html", ROOT_HTML_PAGE1 + WifiNames + ROOT_HTML_PAGE2);
}
// 提交数据后的提示页面
void handleConfigWifi(){
  //判断是否有WiFi名称
  if (server.hasArg("ssid")){
    Serial.print("获得WiFi名称:");
    ssid = server.arg("ssid");
    Serial.println(ssid);
  }else{
    Serial.println("错误, 没有发现WiFi名称");
    server.send(200, "text/html", "<meta charset='UTF-8'>错误, 没有发现WiFi名称");
    return;
  }
  //判断是否有WiFi密码
  if (server.hasArg("pass")){
    Serial.print("获得WiFi密码:");
    pass = server.arg("pass");
    Serial.println(pass);
  }else{
    Serial.println("错误, 没有发现WiFi密码");
    server.send(200, "text/html", "<meta charset='UTF-8'>错误, 没有发现WiFi密码");
    return;
  }
  //判断是否有city名称
  if (server.hasArg("city")){
    Serial.print("获得城市:");
    city = server.arg("city");
    Serial.println(city);
  }else{
    Serial.println("错误, 没有发现城市名称");
    server.send(200, "text/html", "<meta charset='UTF-8'>错误, 没有发现城市名称");
    return;
  }
  Serial.print("获得上级区划:");
  adm = server.arg("adm");
  Serial.println(adm);
  // 将信息存入nvs中
  setWiFiCity();
  // 获得了所需要的一切信息，给客户端回复
  server.send(200, "text/html", "<meta charset='UTF-8'><style type='text/css'>body {font-size: 2rem;}</style><br/><br/>WiFi: " + ssid + "<br/>密码: " + pass + "<br/>城市: " + city + "<br/>上级区划: " + adm + "<br/>已取得相关信息,正在尝试连接,请手动关闭此页面。");
  restartSystem("即将尝试连接", false);
}

// 设置国家码为 CN，开放 1~13 信道。
// 默认国家码下只支持 1~11 信道，如果路由器在 12/13 信道就会连不上。
void setupWiFiCountry(){
  WiFi.mode(WIFI_STA);                       // 触发 WiFi 初始化
  esp_err_t r = esp_wifi_set_country_code("CN", true);
  Serial.printf("[WiFi] 国家码设为 CN: %s\n", r == ESP_OK ? "成功" : "失败");
}

// 记录WiFi断开原因码（用于诊断连不上的原因）
volatile int gWifiReason = 0;
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info){
  if(event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED){
    gWifiReason = info.wifi_sta_disconnected.reason;
    Serial.printf("[WiFi] 断开, 原因码=%d (%s)\n", gWifiReason, WiFi.disconnectReasonName((wifi_err_reason_t)gWifiReason));
  }
}

// 连接WiFi（多次尝试 + 全程日志，方便定位连不上的原因）
void connectWiFi(int timeOut_s){
  delay(1500); // 让“系统启动中”字样多显示一会
  drawText("正在连接网络...");

  // ===== 诊断：先确认读到的配置对不对 =====
  Serial.println("=========== WiFi 连接诊断 ===========");
  Serial.printf("目标SSID : \"%s\"\n", ssid.c_str());
  Serial.printf("密码长度 : %u\n", pass.length());
  Serial.printf("城市     : \"%s\"  上级区划: \"%s\"\n", city.c_str(), adm.c_str());

  WiFi.onEvent(onWiFiEvent);
  WiFi.setSleep(false);      // 关掉省电，连接更稳
  gWifiReason = 0;

  // 扫描一次：拿到目标AP的信道/BSSID/加密方式（定向连接比只给SSID可靠得多）
  int n = WiFi.scanNetworks();
  Serial.printf("扫描到 %d 个WiFi\n", n);
  int32_t targetCh = 0;
  uint8_t targetBssid[6] = {0};
  bool found = false;
  for(int i = 0; i < n; i++){
    if(WiFi.SSID(i) == ssid){
      targetCh = WiFi.channel(i);
      memcpy(targetBssid, WiFi.BSSID(i), 6);
      found = true;
      Serial.printf(">> 找到目标: 信道=%d 信号=%d dBm 加密=%d BSSID=%s\n",
                    targetCh, WiFi.RSSI(i), (int)WiFi.encryptionType(i), WiFi.BSSIDstr(i).c_str());
      break;
    }
  }
  if(n > 0 && !found){
    Serial.printf(">> !! 扫描列表里没有 \"%s\"（名字对不上 / 太远）\n", ssid.c_str());
  }
  WiFi.scanDelete();

  pinMode(D4, OUTPUT);
  uint32_t deadline = millis() + (uint32_t)timeOut_s * 1000;
  int attempt = 0;

  // 最多三轮：①定向(信道+BSSID) ②普通 ③清空驱动状态后重连
  while(attempt < 3 && WiFi.status() != WL_CONNECTED && millis() < deadline){
    attempt++;
    gWifiReason = 0;
    if(attempt == 1 && found){
      Serial.printf("\n[尝试%d] 用 信道%d + BSSID 定向连接...\n", attempt, targetCh);
      WiFi.begin(ssid.c_str(), pass.c_str(), targetCh, targetBssid, true);
    }else if(attempt == 2){
      Serial.printf("\n[尝试%d] 普通方式连接...\n", attempt);
      WiFi.disconnect();
      delay(300);
      WiFi.begin(ssid.c_str(), pass.c_str());
    }else{
      Serial.printf("\n[尝试%d] 清空驱动状态后重连...\n", attempt);
      WiFi.disconnect(true);
      delay(500);
      WiFi.mode(WIFI_STA);
      delay(200);
      WiFi.begin(ssid.c_str(), pass.c_str());
    }
    // 单轮最多等 12 秒
    int waited = 0;
    while(waited < 24 && WiFi.status() != WL_CONNECTED && millis() < deadline){
      Serial.print(".");
      digitalWrite(D4, !digitalRead(D4));
      delay(500);
      waited++;
    }
    Serial.printf("\n[尝试%d] 结束: 状态=%d 原因码=%d (%s)\n", attempt, WiFi.status(), gWifiReason,
                  WiFi.disconnectReasonName((wifi_err_reason_t)gWifiReason));
  }

  if(WiFi.status() == WL_CONNECTED){
    digitalWrite(D4, LOW); // 连接成功后，将D4指示灯熄灭
    Serial.println("\n>>> 网络连接成功");
    Serial.printf(">>> 本地IP=%s  网关=%s\n", WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str());
    Serial.printf(">>> 信号=%d dBm  信道=%d\n", WiFi.RSSI(), WiFi.channel());
    return;
  }

  // 失败：把原因显示在屏幕上（停20秒方便看清），再清除配置重新配网
  Serial.printf("\n>>> 网络连接失败! 最终状态=%d 原因码=%d (%s)\n", WiFi.status(), gWifiReason,
                WiFi.disconnectReasonName((wifi_err_reason_t)gWifiReason));
  char tip[64];
  snprintf(tip, sizeof(tip), "%s (%d)", WiFi.disconnectReasonName((wifi_err_reason_t)gWifiReason), gWifiReason);
  draw2LineText("网络连接失败", tip);
  delay(20000);
  clearWiFiCity();
  restartSystem("网络连接失败", false);
}
// 检查WiFi连接状态，如果断开了，重新连接
void checkWiFiStatus(){
  if(WiFi.status() != WL_CONNECTED){ // 网络断开了，进行重连
    Serial.println("网络断开，即将重新连接...");
    WiFi.begin(ssid, pass);
  }
}
// 启动服务器（AP 配网模式和 STA 正常模式共用，只会启动一次）
static bool serverStarted = false;
void startServer(){
  if (serverStarted) return;
  serverStarted = true;
  // 当浏览器请求服务器根目录(网站首页)时调用自定义函数handleRoot处理，设置主页回调函数，必须添加第二个参数HTTP_GET，否则无法强制门户
  server.on("/", HTTP_GET, handleRoot);
  // 当浏览器请求服务器/configwifi(表单字段)目录时调用自定义函数handleConfigWifi处理
  server.on("/configwifi", HTTP_POST, handleConfigWifi);
  // 手机 APP 用的 /api/* 接口
  apiBegin();
  // 当浏览器请求的网络资源无法在服务器找到时调用自定义函数handleNotFound处理   
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("服务器启动成功！");
  // 起 mDNS，手机 APP 就能用 http://xiaochu.local 找到设备（Q2-C 方案）
  if (MDNS.begin("xiaochu")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS 已启动: http://xiaochu.local");
  } else {
    Serial.println("mDNS 启动失败，APP 里请手动填 IP");
  }
}
// 开启AP模式，如果开启失败，重启系统
void startAP(){
  Serial.println("开启AP模式...");
  WiFi.enableAP(true); // 使能AP模式
  //传入参数静态IP地址,网关,掩码
  WiFi.softAPConfig(staticIP, gateway, subnet);
  if (!WiFi.softAP(APssid)) {
    Serial.println("AP模式启动失败");
    ESP.restart(); // Ap模式启动失败，重启系统
  }  
  Serial.println("AP模式启动成功");
  Serial.print("IP地址: ");
  Serial.println(WiFi.softAPIP());
}
// 扫描WiFi,并将扫描到的Wifi组成option选项字符串
void scanWiFi(){
  Serial.println("开始扫描WiFi");
  int n = WiFi.scanNetworks();
  if (n){
    Serial.print("扫描到");
    Serial.print(n);
    Serial.println("个WIFI");
    WifiNames = "";
    for (size_t i = 0; i < n; i++){
      int32_t rssi = WiFi.RSSI(i);
      String signalStrength;
      if(rssi >= -35){
        signalStrength = " (信号极强)";
      }else if(rssi >= -50){
        signalStrength = " (信号强)";
      }else if(rssi >= -70){
        signalStrength = " (信号中)";
      }else{
        signalStrength = " (信号弱)";
      }
      WifiNames += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + signalStrength + "</option>";
      // Serial.print("WiFi的名称(SSID):");
      // Serial.println(WiFi.SSID(i));
    }
  }else{
    Serial.println("没扫描到WIFI");
  }
}

// 查询城市id
void getCityID(){
  bool flag = false; // 是否成功获取到城市id的标志
  String url = cityURL + KEY + "&location=" + urlEncode(city) + "&adm=" + urlEncode(adm);
  httpClient.setConnectTimeout(queryTimeout * 5);

  // 最多重试3次：网络偶尔抖动，重试能显著提高成功率
  for(int attempt = 1; attempt <= 3 && !flag; attempt++){
    data = "";                    // 每次重试都要清空，否则数据会累加导致解析失败
    httpClient.begin(url);
    int httpCode = httpClient.GET();
    Serial.printf("[城市] 第%d次请求, HTTP=%d\n", attempt, httpCode);

    if (httpCode == HTTP_CODE_OK) {
      // 解压Gzip数据流
      int len = httpClient.getSize();
      uint8_t buff[2048] = { 0 };
      WiFiClient *stream = httpClient.getStreamPtr();
      while (httpClient.connected() && (len > 0 || len == -1)) {
        size_t size = stream->available();
        if (size) {
          size_t realsize = ((size > sizeof(buff)) ? sizeof(buff) : size);
          size_t readBytesSize = stream->readBytes(buff, realsize);
          if (len > 0) len -= readBytesSize;
          outbuf = (uint8_t *)malloc(sizeof(uint8_t) * 5120);
          uint32_t outprintsize = 0;
          int result = ArduinoZlib::libmpq__decompress_zlib(buff, readBytesSize, outbuf, 5120, outprintsize);
          for (int i = 0; i < outprintsize; i++) {
            data += (char)outbuf[i];
          }
          free(outbuf);
        }
        delay(1);
      }

      // 只解析需要的字段（code + 第一个匹配城市）。
      // 重要：城市接口的响应大小随行政区数量变化（武汉就有2817字节），
      // 不分青红皂白地整份解析会撑爆 JsonDocument 导致"假失败"。
      StaticJsonDocument<192> filter;
      filter["code"] = true;
      filter["location"][0]["name"] = true;
      filter["location"][0]["id"] = true;

      DynamicJsonDocument doc(768);   // 用堆，别占栈
      DeserializationError error = deserializeJson(doc, data, DeserializationOption::Filter(filter));
      if(!error){
        String code = doc["code"].as<const char*>();
        if(code.equals("200")){
          flag = true;
          // 多结果的情况下，取第一个
          city = doc["location"][0]["name"].as<const char*>();
          location = doc["location"][0]["id"].as<const char*>();
          Serial.println("[城市] 城市id :" + location);
          // 将信息存入nvs中
          setWiFiCity();
        }else{
          Serial.printf("[城市] 接口返回码=%s (数据%u字节)\n", code.c_str(), data.length());
        }
      }else{
        Serial.printf("[城市] JSON解析失败: %s (数据%u字节)\n", error.c_str(), data.length());
      }
    }
    httpClient.end();

    if(!flag && attempt < 3){
      Serial.println("[城市] 本次失败, 2秒后重试...");
      delay(2000);
    }
  }

  if(flag){
    Serial.println("[城市] 获取成功");
  }else{
    // 注意：这里【不】清空WiFi配置！
    // 城市接口临时抽风不该让用户重新配网，保留配置稍后自动重试即可。
    Serial.println("[城市] 获取城市ID失败，保留WiFi配置，稍后自动重试");
  }
}

// 查询实时天气
void getNowWeather(){
  data = "";
  queryNowWeatherSuccess = false; // 先置为false
  String url = nowURL + KEY + "&location=" + location;
  httpClient.setConnectTimeout(queryTimeout);
  httpClient.begin(url);
  //启动连接并发送HTTP请求
  int httpCode = httpClient.GET();
  // Serial.println(ESP.getFreeHeap());
  Serial.println("正在获取天气数据");
  //如果服务器响应OK则从服务器获取响应体信息并通过串口输出
  if (httpCode == HTTP_CODE_OK) {
    // 解压Gzip数据流
    int len = httpClient.getSize();
    uint8_t buff[2048] = { 0 };
    WiFiClient *stream = httpClient.getStreamPtr();
    while (httpClient.connected() && (len > 0 || len == -1)) {
      size_t size = stream->available();  // 还剩下多少数据没有读完？
      // Serial.println(size);
      if (size) {
        size_t realsize = ((size > sizeof(buff)) ? sizeof(buff) : size);
        // Serial.println(realsize);
        size_t readBytesSize = stream->readBytes(buff, realsize);
        // Serial.write(buff,readBytesSize);
        if (len > 0) len -= readBytesSize;
        outbuf = (uint8_t *)malloc(sizeof(uint8_t) * 5120);
        uint32_t outprintsize = 0;
        int result = ArduinoZlib::libmpq__decompress_zlib(buff, readBytesSize, outbuf, 5120, outprintsize);
        // Serial.write(outbuf, outprintsize);
        for (int i = 0; i < outprintsize; i++) {
          data += (char)outbuf[i];
        }
        free(outbuf);
        Serial.println(data);
      }
      delay(1);
    }
    // 解压完，转换json数据
    DynamicJsonDocument doc(6144); //用堆分配，避免占用栈空间；容量也放大以防响应偏大
    DeserializationError error = deserializeJson(doc, data); //反序列化JSON数据
    if(!error){ //检查反序列化是否成功
      //读取json节点
      String code = doc["code"].as<const char*>();
      if(code.equals("200")){
        queryNowWeatherSuccess = true;       
        //读取json节点
        nowWeather.text = doc["now"]["text"].as<const char*>();
        nowWeather.icon = doc["now"]["icon"].as<int>();
        nowWeather.temp = doc["now"]["temp"].as<int>();
        String feelsLike = doc["now"]["feelsLike"]; // 体感温度
        nowWeather.feelsLike = "体感温度" + feelsLike + "℃";
        String windDir = doc["now"]["windDir"];
        String windScale = doc["now"]["windScale"];
        nowWeather.win = windDir + windScale + "级";
        nowWeather.humidity = doc["now"]["humidity"].as<int>();
        String vis = doc["now"]["vis"];
        nowWeather.vis = "能见度" + vis + " KM";
        Serial.println("获取成功");
      }
    }  
  }
  if(!queryNowWeatherSuccess){
    Serial.print("请求实时天气错误：");
    Serial.println(httpCode);
  }
  //关闭与服务器连接
  httpClient.end();
}
// 查询空气质量
void getAir(){
  data = "";
  queryAirSuccess = false; // 先置为false
  String url = airURL + KEY + "&location=" + location;
  httpClient.setConnectTimeout(queryTimeout);
  httpClient.begin(url);
  //启动连接并发送HTTP请求
  int httpCode = httpClient.GET();
  Serial.println("正在获取空气质量数据");
  //如果服务器响应OK则从服务器获取响应体信息并通过串口输出
  if (httpCode == HTTP_CODE_OK) {
    // 解压Gzip数据流
    int len = httpClient.getSize();
    uint8_t buff[2048] = { 0 };
    WiFiClient *stream = httpClient.getStreamPtr();
    while (httpClient.connected() && (len > 0 || len == -1)) {
      size_t size = stream->available();  // 还剩下多少数据没有读完？
      // Serial.println(size);
      if (size) {
        size_t realsize = ((size > sizeof(buff)) ? sizeof(buff) : size);
        // Serial.println(realsize);
        size_t readBytesSize = stream->readBytes(buff, realsize);
        // Serial.write(buff,readBytesSize);
        if (len > 0) len -= readBytesSize;
        outbuf = (uint8_t *)malloc(sizeof(uint8_t) * 5120);
        uint32_t outprintsize = 0;
        int result = ArduinoZlib::libmpq__decompress_zlib(buff, readBytesSize, outbuf, 5120, outprintsize);
        // Serial.write(outbuf, outprintsize);
        for (int i = 0; i < outprintsize; i++) {
          data += (char)outbuf[i];
        }
        free(outbuf);
        Serial.println(data);
      }
      delay(1);
    }
    // 解压完，转换json数据
    DynamicJsonDocument doc(6144); //用堆分配，避免占用栈空间；容量也放大以防响应偏大
    DeserializationError error = deserializeJson(doc, data); //反序列化JSON数据
    if(!error){ //检查反序列化是否成功
      //读取json节点
      String code = doc["code"].as<const char*>();
      if(code.equals("200")){
        queryAirSuccess = true;
        //读取json节点
        nowWeather.air = doc["now"]["aqi"].as<int>();
        nowWeather.pm10 = doc["now"]["pm10"].as<const char*>();
        nowWeather.pm2p5 = doc["now"]["pm2p5"].as<const char*>();
        nowWeather.no2 = doc["now"]["no2"].as<const char*>();
        nowWeather.so2 = doc["now"]["so2"].as<const char*>();
        nowWeather.co = doc["now"]["co"].as<const char*>();
        nowWeather.o3 = doc["now"]["o3"].as<const char*>();
        Serial.println("获取成功");
      }
    }  
  } 
  if(!queryAirSuccess){
    Serial.print("请求空气质量错误：");
    Serial.println(httpCode);
  }
  //关闭与服务器连接
  httpClient.end();
}
// 查询未来天气，经过实况天气一环，城市名称肯定是合法的，所以无需再检验
void getFutureWeather(){
  data = "";
  queryFutureWeatherSuccess = false; // 先置为false
  String url = futureURL + KEY + "&location=" + location;
  httpClient.setConnectTimeout(queryTimeout);
  httpClient.begin(url);
  //启动连接并发送HTTP请求
  int httpCode = httpClient.GET();
  Serial.println("正在获取一周天气数据");
  //如果服务器响应OK则从服务器获取响应体信息并通过串口输出
  if (httpCode == HTTP_CODE_OK) {
    // 解压Gzip数据流
    int len = httpClient.getSize();
    uint8_t buff[2048] = { 0 };
    WiFiClient *stream = httpClient.getStreamPtr();
    while (httpClient.connected() && (len > 0 || len == -1)) {
      size_t size = stream->available();  // 还剩下多少数据没有读完？
      // Serial.println(size);
      if (size) {
        size_t realsize = ((size > sizeof(buff)) ? sizeof(buff) : size);
        // Serial.println(realsize);
        size_t readBytesSize = stream->readBytes(buff, realsize);
        // Serial.write(buff,readBytesSize);
        if (len > 0) len -= readBytesSize;
        outbuf = (uint8_t *)malloc(sizeof(uint8_t) * 5120);
        uint32_t outprintsize = 0;
        int result = ArduinoZlib::libmpq__decompress_zlib(buff, readBytesSize, outbuf, 5120, outprintsize);
        // Serial.write(outbuf, outprintsize);
        for (int i = 0; i < outprintsize; i++) {
          data += (char)outbuf[i];
        }
        free(outbuf);
        Serial.println(data);
      }
      delay(1);
    }
    // 解压完，转换json数据
    DynamicJsonDocument doc(6144); //用堆分配，避免占用栈空间；容量也放大以防响应偏大
    DeserializationError error = deserializeJson(doc, data); //反序列化JSON数据
    if(!error){ //检查反序列化是否成功
      //读取json节点
      String code = doc["code"].as<const char*>();
      if(code.equals("200")){
        queryFutureWeatherSuccess = true;
        //读取json节点
        futureWeather.day0wea = doc["daily"][0]["textDay"].as<const char*>();
        futureWeather.day0wea_img = doc["daily"][0]["iconDay"].as<int>();
        futureWeather.day0date = doc["daily"][0]["fxDate"].as<const char*>();
        futureWeather.day0tem_day = doc["daily"][0]["tempMax"].as<int>();
        futureWeather.day0tem_night = doc["daily"][0]["tempMin"].as<int>();

        futureWeather.day1wea = doc["daily"][1]["textDay"].as<const char*>();
        futureWeather.day1wea_img = doc["daily"][1]["iconDay"].as<int>();
        futureWeather.day1date = doc["daily"][1]["fxDate"].as<const char*>();
        futureWeather.day1tem_day = doc["daily"][1]["tempMax"].as<int>();
        futureWeather.day1tem_night = doc["daily"][1]["tempMin"].as<int>();

        futureWeather.day2wea = doc["daily"][2]["textDay"].as<const char*>();
        futureWeather.day2wea_img = doc["daily"][2]["iconDay"].as<int>();
        futureWeather.day2date = doc["daily"][2]["fxDate"].as<const char*>();
        futureWeather.day2tem_day = doc["daily"][2]["tempMax"].as<int>();
        futureWeather.day2tem_night = doc["daily"][2]["tempMin"].as<int>();

        futureWeather.day3wea = doc["daily"][3]["textDay"].as<const char*>();
        futureWeather.day3wea_img = doc["daily"][3]["iconDay"].as<int>();
        futureWeather.day3date = doc["daily"][3]["fxDate"].as<const char*>();
        futureWeather.day3tem_day = doc["daily"][3]["tempMax"].as<int>();
        futureWeather.day3tem_night = doc["daily"][3]["tempMin"].as<int>();

        futureWeather.day4wea = doc["daily"][4]["textDay"].as<const char*>();
        futureWeather.day4wea_img = doc["daily"][4]["iconDay"].as<int>();
        futureWeather.day4date = doc["daily"][4]["fxDate"].as<const char*>();
        futureWeather.day4tem_day = doc["daily"][4]["tempMax"].as<int>();
        futureWeather.day4tem_night = doc["daily"][4]["tempMin"].as<int>();

        futureWeather.day5wea = doc["daily"][5]["textDay"].as<const char*>();
        futureWeather.day5wea_img = doc["daily"][5]["iconDay"].as<int>();
        futureWeather.day5date = doc["daily"][5]["fxDate"].as<const char*>();
        futureWeather.day5tem_day = doc["daily"][5]["tempMax"].as<int>();
        futureWeather.day5tem_night = doc["daily"][5]["tempMin"].as<int>();

        futureWeather.day6wea = doc["daily"][6]["textDay"].as<const char*>();
        futureWeather.day6wea_img = doc["daily"][6]["iconDay"].as<int>();
        futureWeather.day6date = doc["daily"][6]["fxDate"].as<const char*>();
        futureWeather.day6tem_day = doc["daily"][6]["tempMax"].as<int>();
        futureWeather.day6tem_night = doc["daily"][6]["tempMin"].as<int>();

        Serial.println("获取成功");
      }
    }  
  } 
  if(!queryFutureWeatherSuccess){
    Serial.print("请求一周天气错误：");
    Serial.println(httpCode);
  }
  //关闭与服务器连接
  httpClient.end();
}

// 获取NTP并同步RTC时间
void getNTPTime(){
  // 8 * 3600 东八区时间修正
  // 使用夏令时 daylightOffset_sec 就填写3600，否则就填写0；
  Serial.println("NTP对时...");
  configTime( 8 * 3600, 0, NTP1, NTP2, NTP3);
}
// 重启系统
// bool endTips  是否需要把“同步天气数据”文字的定时器任务取消
void restartSystem(String msg, bool endTips){
  if(endTips){
    //结束循环显示提示文字的定时器（连同残留标志一起清掉）
    stopTipsTimer();
  }
  reflashTFT();
  for(int i = 3; i > 0; i--){
    String text = "";
    text = text + i + "秒后系统重启";
    draw2LineText(msg,text);
    delay(1000);
  }
  ESP.restart();
}

String urlEncode(const String& text) {
  String encodedText = "";
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encodedText += c;
    } else if (c == ' ') {
      encodedText += '+';
    } else {
      encodedText += '%';
      char hex[4];
      sprintf(hex, "%02X", (uint8_t)c);
      encodedText += hex;
    }
  }
  return encodedText;
}

