/*
 * Web Keyboard for ESP32 (keyboard_web_for_esp32) v3.5
 *    ESP32-S2/S3 模拟 USB HID 键盘 + 网页远程控制(WiFiManager 配网版)
 * ============================================================
 * 用途:PS5 意外断电重启后弹出「未正确关闭 / 正在修复存储」确认框,
 *       远程发一个回车(实测普通 USB 键盘回车 = 确认 OK,且键盘免 Sony 认证)。
 *       打开网页即显示完整虚拟键盘,按下/松开/长按/组合,和普通键盘手感一致。
 *
 * 硬件:任意带原生 USB 口的 ESP32-S2 / ESP32-S3 开发板(¥15~25),
 *       USB 线插到 PS5 任意 USB 口。PS5 开机 → USB 口供电 → ESP32 跟着开机。
 *       注意:普通 ESP32(不带 S/S3 后缀)没有原生 USB,模拟不了键盘。
 *
 * 烧录(Arduino IDE):
 *   1. 库管理器(工具 → 管理库)搜 "WiFiManager",安装 作者 tzapu 的那个
 *   2. 开发板选 "ESP32S2 Dev Module" 或 "ESP32S3 Dev Module"
 *   3. 工具 → USB Mode → 选 "USB-OTG (TinyUSB)"   ← 必须,否则 PS5 识别不到键盘
 *   4. 工具 → USB CDC On Boot → Disabled
 *   5. 烧录,无需改任何代码
 *
 * 首次配网:
 *   上电后 ESP32 发出热点 "PS5Key-Setup",手机/电脑连上它会自动弹出配置页
 *   (没弹就手动打开 http://192.168.4.1),选你家 WiFi 输密码 → 保存,自动记住,
 *   以后上电直连。只支持 2.4GHz。
 *
 * 换 WiFi / 配错了:
 *   方式一:按住板上 BOOT 键(GPIO0)再上电/复位 → 清除已存 WiFi,重新进入配网
 *   方式二:浏览器打开 http://ps5key.local/resetwifi
 *
 * 使用(与 PS5 同一局域网):
 *   浏览器打开 http://ps5key.local/          → 完整虚拟键盘页面
 *       按下即发送 key-down,松开发送 key-up;长按由 PS5 端自动重复(同真键盘);
 *       可同时按住多个键(组合键);Shift/Caps 是页面上的切换开关。
 *   curl http://ps5key.local/enter           → 按一次回车
 *   curl "http://ps5key.local/enter?n=3&gap=800" → 连按 3 次
 *   curl "http://ps5key.local/type?t=hello"  → 整串打字
 *   curl "http://ps5key.local/tap?d=97"      → 点按单个键(ASCII 码或 HID 键码)
 *   curl "http://ps5key.local/down?d=97"     → 按住不松(配合 /up 组合键)
 *   curl "http://ps5key.local/up?d=97"       → 松开
 *       常用键码:回车176 Esc177 退格178 Tab179 Caps193
 *               ←216 →215 ↑218 ↓217 空格32 LCtrl128 LShift129 LAlt130
 *
 * POST 接口(v3.5 新增,全部端点 GET/POST 通用,已开 CORS):
 *   curl -X POST http://ps5key.local/down -d "d=113"        (表单)
 *   curl -X POST http://ps5key.local/key -H "Content-Type: application/json" \
 *        -d '{"d":176,"a":"tap"}'                            (JSON)
 *       /key 参数: d=键码(0-255,同上;也接受 "k")  a=动作 down|up|tap(默认 tap)
 *       返回: 200 + 当前按键计数
 *   短按 BOOT 键(GPIO0)                      → 本地手动按一次回车
 *
 * 备注:
 *   - 键盘发不了 PS 键,唤醒仍靠已实测可用的 rtl8761b 蓝牙寻呼(ps5-wake-rtl.sh)。
 *   - 弹窗确认后若停在「选择用户」界面,再按一次回车试试。
 */

#include <WebServer.h>
#include <ESPmDNS.h>

#include <WiFiManager.h>   // tzapu/WiFiManager,库管理器直接装
#include "USB.h"
#include "USBHIDKeyboard.h"

// ================== 配置 ==================
const char* HOSTNAME = "ps5key";            // 访问名 http://ps5key.local
const char* AP_NAME  = "PS5Key-Setup";      // 首次配网热点名
// 自动模式:上电 N 秒后自动按一次回车(应对断电恢复后的确认框)。
// 0 = 关闭(默认,防止误触其它对话框)。要开建议 180(秒)左右。
const uint32_t AUTO_PRESS_AFTER_SEC = 0;
// ==========================================

USBHIDKeyboard Keyboard;
WebServer  server(80);
WiFiManager wm;

