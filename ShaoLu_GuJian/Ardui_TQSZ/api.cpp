#include <Arduino.h>          // 必须最先包含：common.h 里的图片头用到 uint16_t
#include <FS.h>               // 必须在 TFT_eSPI 之前：FS_NO_GLOBALS 会屏蔽全局 File/FS
#include <SPIFFS.h>
#include <WebServer.h>
#include "api.h"
#include "todo.h"
#include "photo.h"
#include "net.h"
#include "task.h"
#include "app_html.h"         // 由 app/pack_app.py 生成（gzip 后的 APP 页面）
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WebServer.h>

// ===================== 手机 APP 用的 HTTP 接口 =====================
extern WebServer server;

#define API_NAME  "XiaoChu Clock"
#define API_FW    "1.0"

// ---------- 工具 ----------
static void apiCors() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Cache-Control", "no-store");
}

static void apiSend(int code, const char *ctype, const String &body) {
  apiCors();
  server.send(code, ctype, body);
}

// JSON 字符串转义（任务文本里可能有引号/反斜杠）
static String jsonEsc(const char *s) {
  String o;
  o.reserve(strlen(s) + 8);
  for (const char *p = s; *p; p++) {
    char c = *p;
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if ((uint8_t)c < 0x20) { char b[8]; sprintf(b, "\\u%04x", c); o += b; }
    else o += c;
  }
  return o;
}

static int8_t b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static int b64decode(const char *in, uint8_t *out, int maxOut) {
  int o = 0; uint32_t buf = 0; int bits = 0;
  for (const char *p = in; *p; p++) {
    int8_t v = b64val(*p);
    if (v < 0) continue;                       // 跳过 '=' 和空白
    buf = (buf << 6) | (uint8_t)v; bits += 6;
    if (bits >= 8) { bits -= 8; if (o < maxOut) out[o++] = (buf >> bits) & 0xFF; }
  }
  return o;
}

// ---------- GET /api/status ----------
static void handleApiStatus() {
  String j = "{\"ok\":1,\"name\":\"" API_NAME "\",\"fw\":\"" API_FW "\"";
  j += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += ",\"rev\":"  + String(todoRev);
  j += ",\"count\":" + String(todoCount);
  j += ",\"done\":"  + String(todoDoneCount());
  j += ",\"rssi\":"  + String(WiFi.RSSI());
  j += ",\"heap\":"  + String(ESP.getFreeHeap());
  j += ",\"max\":"   + String(TODO_MAX);
  j += "}";
  apiSend(200, "application/json", j);
}

// ---------- GET /api/tasks ----------
static void handleApiTasksGet() {
  String j = "{\"ok\":1,\"rev\":" + String(todoRev) + ",\"items\":[";
  bool first = true;
  for (uint8_t s = 0; s < TODO_MAX; s++) {
    if (!todoMeta[s].id[0]) continue;
    if (!first) j += ",";
    first = false;
    j += "{\"s\":" + String(s);
    j += ",\"i\":\"" + jsonEsc(todoMeta[s].id) + "\"";
    j += ",\"t\":\"" + jsonEsc(todoMeta[s].text) + "\"";
    j += ",\"d\":" + String(todoMeta[s].done ? 1 : 0);
    j += ",\"h\":" + String(todoMeta[s].hash);
    j += "}";
  }
  j += "]}";
  apiSend(200, "application/json", j);
}

