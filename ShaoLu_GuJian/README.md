# 小楚时钟 · XiaoChu Clock

一块放在桌面上的 WiFi 天气时钟，同时是一块能用**手势**摆放照片的电子相框。

- 主控：ESP32-C3（4MB Flash）
- 屏幕：ST7789 240×320 竖屏（SPI）
- 功能：时钟 / 天气（和风天气）/ 今日待办 / 相册 / 手机配网 / 主题切换
- 电脑端 APP：用摄像头识别手势，捏合「抓住」照片，拖到屏幕边缘松手，照片经 WiFi 传进设备

## 下载成品

| 平台 | 文件 | 说明 |
|---|---|---|
| macOS（Apple Silicon）| `XiaoChuClock-macOS-v1.0.0.zip` | 解压后把 .app 拖进「应用程序」；**首次右键 → 打开** |
| Windows 10/11（64 位）| `XiaoChuClock-Windows-v1.0.0.exe` | 需要 WebView2 运行时（Win11 自带）|

> 成品在仓库右侧 **Releases** 页面下载（源码仓库里不放二进制）。
> 国内下载慢可在链接前加 `https://ghproxy.com/`

## 自己编译

固件（Arduino IDE）：见 [说明.md](说明.md) —— **注意分区方案必须选 Huge APP**，否则 3MB 固件编译不过。

需要填自己的和风天气 Key：把 `Ardui_TQSZ/secrets.h.example` 复制成 `secrets.h`，按文件内注释填写。

## 目录结构

```
Ardui_TQSZ/      固件源码（.ino / .cpp / .h）
libraries/       依赖库（离线可用，无需另外安装）
说明.md          编译、烧录、配网、填 Key 的完整说明
烧录排错记录.md   踩过的坑与解决办法
```

## 作者

抖音：小楚君cc
