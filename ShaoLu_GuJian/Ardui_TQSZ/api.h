#ifndef __API_H
#define __API_H

#include "common.h"

// 给手机 APP 用的 HTTP 接口（全部带 CORS 头，允许本地文件打开的网页跨域访问）
//   GET  /api/status          设备状态（APP 的"点击连接"打这个）
//   GET  /api/tasks           待办元数据（不含点阵）
//   POST /api/tasks           整表覆盖：按槽位写入 id/text/done/hash，返回需要上传点阵的槽
//   POST /api/bmp?s=<slot>    上传某个槽的 4bpp 点阵（body 为 base64，2600 字节 -> 3468 字符）
//   GET  /api/scan            扫描 WiFi 列表（APP 内配网用）
// 配网提交复用原有的 POST /configwifi，固件无需新增代码。

void apiBegin();     // 注册路由，需在 server.begin() 之前调用

#endif