// ---------- POST /api/tasks ----------
// body: {"rev":12,"items":[{"s":0,"i":"a1","t":"买牛奶","d":0,"h":123456}]}
//   rev 可选：若比设备当前 rev 旧，说明 APP 数据过期，返回 409 让 APP 先拉取合并
static void handleApiTasksPost() {
  String body = server.arg("plain");
  DynamicJsonDocument doc(6144);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    apiSend(400, "application/json", String("{\"ok\":0,\"err\":\"json\",\"msg\":\"") + err.c_str() + "\"}");
    return;
  }
  if (doc.containsKey("rev") && (uint16_t)(doc["rev"] | 0) < todoRev) {
    apiSend(409, "application/json",
            "{\"ok\":0,\"err\":\"conflict\",\"rev\":" + String(todoRev) + "}");
    return;
  }
  JsonArray items = doc["items"].as<JsonArray>();
  if (items.isNull()) {
    apiSend(400, "application/json", "{\"ok\":0,\"err\":\"noitems\"}");
    return;
  }

  bool     used[TODO_MAX];
  uint32_t oldHash[TODO_MAX];
  memset(used, 0, sizeof(used));
  for (uint8_t s = 0; s < TODO_MAX; s++) oldHash[s] = todoMeta[s].hash;

  String need;
  uint8_t n = 0;
  for (JsonObject o : items) {
    int s = o["s"] | -1;
    if (s < 0 || s >= TODO_MAX || used[s]) continue;
    used[s] = true;
    n++;

    const char *nid  = o["i"] | "";
    const char *ntxt = o["t"] | "";
    uint8_t     nd   = (o["d"] | 0) ? 1 : 0;
    uint32_t    nh   = o["h"].as<uint32_t>();

    bool idChanged = (strcmp(todoMeta[s].id, nid) != 0);
    if (idChanged) {
      todoSlotBmpClear(s);                     // 换内容了，旧点阵作废
    }
    strlcpy(todoMeta[s].id,   nid,  TODO_IDLEN);
    strlcpy(todoMeta[s].text, ntxt, TODO_TXTLEN);
    todoMeta[s].done = nd;
    todoMeta[s].hash = nh;

    if (idChanged || nh != oldHash[s]) {       // 需要 APP 重新上传点阵（返回槽号）
      if (need.length()) need += ",";
      need += String(s);
    }
  }
  // 清掉没被占用的槽
  for (uint8_t s = 0; s < TODO_MAX; s++) {
    if (!used[s] && todoMeta[s].id[0]) {
      memset(&todoMeta[s], 0, sizeof(TodoMeta));
      todoSlotBmpClear(s);
    }
  }
  todoRebuildOrder();
  if (todoCursor < 0 && todoCount > 0) todoCursor = 0;
  todoRev++;
  todoSave();

  String j = "{\"ok\":1,\"rev\":" + String(todoRev) + ",\"count\":" + String(n) + ",\"need\":[" + need + "]}";
  apiSend(200, "application/json", j);
}

// ---------- POST /api/bmp?s=<slot> ----------
static void handleApiBmpPost() {
  int s = server.hasArg("s") ? server.arg("s").toInt() : -1;
  if (s < 0 || s >= TODO_MAX || !todoMeta[s].id[0]) {
    apiSend(400, "application/json", "{\"ok\":0,\"err\":\"badslot\"}");
    return;
  }
  String body = server.arg("plain");
  static uint8_t tmp[TODO_BMP_SZ];
  int got = b64decode(body.c_str(), tmp, TODO_BMP_SZ);
  if (got != TODO_BMP_SZ) {
    apiSend(400, "application/json",
            "{\"ok\":0,\"err\":\"badbmp\",\"got\":" + String(got) + ",\"want\":" + String(TODO_BMP_SZ) + "}");
    return;
  }
  todoSlotBmpStore(s, tmp);             // 直接写入 SPIFFS 里该槽的点阵区
  todoSaveSoon();                       // 合并落盘，避免每张点阵都写一次 SPIFFS
  if (currentPage == TODO) todoDrawList();
  apiSend(200, "application/json", "{\"ok\":1,\"slot\":" + String(s) + "}");
}

