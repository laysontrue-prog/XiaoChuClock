#include <Arduino.h>
#include <SPIFFS.h>
#include <FS.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include "photo.h"
#include "tftUtil.h"
#include "common.h"
#include "task.h"          // 需要 extern enum CurrentPage currentPage

// ==================== 多图相册实现 ====================
// 关键点：照片不驻留内存，存在 SPIFFS；显示时 TJpg_Decoder 逐块解码直接推屏，
// 峰值 RAM 只有几 KB（这就是不做 TF 卡也够用的原因）。

static bool     photoWriting = false;
static uint8_t  sWriteSlot   = 0;

static uint8_t  sList[PHOTO_MAX];   // 存在的照片槽位（升序）
static uint8_t  sCount = 0;         // 实际张数
static uint8_t  sPos   = 0;         // 当前显示的是 sList 里的第几个

// TJpg_Decoder 的解码输出回调：每解出一块就推给屏幕，不在内存里留整张图
static bool photoOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= (int16_t)tft.height()) return 0;   // 超出屏幕底部就别画了
  tft.pushImage(x, y, w, h, bitmap);
  return 1;
}

void photoInit() {
  TJpgDec.setJpgScale(1);
  // 注意：tftInit() 里已经调了 tft.setSwapBytes(true)，pushImage 时会交换一次。
  // TJpg_Decoder 的官方示例中，回调方式只用 TJpgDec.setSwapBytes(true)（此时不设 tft 的），
  // drawFsJpg 方式只用 tft.setSwapBytes(true)。两边都设 = 交换两次 = 颜色错乱。
  // 这里必须为 false，让交换只发生在 TFT_eSPI 内部这一处。
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(photoOutput);
  photoRescan();
}

// 拼出某槽位的文件名
static void photoPath(uint8_t slot, char *buf, size_t n) {
  snprintf(buf, n, "/p%u.jpg", (unsigned)slot);
}

// ---------- 索引管理 ----------
void photoRescan() {
  if (!SPIFFS.begin(true)) { sCount = 0; return; }

  // 老版本用的是单张 /photo.jpg，这里自动迁移成第 1 张，避免用户照片丢失
  if (SPIFFS.exists("/photo.jpg")) {
    if (!SPIFFS.exists("/p0.jpg")) {
      SPIFFS.rename("/photo.jpg", "/p0.jpg");
      Serial.println("[PHOTO] 已把单张模式的老照片迁移为第 1 张");
    } else {
      SPIFFS.remove("/photo.jpg");
    }
  }

  sCount = 0;
  char path[24];
  for (uint8_t i = 0; i < PHOTO_MAX; i++) {
    photoPath(i, path, sizeof(path));
    if (SPIFFS.exists(path)) {
      File f = SPIFFS.open(path, FILE_READ);
      if (f && f.size() > 0) sList[sCount++] = i;
      if (f) f.close();
    }
  }
  if (sPos >= sCount) sPos = 0;
  Serial.printf("[PHOTO] 扫描到 %u 张照片\n", (unsigned)sCount);
}

uint8_t photoCount()       { return sCount; }
uint8_t photoPos()         { return sPos; }
uint8_t photoCurrentSlot() { return sCount ? sList[sPos] : 0; }

uint8_t photoWriteSlot()   { return sWriteSlot; }
uint8_t photoSlotAt(uint8_t pos) { return pos < sCount ? sList[pos] : 255; }

int8_t photoFirstFreeSlot() {
  if (!SPIFFS.begin(true)) return -1;
  char path[24];
  for (uint8_t i = 0; i < PHOTO_MAX; i++) {
    photoPath(i, path, sizeof(path));
    if (!SPIFFS.exists(path)) return (int8_t)i;
  }
  return -1;
}

// ---------- 单张操作 ----------
bool photoExistsAt(uint8_t slot) {
  if (slot >= PHOTO_MAX) return false;
  if (!SPIFFS.begin(true)) return false;
  char path[24]; photoPath(slot, path, sizeof(path));
  if (!SPIFFS.exists(path)) return false;
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return false;
  bool ok = (f.size() > 0);
  f.close();
  return ok;
}

uint32_t photoSizeAt(uint8_t slot) {
  if (slot >= PHOTO_MAX) return 0;
  if (!SPIFFS.begin(true)) return 0;
  char path[24]; photoPath(slot, path, sizeof(path));
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return 0;
  uint32_t n = f.size();
  f.close();
  return n;
}

bool photoBeginWriteAt(uint8_t slot) {
  if (slot >= PHOTO_MAX) return false;
  if (!SPIFFS.begin(true)) return false;
  char path[24]; photoPath(slot, path, sizeof(path));
  File f = SPIFFS.open(path, FILE_WRITE);   // FILE_WRITE 会截断旧内容
  if (!f) { Serial.println("[PHOTO] 创建文件失败"); return false; }
  f.close();
  photoWriting = true;
  sWriteSlot   = slot;
  Serial.printf("[PHOTO] 开始接收图片 -> 第 %u 张\n", (unsigned)slot + 1);
  return true;
}

bool photoAppend(const uint8_t *d, size_t n) {
  if (!photoWriting) {
    // 容错：没收到 seq=0 就直接追加，则先建文件
    int8_t slot = photoFirstFreeSlot();
    if (slot < 0 || !photoBeginWriteAt((uint8_t)slot)) return false;
  }
  char path[24]; photoPath(sWriteSlot, path, sizeof(path));
  File f = SPIFFS.open(path, FILE_APPEND);
  if (!f) { Serial.println("[PHOTO] 追加失败"); return false; }
  size_t w = f.write(d, n);
  f.close();
  return (w == n);
}

