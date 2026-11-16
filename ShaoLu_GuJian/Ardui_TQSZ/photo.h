#ifndef __PHOTO_H
#define __PHOTO_H

#include <Arduino.h>

// ==================== 相册（多张照片） ====================
// 照片以 JPEG 存在 SPIFFS 里，命名 /p0.jpg ~ /p{PHOTO_MAX-1}.jpg
// 显示时用 TJpg_Decoder 流式解码，逐块推给屏幕，峰值内存只要几 KB。
//
// 容量：SPIFFS 分区 896KB，扣掉待办文件 52.5KB 和文件系统开销，
//       约 800KB 可用。240x320 的照片一般 10~25KB，所以：
//       8KB/张 → 约 100 张 ；15KB/张 → 约 53 张 ；25KB/张 → 约 32 张
//       这里上限取 30，留足余量，实际能存多少由剩余空间决定。
//
// 上传协议（电脑端 app 使用，分块避免一次性占大内存）：
//   POST /api/photo?idx=2&seq=0&last=0  body=base64(第一块)   ← seq=0 截断该槽旧文件
//   POST /api/photo?idx=2&seq=1&last=0  body=base64(第二块)   ← 追加
//   POST /api/photo?idx=2&seq=N&last=1  body=base64(最后一块) ← 收完并刷新屏幕
//   POST /api/photo?seq=0               ← 不带 idx 时自动用第一个空槽
//   GET  /api/photo?idx=2               取某张的 JPEG（不带 idx 取当前这张）
//   GET  /api/photo/info                张数、当前是第几张、占用空间
//   DELETE /api/photo?idx=2             删某张（不带 idx 删当前张；?all=1 全删）

#define PHOTO_MAX     30         // 相册最多 30 张
#define PHOTO_CHUNK   2048       // 每块解码后的字节数（base64 后约 2732 字符）

// ---------- 初始化与索引 ----------
void     photoInit();                            // 启动时初始化解码器回调
void     photoRescan();                          // 扫描 SPIFFS，重建照片列表
uint8_t  photoCount();                           // 当前有几张
uint8_t  photoPos();                             // 当前是列表里的第几张（0 基）
uint8_t  photoCurrentSlot();                     // 当前张对应的槽位号
int8_t   photoFirstFreeSlot();                   // 第一个空闲槽位，-1 表示已满
uint8_t  photoWriteSlot();                       // 正在接收的槽位
uint8_t  photoSlotAt(uint8_t pos);               // 列表第 pos 个对应的槽位号

// ---------- 单张操作（slot 为 0..PHOTO_MAX-1） ----------
bool     photoExistsAt(uint8_t slot);
uint32_t photoSizeAt(uint8_t slot);
bool     photoBeginWriteAt(uint8_t slot);        // 开始接收（截断该槽）
bool     photoAppend(const uint8_t *d, size_t n);// 追加一块
void     photoEndWrite();                        // 收完收尾（刷新列表和屏幕）
bool     photoRemoveAt(uint8_t slot);            // 删除某张

// ---------- 切换与绘制 ----------
bool     photoSelectSlot(uint8_t slot);          // 切到指定槽位
bool     photoNext();                            // 下一张（循环）
bool     photoPrev();                            // 上一张（循环）
void     photoDrawPage();                        // 绘制相册页

// ---------- 空间信息 ----------
uint32_t photoUsedBytes();                       // SPIFFS 已用字节
uint32_t photoTotalBytes();                      // SPIFFS 总容量

#endif
