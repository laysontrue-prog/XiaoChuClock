# 小楚时钟 · XiaoChu Clock

一块放在桌面上的 WiFi 天气时钟，同时是一块能用**手势**摆放照片的电子相框。

- 主控：ESP32-C3（4MB Flash）
- 屏幕：ST7789 240×320 竖屏（SPI）
- 功能：时钟 / 天气（和风天气）/ 今日待办 / 相册 / 手机配网 / 主题切换
- 电脑端 APP：用摄像头识别手势，捏合「抓住」照片，拖到屏幕边缘松手，照片经 WiFi 传进设备

## 仓库内容

| 目录 / 文件 | 说明 |
|---|---|
| [ShaoLu_GuJian/](ShaoLu_GuJian/) | 固件源码（Arduino）+ 依赖库 + 编译烧录说明 |
| [XChu外壳模型/](XChu外壳模型/) | 3D 打印外壳 STL 文件（原版 + 后盖修改版） |
| [XChu时钟物料清单、接线图.pdf](XChu时钟物料清单、接线图.pdf) | 硬件物料清单与接线图 |
| **Releases** | 电脑端成品 APP（macOS / Windows），见下方 |

## 下载成品 APP

成品程序在仓库右侧 **[Releases](https://github.com/laysontrue-prog/XiaoChuClock/releases)** 页面下载（源码仓库里不放二进制）：

| 平台 | 文件 | 说明 |
|---|---|---|
| macOS（Apple Silicon） | `小楚时钟-macOS-v1.0.0.zip` | 解压后把 .app 拖进「应用程序」；首次打开需右键 → 打开 |
| Windows 10/11（64 位） | `小楚时钟-Windows-v1.0.0.exe` | 需要 WebView2 运行时（Win11 自带） |

## 自己编译固件

见 [ShaoLu_GuJian/说明.md](ShaoLu_GuJian/说明.md)：

1. 把 `Ardui_TQSZ/secrets.h.example` 复制为 `secrets.h`，填写自己的和风天气 API Key
2. Arduino IDE 打开 `Ardui_TQSZ/Ardui_TQSZ.ino`
3. **分区方案必须选 Huge APP**，否则 3MB 固件编译不过

依赖库已放在 `ShaoLu_GuJian/libraries/`，离线可用，无需另外安装。

## 开源协议

- **固件与代码**：[MIT License](LICENSE)
- **3D 外壳模型与文档**：[CC BY-NC 4.0](LICENSE-CC-BY-NC-4.0.txt)（署名-非商业使用）
- `ShaoLu_GuJian/libraries/` 中的第三方库（ArduinoJson、TFT_eSPI、TaskScheduler、OneButton、ArduinoZlib）版权归原作者所有，保留各自许可协议，见各库目录内的 LICENSE 文件。

## 作者

抖音：小楚君cc
