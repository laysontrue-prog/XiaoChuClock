#include <Arduino.h>          // 必须最先包含：common.h 里的图片头用到 uint16_t
#include <FS.h>               // 必须在 TFT_eSPI 之前！它定义了 FS_NO_GLOBALS，会屏蔽全局 File/FS
#include <SPIFFS.h>
#include "todo.h"
#include "tftUtil.h"
#include "task.h"
#include <WiFi.h>

// ===================== 待办事项模块实现 =====================
//  持久化文件：SPIFFS:/todo.bin
//  布局：10 字节文件头 + TODO_MAX 个槽，每槽 id+text+done+hash+bmp

TodoMeta todoMeta[TODO_MAX];
uint8_t  todoBmpRow[TODO_BMP_SZ];              // 单行点阵缓冲（2.6KB，替代原来的 52KB）

// 单槽内：元数据长度（id+text+done+hash），点阵紧跟在后面
#define TODO_BMP_OFF (TODO_IDLEN + TODO_TXTLEN + 5)
uint8_t  todoOrder[TODO_MAX];
uint8_t  todoCount  = 0;
uint16_t todoRev    = 0;
int8_t   todoCursor = -1;
uint8_t  todoTop    = 0;
static uint16_t todoDay = 0;   // 记录列表所属的"日期序号"，跨天自动清空（Q4-A）

#define TODO_FILE     "/todo.bin"
#define TODO_MAGIC    "XCTD"
#define TODO_VER      1
// 单槽字节数：id + text + done + hash + bmp
#define TODO_SLOT_SZ  (TODO_IDLEN + TODO_TXTLEN + 1 + 4 + TODO_BMP_SZ)
#define TODO_HDR_SZ   10

static uint8_t todoSlotBuf[TODO_SLOT_SZ];       // 读写单槽用的临时缓冲

// ---------- 显示顺序 ----------
void todoRebuildOrder() {
  uint8_t n = 0;
  for (uint8_t s = 0; s < TODO_MAX; s++) {
    if (todoMeta[s].id[0]) todoOrder[n++] = s;
  }
  todoCount = n;
  if (todoCursor >= (int8_t)todoCount) todoCursor = todoCount ? todoCount - 1 : -1;
  if (todoTop + TODO_ROWS > todoCount) todoTop = (todoCount > TODO_ROWS) ? todoCount - TODO_ROWS : 0;
}

uint8_t todoSlotAt(uint8_t pos) { return (pos < todoCount) ? todoOrder[pos] : 0; }

int8_t todoPosOfSlot(uint8_t slot) {
  for (uint8_t i = 0; i < todoCount; i++) if (todoOrder[i] == slot) return (int8_t)i;
  return -1;
}

uint8_t todoDoneCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < todoCount; i++) if (todoMeta[todoOrder[i]].done) n++;
  return n;
}

// 确保 /todo.bin 存在且长度足够（不足则用零重建）。点阵区一并置零。
static bool todoEnsureFile() {
  if (!SPIFFS.begin(true)) return false;
  uint32_t need = TODO_HDR_SZ + (uint32_t)TODO_MAX * TODO_SLOT_SZ;
  if (SPIFFS.exists(TODO_FILE)) {
    File f = SPIFFS.open(TODO_FILE, FILE_READ);
    if (f) { bool ok = (f.size() >= need); f.close(); if (ok) return true; }
  }
  File f = SPIFFS.open(TODO_FILE, FILE_WRITE);
  if (!f) return false;
  uint8_t hdr[TODO_HDR_SZ];
  memset(hdr, 0, sizeof(hdr));
  memcpy(hdr, TODO_MAGIC, 4);
  hdr[4] = TODO_VER;
  f.write(hdr, TODO_HDR_SZ);
  memset(todoSlotBuf, 0, TODO_SLOT_SZ);
  for (uint8_t s = 0; s < TODO_MAX; s++) f.write(todoSlotBuf, TODO_SLOT_SZ);
  f.close();
  return true;
}

