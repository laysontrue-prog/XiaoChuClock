#include <TFT_eSPI.h>
#include "common.h"
#include "PreferencesUtil.h"

TFT_eSPI tft = TFT_eSPI(); 
TFT_eSprite clk = TFT_eSprite(&tft);
int backColor;
uint16_t backFillColor;
uint16_t penColor;

// clock_tips_28 是 font/*.h 里定义的常量数组。C++ 里 const 数组是内部链接，
// 每个引用它的 .cpp 都会各自存一份副本（这个字体 61KB）。
// 所以只让本文件持有它，其它文件统一用这个指针，避免重复占用 Flash。
const uint8_t *gFontTips = clock_tips_28;

// 初始化tft
void tftInit(){
  tft.init();
  tft.setSwapBytes(true);
  getBackColor();
  if(backColor == BACK_BLACK){
    backFillColor = 0x0000;
    penColor = 0xFFFF;
  }else{
    backFillColor = 0xFFFF;
    penColor = 0x0000;
  }
  tft.fillScreen(backFillColor);
  // 【关键修复】关掉画布的自动换行。
  // TFT_eSprite::width() 在 _vpDatum 被设置过之后会返回"视口宽度 _xWidth"而不是画布宽度，
  // 而 Smooth_font 的 drawString 用 width() 判断是否换行 —— 于是标题这类文字会被
  // 错误地折到第二行，第二行又被 60 像素高的画布下边缘裁掉，表现为
  // "左上角出现半个字"。这里直接禁用换行（标题本来也不需要换行）。
  clk.setTextWrap(false);
}

// 按背景颜色刷新整个屏幕
void reflashTFT(){
  tft.fillScreen(backFillColor);
}

// 在屏幕中间显示文字
void drawText(String text){
  // 直接画到屏幕：原来开 240x240 画布要 57.6KB，内存紧张时会失败（画面全空）。
  reflashTFT();
  tft.loadFont(gFontTips);
  tft.setTextDatum(CC_DATUM);
  tft.setTextColor(penColor);
  tft.drawString(text, 120, 120);
  tft.unloadFont();
}

// 在屏幕中间显示两行文字
void draw2LineText(String text1, String text2){
  // ★ 只清掉自己这两行占用的区域，绝对不要清全屏！
  // 这个函数被"同步天气数据"的提示动画每秒调用一次。原来这里调的是 reflashTFT()
  // （= tft.fillScreen 清全屏），于是：首页画好之后，只要提示定时器残留的标志被主循环
  // 消费一次，整个首页就被擦掉，只重画这两行提示。
  // 之后时钟/日期/滚动文字/太空人都有定时任务会自己重画，但
  // 标题、天气内容、温湿度图标只有 drawWeatherPage() 会画 —— 没有任务重绘，
  // 就永久消失了。表现就是：上下空白、内容挤在中间、提示文字和滚动文字叠在一起，
  // 双击切页再切回来（重新调用 drawWeatherPage）才恢复。
  tft.fillRect(0, 86, 240, 68, backFillColor);
  Serial.printf("[TIPS] 提示重绘: %s\n", text1.c_str());
  tft.loadFont(gFontTips);
  tft.setTextDatum(CC_DATUM);
  tft.setTextColor(penColor);
  tft.drawString(text1, 120, 105);
  tft.drawString(text2, 120, 135);
  tft.unloadFont();
}