// ---------- POST /api/photo?idx=N&seq=N&last=0|1 ----------
// 分块接收 JPEG（body 为 base64）。分块是为了避免一次性把几十 KB 塞进内存。
// idx 不填时：第一块自动用第一个空槽位。
static void handleApiPhotoPost() {
  int  seq  = server.hasArg("seq")  ? server.arg("seq").toInt() : 0;
  bool last = server.hasArg("last") && server.arg("last") == "1";
  String body = server.arg("plain");

  // 决定写到哪个槽位
  int slot = -1;
  if (server.hasArg("idx"))      slot = server.arg("idx").toInt();
  else if (seq == 0)             slot = photoFirstFreeSlot();   // 新的一张
  else                           slot = photoWriteSlot();       // 续传同一张
  if (slot < 0 || slot >= PHOTO_MAX) {
    apiSend(507, "application/json", "{\"ok\":0,\"err\":\"full\"}");
    return;
  }

  static uint8_t tmp[PHOTO_CHUNK];
  int got = b64decode(body.c_str(), tmp, sizeof(tmp));
  if (got <= 0) {
    apiSend(400, "application/json", "{\"ok\":0,\"err\":\"empty\"}");
    return;
  }

  if (seq == 0 && !photoBeginWriteAt((uint8_t)slot)) {   // 第一块：截断该槽旧文件
    apiSend(500, "application/json", "{\"ok\":0,\"err\":\"openfail\"}");
    return;
  }
  if (!photoAppend(tmp, (size_t)got)) {
    apiSend(500, "application/json", "{\"ok\":0,\"err\":\"writefail\"}");
    return;
  }

  uint32_t sz = photoSizeAt((uint8_t)slot);
  if (last) photoEndWrite();     // 内部会重扫列表、自动切到这张、若在相册页则刷新
  apiSend(200, "application/json",
          "{\"ok\":1,\"slot\":" + String(slot) + ",\"seq\":" + String(seq) +
          ",\"got\":" + String(got) + ",\"size\":" + String(sz) + "}");
}

// ---------- GET /api/photo?idx=N ----------
// 把设备里的照片原样吐出来（不填 idx 就是当前这张），方便电脑端预览
static void handleApiPhotoGet() {
  int slot = server.hasArg("idx") ? server.arg("idx").toInt() : photoCurrentSlot();
  if (slot < 0 || slot >= PHOTO_MAX || !photoExistsAt((uint8_t)slot)) {
    apiSend(404, "application/json", "{\"ok\":0,\"err\":\"nophoto\"}");
    return;
  }
  char path[24];
  snprintf(path, sizeof(path), "/p%d.jpg", slot);
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) {
    apiSend(500, "application/json", "{\"ok\":0,\"err\":\"openfail\"}");
    return;
  }
  apiCors();
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(f, "image/jpeg");
  f.close();
}

// ---------- GET /api/photo/info ----------
static void handleApiPhotoInfo() {
  uint8_t n = photoCount();
  String j = "{\"ok\":1";
  j += ",\"count\":"   + String(n);
  j += ",\"pos\":"     + String(n ? photoPos() + 1 : 0);       // 1 基，给界面显示用
  j += ",\"current\":" + String(photoCurrentSlot());          // 当前槽位号
  j += ",\"size\":"    + String(n ? photoSizeAt(photoCurrentSlot()) : 0);
  j += ",\"used\":"    + String(photoUsedBytes());
  j += ",\"total\":"   + String(photoTotalBytes());
  j += ",\"max\":"     + String(PHOTO_MAX);
  j += ",\"chunk\":"   + String(PHOTO_CHUNK);
  j += ",\"slots\":[";                                        // 各槽位号，电脑端用来列缩略图
  for (uint8_t i = 0; i < n; i++) {
    if (i) j += ",";
    j += String(photoSlotAt(i));
  }
  j += "]}";
  apiSend(200, "application/json", j);
}