// 从 SPIFFS 读某个槽的点阵到 todoBmpRow（显示/上传时按需读取，省内存）
bool todoSlotBmpLoad(uint8_t slot) {
  if (slot >= TODO_MAX) return false;
  if (!todoEnsureFile()) return false;
  File f = SPIFFS.open(TODO_FILE, FILE_READ);
  if (!f) return false;
  bool ok = f.seek(TODO_HDR_SZ + (uint32_t)slot * TODO_SLOT_SZ + TODO_BMP_OFF);
  if (ok) ok = (f.read(todoBmpRow, TODO_BMP_SZ) == TODO_BMP_SZ);
  f.close();
  if (!ok) memset(todoBmpRow, 0, TODO_BMP_SZ);
  return ok;
}

// 把一张点阵写进某个槽（APP 上传点阵时调用）
bool todoSlotBmpStore(uint8_t slot, const uint8_t *src) {
  if (slot >= TODO_MAX) return false;
  if (!todoEnsureFile()) return false;
  File f = SPIFFS.open(TODO_FILE, "r+");
  if (!f) return false;
  bool ok = f.seek(TODO_HDR_SZ + (uint32_t)slot * TODO_SLOT_SZ + TODO_BMP_OFF);
  if (ok) ok = (f.write(src, TODO_BMP_SZ) == TODO_BMP_SZ);
  f.close();
  return ok;
}

// 清空某个槽的点阵（内容变了，旧点阵作废）
void todoSlotBmpClear(uint8_t slot) {
  memset(todoBmpRow, 0, TODO_BMP_SZ);
  todoSlotBmpStore(slot, todoBmpRow);
}

// ---------- 持久化 ----------
// 只就地改写元数据（89字节/槽），点阵区原样保留，避免把 52KB 重新搬一遍
void todoSave() {
  if (!todoEnsureFile()) return;
  File f = SPIFFS.open(TODO_FILE, "r+");
  if (!f) { Serial.println("[TODO] 打开文件失败"); return; }
  uint8_t hdr[TODO_HDR_SZ];
  memcpy(hdr, TODO_MAGIC, 4);
  hdr[4] = TODO_VER;
  hdr[5] = todoCount;
  hdr[6] = todoRev & 0xFF;
  hdr[7] = (todoRev >> 8) & 0xFF;
  hdr[8] = todoDay & 0xFF;
  hdr[9] = (todoDay >> 8) & 0xFF;
  f.seek(0);
  f.write(hdr, TODO_HDR_SZ);
  for (uint8_t s = 0; s < TODO_MAX; s++) {
    uint8_t *b = todoSlotBuf;
    memset(b, 0, TODO_BMP_OFF);        // 只清元数据区
    memcpy(b, todoMeta[s].id, TODO_IDLEN);
    memcpy(b + TODO_IDLEN, todoMeta[s].text, TODO_TXTLEN);
    b[TODO_IDLEN + TODO_TXTLEN] = todoMeta[s].done;
    uint8_t *h = b + TODO_IDLEN + TODO_TXTLEN + 1;
    h[0] = todoMeta[s].hash & 0xFF; h[1] = (todoMeta[s].hash >> 8) & 0xFF;
    h[2] = (todoMeta[s].hash >> 16) & 0xFF; h[3] = (todoMeta[s].hash >> 24) & 0xFF;
    // 关键：只写前面 89 字节元数据，后面的点阵区原样保留（不再需要 52KB 内存）
    f.seek(TODO_HDR_SZ + (uint32_t)s * TODO_SLOT_SZ);
    f.write(b, TODO_BMP_OFF);
  }
  f.close();
}

static void todoCheckDay();      // 实现在下面（跨天自动清空）

// APP 会连续上传多张点阵，每张都落盘太慢，这里合并成一次延迟写入
static uint32_t todoSaveDue = 0;
static uint32_t todoDayCheckDue = 0;
void todoSaveSoon() { todoSaveDue = millis() + 3000; }
void todoTick() {
  if (todoSaveDue && (int32_t)(millis() - todoSaveDue) >= 0) {
    todoSaveDue = 0;
    todoSave();
    Serial.println("[TODO] 点阵已落盘");
  }
  // 每分钟检查一次是否跨天
  if ((int32_t)(millis() - todoDayCheckDue) >= 0) {
    todoDayCheckDue = millis() + 60000;
    todoCheckDay();
  }
}

