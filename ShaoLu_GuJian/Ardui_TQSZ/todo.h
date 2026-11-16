#ifndef __TODO_H
#define __TODO_H

#include "common.h"

// ===================== 待办事项模块 =====================
// 文字点阵由手机 APP 渲染后推送到设备（设备端没有中文点阵字库），
// 设备只负责：按槽位存点阵 + 显示 + 画勾选框和中划线。
//
// 槽位模型：APP 决定每条待办放在哪个槽（0..TODO_MAX-1），设备按槽存取。
// 显示顺序 = 被占用槽号升序。这样重排/删除都不需要搬动大块点阵。

#define TODO_MAX      20                        // 最多 20 条（Q4-A）
#define TODO_IDLEN    12                        // id 最长 11 字符
#define TODO_TXTLEN   72                        // 文本最长 71 字节 UTF-8
#define TODO_BMP_W    200                       // 点阵宽（像素）
#define TODO_BMP_H    26                        // 点阵高（像素）
#define TODO_BMP_ROW  (TODO_BMP_W / 2)          // 4bpp：每行 100 字节
#define TODO_BMP_SZ   (TODO_BMP_ROW * TODO_BMP_H)  // 每张 2600 字节

// 列表区布局
#define TODO_LIST_Y   92                        // 列表起始 Y
#define TODO_ROWH     26                        // 行高
#define TODO_ROWS     8                         // 一屏 8 行
#define TODO_TEXT_X   36                        // 文字点阵起始 X

// 选中行动效：左侧竖杠 + 整行往右移一点点（设计稿的交互效果）
#define TODO_SEL_SHIFT   4                      // 选中行右移的像素数（动效终点）
#define TODO_SEL_BAR_W   3                      // 选中行左侧竖条宽度
#define TODO_ANIM_MS     26                     // 动效每步间隔（毫秒）

void todoAnimTick();                            // 主循环里调用，推进选中行动效

struct TodoMeta {
  char     id[TODO_IDLEN];
  char     text[TODO_TXTLEN];
  uint8_t  done;
  uint32_t hash;                                // APP 给的点阵指纹，变了就要重传
};

extern TodoMeta todoMeta[TODO_MAX];
// 关键：点阵不再常驻内存！
// 原来用 todoBmps[20][2600] = 52KB 静态 RAM，把本来就紧张的堆内存挤到
// loadFont() 的 malloc 失败（TFT_eSPI 不检查返回值 → 往 NULL 写 → 崩溃重启）。
// SPIFFS 里本来就存着全部 20 条的完整点阵，这里改成"用一个 2.6KB 的单行缓冲，
// 逐行从 SPIFFS 读"，直接省下 49.4KB。
extern uint8_t  todoBmpRow[TODO_BMP_SZ];        // 当前操作的那一张点阵
bool todoSlotBmpLoad(uint8_t slot);             // SPIFFS → todoBmpRow
bool todoSlotBmpStore(uint8_t slot, const uint8_t *src);  // todoBmpRow ← src 并写入 SPIFFS
void todoSlotBmpClear(uint8_t slot);            // 清空某槽点阵（内容变了作废旧点阵）
extern uint8_t  todoOrder[TODO_MAX];            // 显示顺序 -> 槽位
extern uint8_t  todoCount;                      // 有效条数
extern uint16_t todoRev;                        // 版本号，任何改动 +1
extern int8_t   todoCursor;                     // 选中的显示位置 0..todoCount-1
extern uint8_t  todoTop;                        // 列表顶部显示位置

void    todoBegin();                            // 挂载 SPIFFS 并载入
void    todoSave();                             // 立即落盘
void    todoSaveSoon();                         // 3 秒后合并落盘（APP 连续上传点阵时用）
void    todoTick();                             // 主循环里调用，处理延迟落盘
uint8_t todoDoneCount();
uint8_t todoSlotAt(uint8_t pos);                // 显示位置 -> 槽位
int8_t  todoPosOfSlot(uint8_t slot);            // 槽位 -> 显示位置
void    todoRebuildOrder();                     // 重算显示顺序

void    todoDrawPage();
void    todoDrawList();
void    todoDrawFooter();
void    todoMoveCursor(int8_t d);               // 单击
void    todoToggleCursor();                     // 长按

#endif