uint32_t bootMs    = 0;
bool     wifiOK    = false;
bool     autoDone  = (AUTO_PRESS_AFTER_SEC == 0);
uint32_t pressCount = 0;

#include "page.h"   // 页面放独立头文件,避开 .ino 预处理器的 #line 注入 bug

void pressEnterOnce() {
  // 长按 80ms 的回车(比瞬时 write 更稳,PS5 UI 采样慢也不丢)
  Keyboard.press(KEY_RETURN);
  delay(80);
  Keyboard.releaseAll();
  pressCount++;
}

// ---------------- HTTP 处理器 ----------------

// 解析 d 参数为合法键码;失败返回 -1
long parseKeyArg() {
  if (!server.hasArg("d")) return -1;
  long d = server.arg("d").toInt();
  if (d < 0 || d > 255) return -1;
  return d;
}

// 按住不松(虚拟键盘按下时调用;配合 /up 实现长按与组合键)
void handleDown() {
  long d = parseKeyArg();
  if (d < 0) { server.send(400, "text/plain; charset=utf-8", "usage: /down?d=<keycode>"); return; }
  Keyboard.press((uint8_t)d);          // 重复触发同一键无副作用(库内部去重)
  pressCount++;
  server.send(200, "text/plain; charset=utf-8", String(pressCount));
}

// 松开
void handleUp() {
  long d = parseKeyArg();
  if (d < 0) { server.send(400, "text/plain; charset=utf-8", "usage: /up?d=<keycode>"); return; }
  Keyboard.release((uint8_t)d);
  server.send(200, "text/plain; charset=utf-8", String(pressCount));
}

// 点按一下(press + 45ms + release),命令行/兼容用
void handleTap() {
  long d = parseKeyArg();
  if (d < 0) { server.send(400, "text/plain; charset=utf-8", "usage: /tap?d=<keycode>"); return; }
  Keyboard.press((uint8_t)d);
  delay(45);
  Keyboard.releaseAll();
  pressCount++;
  server.send(200, "text/plain; charset=utf-8", String(pressCount));
}

// 按键计数(页面轮询用)
void handleCount() {
  server.send(200, "text/plain; charset=utf-8", String(pressCount));
}

// ---- 极简 JSON 解析(免 ArduinoJson 依赖) ----
// 取 "key":数字
long jsonNum(const String& s, const char* key) {
  String pat = String("\"") + key + "\"";
  int p = s.indexOf(pat);
  if (p < 0) return -1;
  p += pat.length();
  while (p < (int)s.length() && !isDigit(s[p])) p++;
  return p < (int)s.length() ? strtol(s.c_str() + p, nullptr, 10) : -1;
}
// 取 "key":"字符串"
String jsonStr(const String& s, const char* key) {
  String pat = String("\"") + key + "\"";
  int p = s.indexOf(pat);
  if (p < 0) return "";
  p = s.indexOf('"', p + pat.length());
  if (p < 0) return "";
  int q = s.indexOf('"', p + 1);
  if (q < 0) return "";
  return s.substring(p + 1, q);
}

// 统一按键接口:支持 JSON POST /key,也支持 GET /key?d=113&a=down
//   {"d":176,"a":"tap"}   a: down(按住) / up(松开) / tap(点按,默认)
void handleKey() {
  String body = server.hasArg("plain") ? server.arg("plain") : String();
  long d = -1;
  String a = "";
  if (body.length() > 0) {           // JSON 体
    d = jsonNum(body, "d");
    if (d < 0) d = jsonNum(body, "k");
    a = jsonStr(body, "a");
  } else if (server.hasArg("d")) {   // GET 查询参数
    d = server.arg("d").toInt();
    a = server.arg("a");
  }
  if (d < 0 || d > 255) {
    server.send(400, "text/plain; charset=utf-8",
                "usage: POST /key {\"d\":113,\"a\":\"down|up|tap\"} 或 /key?d=113&a=down");
    return;
  }
  if (a == "down") { Keyboard.press((uint8_t)d); pressCount++; }
  else if (a == "up") { Keyboard.release((uint8_t)d); }
  else { Keyboard.press((uint8_t)d); delay(45); Keyboard.releaseAll(); pressCount++; }  // tap
  server.send(200, "text/plain; charset=utf-8", String(pressCount));
}

void handleType() {
  // 整串打字
  if (!server.hasArg("t")) {
    server.send(400, "text/plain; charset=utf-8", "usage: /type?t=hello");
    return;
  }
  String t = server.arg("t");
  if (t.length() > 60) t = t.substring(0, 60);
  for (unsigned i = 0; i < t.length(); i++) {
    Keyboard.press(t[i]); delay(40); Keyboard.releaseAll(); delay(40);
    pressCount++;
  }
  server.send(200, "text/plain; charset=utf-8", String(pressCount));
}