static void todoLoad() {
  memset(todoMeta, 0, sizeof(todoMeta));
  todoCount = 0; todoRev = 0; todoCursor = -1; todoTop = 0;
  if (!SPIFFS.begin(true)) { Serial.println("[TODO] SPIFFS 挂载失败"); return; }
  if (!SPIFFS.exists(TODO_FILE)) return;
  File f = SPIFFS.open(TODO_FILE, FILE_READ);
  if (!f) return;
  uint8_t hdr[TODO_HDR_SZ];
  if (f.read(hdr, TODO_HDR_SZ) != TODO_HDR_SZ) { f.close(); return; }
  if (memcmp(hdr, TODO_MAGIC, 4) != 0 || hdr[4] != TODO_VER) { f.close(); return; }
  todoRev = (uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8);
  todoDay = (uint16_t)hdr[8] | ((uint16_t)hdr[9] << 8);
  for (uint8_t s = 0; s < TODO_MAX; s++) {
    if (f.read(todoSlotBuf, TODO_BMP_OFF) != TODO_BMP_OFF) break;   // 只读元数据
    uint8_t *b = todoSlotBuf;
    memcpy(todoMeta[s].id, b, TODO_IDLEN);                        todoMeta[s].id[TODO_IDLEN - 1] = 0;
    memcpy(todoMeta[s].text, b + TODO_IDLEN, TODO_TXTLEN);        todoMeta[s].text[TODO_TXTLEN - 1] = 0;
    todoMeta[s].done = b[TODO_IDLEN + TODO_TXTLEN] ? 1 : 0;
    const uint8_t *h = b + TODO_IDLEN + TODO_TXTLEN + 1;
    todoMeta[s].hash = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
    f.seek(f.position() + TODO_BMP_SZ);                             // 跳过点阵区
  }
  f.close();
  todoRebuildOrder();
}

void todoBegin() {
  todoLoad();
  Serial.printf("[TODO] 载入 %u 条，rev=%u\n", todoCount, todoRev);
  if (todoCount > 0) todoCursor = 0;
}

// ---------- 跨天自动清空（Q4-A） ----------
// 用"年*366 + 年内第几天"当日期序号：单调递增，跨天必变，不用管闰年
static uint16_t todoTodayIndex() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return 0;          // 时间还没同步
  return (uint16_t)((t.tm_year - 100) * 366 + t.tm_yday);
}

static void todoClearAll() {
  memset(todoMeta, 0, sizeof(todoMeta));
  todoCursor = -1; todoTop = 0;
  // 点阵存在 SPIFFS 里，删掉文件即等于把点阵全部清空（不必再占 52KB 内存）
  if (SPIFFS.begin(true)) SPIFFS.remove(TODO_FILE);
  todoRebuildOrder();
}

static void todoCheckDay() {
  uint16_t today = todoTodayIndex();
  if (today == 0) return;
  if (todoDay == 0) { todoDay = today; todoSave(); return; }   // 首次记录
  if (today == todoDay) return;
  todoDay = today;
  if (todoCount > 0) {
    todoClearAll();
    todoRev++;
    todoSave();
    Serial.println("[TODO] 跨天，已自动清空");
    if (currentPage == TODO) todoDrawPage();
  } else {
    todoSave();
  }
}

// ---------- 交互 ----------
void todoMoveCursor(int8_t d) {
  if (todoCount == 0) { todoCursor = -1; return; }
  if (todoCursor < 0) todoCursor = 0;
  else {
    int16_t c = todoCursor + d;
    if (c < 0) c = todoCount - 1;
    if (c >= todoCount) c = 0;
    todoCursor = (int8_t)c;
  }
  if (todoCursor < (int8_t)todoTop) todoTop = todoCursor;
  if (todoCursor >= (int8_t)(todoTop + TODO_ROWS)) todoTop = todoCursor - TODO_ROWS + 1;
  todoDrawList();
}

void todoToggleCursor() {
  if (todoCount == 0 || todoCursor < 0 || todoCursor >= (int8_t)todoCount) return;
  TodoMeta &m = todoMeta[todoOrder[todoCursor]];
  m.done = !m.done;
  todoRev++;
  todoDrawList();
  todoDrawFooter();
  todoSave();
  Serial.printf("[TODO] 槽 %u -> %s\n", todoOrder[todoCursor], m.done ? "已完成" : "未完成");
}