// ---------- DELETE /api/photo?idx=N | ?all=1 ----------
static void handleApiPhotoDelete() {
  if (server.hasArg("all") && server.arg("all") == "1") {
    for (uint8_t i = 0; i < PHOTO_MAX; i++) {
      char path[24];
      snprintf(path, sizeof(path), "/p%u.jpg", (unsigned)i);
      if (SPIFFS.exists(path)) SPIFFS.remove(path);
    }
    photoRescan();
    Serial.println("[PHOTO] 已清空相册");
  } else {
    int slot = server.hasArg("idx") ? server.arg("idx").toInt() : photoCurrentSlot();
    photoRemoveAt((uint8_t)slot);
  }
  if (currentPage == PHOTO) photoDrawPage();
  apiSend(200, "application/json", "{\"ok\":1}");
}

// ---------- POST /api/photo/switch?to=next|prev|N ----------
// 电脑端 / 手势控制切换当前显示的照片
static void handleApiPhotoSwitch() {
  String to = server.hasArg("to") ? server.arg("to") : "next";
  bool changed = false;
  if (to == "next")      changed = photoNext();
  else if (to == "prev") changed = photoPrev();
  else                   changed = photoSelectSlot((uint8_t)to.toInt());
  String j = "{\"ok\":" + String(changed ? 1 : 0);
  j += ",\"count\":" + String(photoCount());
  j += ",\"pos\":"   + String(photoCount() ? photoPos() + 1 : 0);
  j += ",\"current\":" + String(photoCurrentSlot()) + "}";
  apiSend(changed ? 200 : 400, "application/json", j);
}

// ---------- GET /api/scan ----------
static void handleApiScan() {
  int n = WiFi.scanNetworks();
  String j = "{\"ok\":1,\"aps\":[";
  for (int i = 0; i < n; i++) {
    if (i) j += ",";
    j += "{\"s\":\"" + jsonEsc(WiFi.SSID(i).c_str()) + "\"";
    j += ",\"r\":" + String(WiFi.RSSI(i));
    j += ",\"e\":" + String((int)WiFi.encryptionType(i));
    j += "}";
  }
  j += "]}";
  WiFi.scanDelete();
  apiSend(200, "application/json", j);
}

// ---------- GET /app ----------
// 手机 APP 本体（gzip 存在 PROGMEM 里，浏览器自动解压）
static void handleApp() {
  apiCors();
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html", (PGM_P)APP_HTML_GZ, APP_HTML_GZ_LEN);
}

// ---------- OPTIONS 预检 ----------
static void handleApiOptions() {
  apiCors();
  server.send(204, "text/plain", "");
}

void apiBegin() {
  server.on("/app",        HTTP_GET,  handleApp);
  server.on("/api/status", HTTP_GET,  handleApiStatus);
  server.on("/api/tasks",  HTTP_GET,  handleApiTasksGet);
  server.on("/api/tasks",  HTTP_POST, handleApiTasksPost);
  server.on("/api/bmp",    HTTP_POST, handleApiBmpPost);
  server.on("/api/scan",   HTTP_GET,  handleApiScan);
  server.on("/api/photo",      HTTP_GET,    handleApiPhotoGet);
  server.on("/api/photo",      HTTP_POST,   handleApiPhotoPost);
  server.on("/api/photo/info", HTTP_GET,    handleApiPhotoInfo);
  server.on("/api/photo/switch", HTTP_POST,  handleApiPhotoSwitch);
  server.on("/api/photo",      HTTP_DELETE, handleApiPhotoDelete);
  server.on("/api/photo",      HTTP_OPTIONS, handleApiOptions);
  server.on("/api/photo/info", HTTP_OPTIONS, handleApiOptions);
  server.on("/api/photo/switch", HTTP_OPTIONS, handleApiOptions);
  server.on("/api/status", HTTP_OPTIONS, handleApiOptions);
  server.on("/api/tasks",  HTTP_OPTIONS, handleApiOptions);
  server.on("/api/bmp",    HTTP_OPTIONS, handleApiOptions);
  server.on("/api/scan",   HTTP_OPTIONS, handleApiOptions);
  Serial.println("[API] 接口已注册");
}