void photoEndWrite() {
  photoWriting = false;
  photoRescan();
  // 收完自动切到刚传的这张
  for (uint8_t i = 0; i < sCount; i++) {
    if (sList[i] == sWriteSlot) { sPos = i; break; }
  }
  Serial.printf("[PHOTO] 接收完成, 第 %u 张共 %u 字节\n",
                (unsigned)sWriteSlot + 1, (unsigned)photoSizeAt(sWriteSlot));

  // ===== 导入照片 → 优先级最高，直接把屏幕切到相册页 =====
  // 1) 必须先关掉天气页的动画和轮播：tAnim 每 30ms 就往 (170,240) 画一帧太空人，
  //    tScrollText 每 5s 画一次轮播文字。不关的话刚切过去的相册页会立刻被它们覆盖。
  // 2) 然后切页并重绘。按键逻辑完全不变：之后双击仍按原菜单顺序往下走
  //    （相册 → 主题 → 还原设置 → 天气），回到天气页时动画会自动重新开启。
  disableAnimScrollText();
  currentPage = PHOTO;
  photoDrawPage();
}

bool photoRemoveAt(uint8_t slot) {
  if (slot >= PHOTO_MAX) return false;
  if (!SPIFFS.begin(true)) return false;
  char path[24]; photoPath(slot, path, sizeof(path));
  bool ok = SPIFFS.remove(path);
  if (ok) Serial.printf("[PHOTO] 已删除第 %u 张\n", (unsigned)slot + 1);
  else    Serial.printf("[PHOTO] 删除第 %u 张失败\n", (unsigned)slot + 1);
  photoRescan();
  if (currentPage == PHOTO) photoDrawPage();
  return ok;
}

// ---------- 切换与绘制 ----------
bool photoSelectSlot(uint8_t slot) {
  for (uint8_t i = 0; i < sCount; i++) {
    if (sList[i] == slot) {
      sPos = i;
      Serial.printf("[PHOTO] 切到第 %u/%u 张\n", (unsigned)sPos + 1, (unsigned)sCount);
      if (currentPage == PHOTO) photoDrawPage();
      return true;
    }
  }
  return false;
}

bool photoNext() {
  if (sCount == 0) return false;
  sPos = (uint8_t)((sPos + 1) % sCount);
  Serial.printf("[PHOTO] 切到第 %u/%u 张\n", (unsigned)sPos + 1, (unsigned)sCount);
  if (currentPage == PHOTO) photoDrawPage();
  return true;
}

bool photoPrev() {
  if (sCount == 0) return false;
  sPos = (uint8_t)((sPos + sCount - 1) % sCount);
  Serial.printf("[PHOTO] 切到第 %u/%u 张\n", (unsigned)sPos + 1, (unsigned)sCount);
  if (currentPage == PHOTO) photoDrawPage();
  return true;
}

// ---------- 空间信息 ----------
uint32_t photoUsedBytes() {
  if (!SPIFFS.begin(true)) return 0;
  return SPIFFS.usedBytes();
}

uint32_t photoTotalBytes() {
  if (!SPIFFS.begin(true)) return 0;
  return SPIFFS.totalBytes();
}

// ---------- 绘制相册页 ----------
void photoDrawPage() {
  if (sCount == 0) {
    // 没有照片：显示占位提示。
    // 注意这里直接用屏幕绘制，不开画布 —— 240x240 的 8bpp 画布要 57.6KB，
    // 可用堆不足时 createSprite 会失败，整页就成了空白。
    reflashTFT();
    tft.loadFont(gFontTips);
    tft.setTextDatum(CC_DATUM);
    tft.setTextColor(penColor);
    tft.drawString(F("暂无照片"), 120, 140);
    tft.drawString(F("用电脑端相册程序上传"), 120, 180);
    tft.unloadFont();
    return;
  }

  char path[24];
  photoPath(sList[sPos], path, sizeof(path));

  // 先清屏，再按图片实际尺寸居中、必要时自动缩小
  reflashTFT();

  uint16_t w = 0, h = 0;
  if (TJpgDec.getFsJpgSize(&w, &h, path, SPIFFS) != JDR_OK || w == 0 || h == 0) {
    Serial.printf("[PHOTO] 读取 %s 尺寸失败（文件可能损坏）\n", path);
    return;
  }

  uint8_t scale = 1;
  while ((w / scale) > 240 || (h / scale) > 320) scale *= 2;   // 1/2/4/8
  int16_t dw = w / scale, dh = h / scale;
  int16_t x = (240 - dw) / 2, y = (320 - dh) / 2;
  if (x < 0) x = 0;
  if (y < 0) y = 0;

  Serial.printf("[PHOTO] 显示第 %u/%u 张 %s: %ux%u (缩放1/%d) -> %dx%d @(%d,%d)\n",
                (unsigned)sPos + 1, (unsigned)sCount, path, w, h, scale, dw, dh, x, y);
  TJpgDec.setJpgScale(scale);

  uint32_t t0 = millis();
  JRESULT r = TJpgDec.drawFsJpg(x, y, path, SPIFFS);
  Serial.printf("[PHOTO] 解码%s, 耗时 %lu ms\n", (r == JDR_OK ? "成功" : "失败"), millis() - t0);

  // 右下角显示 "第几张/共几张"：用反色（字=背景色、底=前景色）形成小色块，
  // 这样无论照片什么颜色都能看清，也不会挡住画面主体。
  if (sCount > 1) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u/%u", (unsigned)sPos + 1, (unsigned)sCount);
    tft.loadFont(clock_future_weather_20);
    tft.setTextDatum(BR_DATUM);
    tft.setTextColor(backFillColor, penColor);
    tft.drawString(buf, 236, 314);
    tft.unloadFont();
  }
}