void handleEnter() {
  int n   = server.hasArg("n")   ? server.arg("n").toInt()   : 1;
  int gap = server.hasArg("gap") ? server.arg("gap").toInt() : 600;
  if (n < 1) n = 1;
  if (n > 10) n = 10;
  if (gap < 100) gap = 100;
  if (gap > 5000) gap = 5000;

  for (int i = 0; i < n; i++) {
    pressEnterOnce();
    if (i < n - 1) delay(gap);
  }
  server.send(200, "text/plain; charset=utf-8", String(pressCount));
}

void handleRoot() {
  String ip   = wifiOK ? WiFi.localIP().toString() : String("(未联网)");
  String ssid = wifiOK ? WiFi.SSID() : String("-");
  String page = FPSTR(PAGE);
  page.replace("{{SSID}}", ssid);
  page.replace("{{IP}}", ip);
  page.replace("{{CNT}}", String(pressCount));
  server.sendHeader("Cache-Control", "no-store, must-revalidate");  // 禁浏览器缓存,保证每次拿到最新固件的页面
  server.send(200, "text/html; charset=utf-8", page);
}

void setup() {
  bootMs = millis();

  // USB HID 键盘上线(顺序:先 begin HID,再 USB.begin)
  Keyboard.begin();
  USB.begin();

  // BOOT 键(GPIO0):按住上电 = 清除 WiFi 配置
  pinMode(0, INPUT_PULLUP);
  bool heldAtBoot = (digitalRead(0) == LOW);

  // WiFiManager 自动配网:已存过配置则直连;没有/连不上则开 AP 等手机来配
  wm.setConfigPortalTimeout(180);              // 配网热点 3 分钟没人来就重启再试
  wm.setAPCallback([](WiFiManager*) {});       // 预留:可在这里加指示灯提示
  if (heldAtBoot) wm.resetSettings();          // 按住 BOOT 上电 → 忘掉旧配置

  wifiOK = wm.autoConnect(AP_NAME);            // 阻塞直到连上或超时
  if (wifiOK) {
    WiFi.setHostname(HOSTNAME);
    MDNS.begin(HOSTNAME);
  }

  server.on("/", handleRoot);

  // CORS:所有响应带允许跨域头;OPTIONS 预检一律 200
  server.enableCORS(true);                       // WebServer 自带,给所有路由加 Access-Control-Allow-Origin: *
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {       // 预检请求直接放行
      server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
      server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
      server.send(200, "text/plain", "");
    } else {
      server.send(404, "text/plain; charset=utf-8", "not found");
    }
  });

  // 所有端点 GET/POST 通用
  server.on("/enter", HTTP_GET, handleEnter);
  server.on("/enter", HTTP_POST, handleEnter);
  server.on("/type",  HTTP_GET, handleType);
  server.on("/type",  HTTP_POST, handleType);
  server.on("/tap",   HTTP_GET, handleTap);
  server.on("/tap",   HTTP_POST, handleTap);
  server.on("/down",  HTTP_GET, handleDown);
  server.on("/down",  HTTP_POST, handleDown);
  server.on("/up",    HTTP_GET, handleUp);
  server.on("/up",    HTTP_POST, handleUp);
  server.on("/key",   HTTP_GET, handleKey);      // /key?d=113&a=down
  server.on("/key",   HTTP_POST, handleKey);     // POST JSON {"d":113,"a":"tap"} 或表单 d=113&a=tap
  server.on("/cnt",   HTTP_GET, handleCount);
  server.on("/cnt",   HTTP_POST, handleCount);
  server.on("/resetwifi", []() {
    server.send(200, "text/plain; charset=utf-8",
                "已清除 WiFi 配置,5 秒后重启进入配网模式(热点 " + String(AP_NAME) + ")");
    delay(500);
    wm.resetSettings();
    delay(4500);
    ESP.restart();
  });
  server.on("/reboot", []() {
    server.send(200, "text/plain", "rebooting...");
    delay(300);
    ESP.restart();
  });
  server.begin();
}

void loop() {
  server.handleClient();

  // 自动模式:上电 N 秒后按一次
  if (!autoDone && millis() - bootMs > AUTO_PRESS_AFTER_SEC * 1000UL) {
    autoDone = true;
    pressEnterOnce();
  }

  // BOOT 键(GPIO0)短按 → 触发一次(简单去抖)
  static bool lastHigh = true;
  bool now = digitalRead(0);
  if (lastHigh && !now) {
    delay(30);                       // 去抖
    if (digitalRead(0) == LOW) pressEnterOnce();
  }
  lastHigh = now;
  delay(10);
}
