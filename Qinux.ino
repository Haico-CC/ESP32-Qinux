#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WebSocketsServer.h>
#include <LittleFS.h>
#include <FS.h>
#include <time.h>
#include <sys/time.h>
#include <vector>
#include <map>
#include <utility>
#include "llm.h"

// LLM Bridge 接口声明
bool llm_bridge_init(const char*, const char*);
bool llm_bridge_generate(const char*, int, llm_token_cb, generated_complete_cb, void*);
bool llm_bridge_is_ready();
bool llm_bridge_is_busy();
void llm_bridge_free();

// ========== 配置参数 ==========
const char* AP_SSID = "Welcome Back to the 80's !";
const char* AP_PASS = "";
const byte DNS_PORT = 53;
IPAddress AP_IP(192, 168, 4, 1);
const unsigned long SERIAL_BAUD = 115200;

// ========== 分块传输配置 ==========
const size_t CHUNK_SIZE = 4096;
const size_t MAX_FILE_SIZE = 5 * 1024 * 1024;
const uint32_t CHUNK_TIMEOUT = 8000;

// ========== 全局对象 ==========
DNSServer dnsServer;
WebServer webServer(80);
WebSocketsServer webSocket(81);

// ========== 全局状态 ==========
String currentPath = "/";
String serialInputBuffer = "";
bool isEditing = false;
String editingFilePath = "";
int g_lastExitCode = 0;

// ========== LLM 全局状态 ==========
uint8_t llmActiveClient = 255;
bool llmGenerationActive = false;
const char* LLM_MODEL_PATH = "/bin/stories260K.bin";
const char* LLM_TOKEN_PATH = "/bin/tok512.bin";

// ========== 分块传输状态（修复版）==========
struct ChunkTransferState {
  bool active = false;
  bool isUpload = false;
  bool sending = false;
  String filename;
  size_t totalSize = 0;
  size_t totalChunks = 0;
  size_t currentChunk = 0;
  size_t clientChunkSize = CHUNK_SIZE;
  File file;
  uint32_t lastChunkTime = 0;
  uint8_t clientNum = 255;
  int lastPrintedProgress = -1;
};
ChunkTransferState chunkState;

// ========== 受保护目录列表 ==========
const char* PROTECTED_DIRS[] = { "/bin", "/etc", "/sys" };
const int PROTECTED_COUNT = sizeof(PROTECTED_DIRS) / sizeof(PROTECTED_DIRS[0]);

bool isProtectedPath(const String& path) {
  String resolved = resolvePath(path);
  for (int i = 0; i < PROTECTED_COUNT; i++) {
    if (resolved == PROTECTED_DIRS[i] || resolved.startsWith(String(PROTECTED_DIRS[i]) + "/")) {
      return true;
    }
  }
  return false;
}

bool canWritePath(const String& path) {
  return !isProtectedPath(path);
}

// ========== 脚本执行上下文 ==========
std::map<String, String> scriptVars;
std::vector<String> scriptInputLines;
int scriptInputIndex = 0;
bool scriptWaitingInput = false;
String scriptInputVarName = "";

// ========== Fortune彩蛋库 ==========
const char* FORTUNES[] PROGMEM = {
  "🔮 The cake is a lie.", "🔮 sudo make me a sandwich", "🔮 Everything is... 42.",
  "🔮 Have you tried turning it off and on again?", "🔮 It works on my machine.",
  "🔮 There's no place like 127.0.0.1", "🔮 To err is human, to really foul things up requires root.",
  "🔮 I'm not lazy, I'm in energy-saving mode.", "🔮 Hardware: The part of a computer that can be kicked.",
  "🔮 All your base are belong to us.", "🔮 Talk is cheap. Show me the code. - Linus Torvalds",
  "🔮 First, solve the problem. Then, write the code. - John Johnson",
  "🔮 ESP32: Because sometimes you need two cores to handle the chaos.",
  "🔮 Debugging: Being the detective in a crime movie where you are also the murderer.",
  "🔮 Coffee: The official programming language of ESP32 developers.",
  "🔮 Warning: May contain traces of undefined behavior.",
  "🔮 If at first you don't succeed; call it version 1.0.",
  "🔮 I would agree with you, but then we'd both be wrong.",
  "🔮 My code doesn't have bugs, it just develops random features.",
  "🔮 Keep calm and rm -rf /"
};
const int FORTUNE_COUNT = sizeof(FORTUNES) / sizeof(FORTUNES[0]);

// ========== Base64编解码 ==========
static const char* b64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

String base64Encode(const uint8_t* data, size_t length) {
  String ret;
  int i = 0, j = 0;
  uint8_t char_array_3[3], char_array_4[4];
  while (length--) {
    char_array_3[i++] = *(data++);
    if (i == 3) {
      char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
      char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
      char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
      char_array_4[3] = char_array_3[2] & 0x3f;
      for (i = 0; i < 4; i++) ret += b64_chars[char_array_4[i]];
      i = 0;
    }
  }
  if (i) {
    for (j = i; j < 3; j++) char_array_3[j] = '\0';
    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
    char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
    char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
    char_array_4[3] = char_array_3[2] & 0x3f;
    for (j = 0; j < i + 1; j++) ret += b64_chars[char_array_4[j]];
    while ((i++ < 3)) ret += '=';
  }
  return ret;
}

static inline bool is_base64(uint8_t c) {
  return (isalnum(c) || (c == '+') || (c == '/'));
}

std::vector<uint8_t> base64Decode(const String& encoded_string) {
  int in_len = encoded_string.length();
  int i = 0, j = 0, in_ = 0;
  uint8_t char_array_4[4], char_array_3[3];
  std::vector<uint8_t> ret;
  while (in_len-- && (encoded_string[in_] != '=') && is_base64(encoded_string[in_])) {
    char_array_4[i++] = encoded_string[in_];
    in_++;
    if (i == 4) {
      for (i = 0; i < 4; i++) char_array_4[i] = strchr(b64_chars, char_array_4[i]) - b64_chars;
      char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
      char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
      char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
      for (i = 0; (i < 3); i++) ret.push_back(char_array_3[i]);
      i = 0;
    }
  }
  if (i) {
    for (j = i; j < 4; j++) char_array_4[j] = 0;
    for (j = 0; j < 4; j++) char_array_4[j] = strchr(b64_chars, char_array_4[j]) - b64_chars;
    char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
    char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
    char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
    for (j = 0; (j < i - 1); j++) ret.push_back(char_array_3[j]);
  }
  return ret;
}

// ========== 路径处理辅助函数 ==========
String joinPath(String base, String sub) {
  if (base == "/") return "/" + sub;
  return base + "/" + sub;
}

String resolvePath(String inputPath) {
  String fullPath = inputPath.startsWith("/") ? inputPath : (currentPath == "/" ? "/" + inputPath : currentPath + "/" + inputPath);
  while (fullPath.indexOf("//") != -1) fullPath.replace("//", "/");
  std::vector<String> parts;
  int start = 0;
  for (int i = 0; i <= fullPath.length(); i++) {
    if (i == fullPath.length() || fullPath[i] == '/') {
      String part = fullPath.substring(start, i);
      if (part == "" || part == ".") {
        start = i + 1;
        continue;
      }
      if (part == "..") {
        if (!parts.empty()) parts.pop_back();
      } else {
        parts.push_back(part);
      }
      start = i + 1;
    }
  }
  String result = "/";
  for (size_t i = 0; i < parts.size(); i++) {
    if (i > 0) result += "/";
    result += parts[i];
  }
  return result;
}

// ========== JSON转义 ==========
String escapeJson(String str) {
  String result = "";
  for (size_t i = 0; i < str.length(); i++) {
    char c = str[i];
    if (c == '\\') result += "\\\\";
    else if (c == '"') result += "\\\"";
    else if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else result += c;
  }
  return result;
}

// ========== 时间处理 ==========
void setSystemTime(time_t epochSecs) {
  struct timeval tv = { epochSecs, 0 };
  settimeofday(&tv, NULL);
}

bool parseDateTimeString(String dateStr, time_t& outTime) {
  int year, month, day, hour, minute, second;
  if (sscanf(dateStr.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
    struct tm timeinfo = {};
    timeinfo.tm_year = year - 1900;
    timeinfo.tm_mon = month - 1;
    timeinfo.tm_mday = day;
    timeinfo.tm_hour = hour;
    timeinfo.tm_min = minute;
    timeinfo.tm_sec = second;
    outTime = mktime(&timeinfo);
    return true;
  }
  return false;
}

// ========== 系统信息 ==========
String getResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    case ESP_RST_POWERON: return "Power On";
    case ESP_RST_EXT: return "External Pin";
    case ESP_RST_SW: return "Software Reset";
    case ESP_RST_PANIC: return "Exception/Panic";
    case ESP_RST_INT_WDT: return "Interrupt WDT";
    case ESP_RST_TASK_WDT: return "Task WDT";
    case ESP_RST_WDT: return "Other WDT";
    case ESP_RST_DEEPSLEEP: return "Deep Sleep Wakeup";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "Unknown";
  }
}

// ========== GPIO安全校验 ==========
bool isSafeGpio(int pin) {
#if defined(CONFIG_IDF_TARGET_ESP32)
  return (pin == 2 || pin == 4 || pin == 5 || pin == 12 || pin == 13 || pin == 14 || pin == 15 || pin == 18 || pin == 19 || pin == 21 || pin == 22 || pin == 23 || pin == 25 || pin == 26 || pin == 27 || pin == 32 || pin == 33);
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  return (pin >= 1 && pin <= 10) || (pin >= 18 && pin <= 19);
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  return (pin >= 1 && pin <= 18) || (pin == 21);
#else
  return (pin == 2 || pin == 4 || pin == 5 || pin == 12 || pin == 13);
#endif
}

void initAllGpioLow() {
  for (int pin = 0; pin <= 48; pin++) {
    if (isSafeGpio(pin)) {
      pinMode(pin, OUTPUT);
      digitalWrite(pin, LOW);
    }
  }
}

String getSafeGpioList() {
#if defined(CONFIG_IDF_TARGET_ESP32)
  return "2, 4, 5, 12-15, 18-19, 21-23, 25-27, 32-33";
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  return "1-10, 18-19";
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  return "1-18, 21";
#else
  return "2, 4, 5, 12, 13";
#endif
}

// ========== ADC引脚校验 ==========
bool isAdc1Pin(int pin) {
#if defined(CONFIG_IDF_TARGET_ESP32)
  return (pin == 32 || pin == 33 || pin == 34 || pin == 35 || pin == 36 || pin == 39);
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  return (pin >= 0 && pin <= 4);
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  return (pin >= 1 && pin <= 10);
#else
  return (pin == 4);
#endif
}

String getAdc1ValidPins() {
#if defined(CONFIG_IDF_TARGET_ESP32)
  return "32, 33, 34, 35, 36, 39";
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  return "0, 1, 2, 3, 4";
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  return "1, 2, 3, 4, 5, 6, 7, 8, 9, 10";
#else
  return "4";
#endif
}

// ========== WiFi状态显示辅助 ==========
String getWiFiModeStr(wifi_mode_t mode) {
  switch (mode) {
    case WIFI_OFF: return "OFF";
    case WIFI_STA: return "STA";
    case WIFI_AP: return "AP";
    case WIFI_AP_STA: return "AP+STA";
    default: return "Unknown";
  }
}

String getAuthModeStr(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA3_PSK: return "WPA3-PSK";
    default: return "Unknown";
  }
}

// ========= ✨ WiFi功率转换辅助函数（Core 3.x修复）✨ ==========
#if ESP_ARDUINO_VERSION_MAJOR >= 3
// dBm → wifi_power_t 枚举值（支持0.5dBm步进，公式：枚举值 = dBm × 4）
wifi_power_t dbmToWifiPower(int dbm) {
  int enum_val = (int)((float)dbm * 4.0f + 0.5f);
  return static_cast<wifi_power_t>(constrain(enum_val, -4, 84));
}

// wifi_power_t 枚举值 → dBm（浮点精度）
float wifiPowerToDbm(wifi_power_t power) {
  return (float)power / 4.0f;
}

// 获取当前发射功率的dBm值（整数显示用，四舍五入）
int getWiFiTxPowerDbm() {
  wifi_power_t power = WiFi.getTxPower();
  return (int)(wifiPowerToDbm(power) + 0.5f);
}
#endif

bool setWiFiTxPower(int dbm) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  dbm = constrain(dbm, -1, 21);
  wifi_power_t power_enum = dbmToWifiPower(dbm);
  bool result = WiFi.setTxPower(power_enum);
  return result;
#else
  dbm = constrain(dbm, -20, 20);
  return WiFi.setTxPower(dbm);
#endif
}

void handleWifiCommand(String args, String& output) {
  args.trim();
  if (args.isEmpty() || args == "status") {
    output += "=== WiFi Status ===\n";
    output += "wifi: Mode:      " + getWiFiModeStr(WiFi.getMode()) + "\n";
    if (WiFi.getMode() & WIFI_AP) {
      output += "wifi: AP SSID:   " + WiFi.softAPSSID() + "\n";
      output += "wifi: AP IP:     " + WiFi.softAPIP().toString() + "\n";
      output += "wifi: Clients:   " + String(WiFi.softAPgetStationNum()) + "\n";
    }
    if (WiFi.getMode() & WIFI_STA) {
      if (WiFi.status() == WL_CONNECTED) {
        output += "wifi: STA SSID:   " + WiFi.SSID() + "\n";
        output += "wifi: STA IP:     " + WiFi.localIP().toString() + "\n";
        output += "wifi: RSSI:       " + String(WiFi.RSSI()) + " dBm\n";
      } else output += "wifi: STA:        Disconnected\n";
    }
    output += "wifi: Channel:   " + String(WiFi.channel()) + "\n";
    // ✨ 修复：Core 3.x 正确显示dBm值
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    output += "wifi: Tx Power:  " + String(getWiFiTxPowerDbm()) + " dBm\n";
#else
    output += "wifi: Tx Power:  " + String(WiFi.getTxPower()) + " dBm\n";
#endif
    output += "wifi: MAC:       " + WiFi.macAddress() + "\n";
    return;
  }
  if (args == "info") {
    output += "=== WiFi Configuration ===\n";
    output += "Mode:            " + getWiFiModeStr(WiFi.getMode()) + "\n";
    output += "SDK Version:     " + String(ESP.getSdkVersion()) + "\n";
#if ESP_ARDUINO_VERSION_MAJOR < 3
    output += "PHY Mode:        " + String(WiFi.getPhyMode()) + "\n";
    output += "Sleep Mode:      " + String(WiFi.getSleepMode()) + "\n";
    output += "Min Rate:        " + String(WiFi.getMinRate() / 2) + " Mbps\n";
    output += "Max Rate:        " + String(WiFi.getMaxRate() / 2) + " Mbps\n";
#else
    output += "PHY Mode:        N/A (Core 3.x)\n";
    output += "Sleep Mode:      " + String(WiFi.getSleep() ? "Enabled" : "Disabled") + "\n";
    output += "Min Rate:        N/A\n";
    output += "Max Rate:        N/A\n";
#endif
    output += "MAC Address:     " + WiFi.macAddress() + "\n";
    output += "Channel:         " + String(WiFi.channel()) + "\n";
    // ✨ 修复：Core 3.x 正确显示dBm值
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    output += "Tx Power:        " + String(getWiFiTxPowerDbm()) + " dBm\n";
#else
    output += "Tx Power:        " + String(WiFi.getTxPower()) + " dBm\n";
#endif
    if (WiFi.getMode() & WIFI_AP) {
      output += "\n[AP Configuration]\n";
      output += "SSID:            " + WiFi.softAPSSID() + "\n";
#if ESP_ARDUINO_VERSION_MAJOR < 3
      output += "Auth Mode:       " + getAuthModeStr(WiFi.softAPgetAuthMode()) + "\n";
#else
      output += "Auth Mode:       WPA2-PSK (default)\n";
#endif
      output += "IP Address:      " + WiFi.softAPIP().toString() + "\n";
      output += "Subnet Mask:     " + WiFi.softAPSubnetMask().toString() + "\n";
      output += "Gateway:         " + WiFi.softAPIP().toString() + "\n";
      output += "MAC:             " + WiFi.softAPmacAddress() + "\n";
      output += "Connected:       " + String(WiFi.softAPgetStationNum()) + "/4\n";
    }
    if (WiFi.getMode() & WIFI_STA) {
      output += "\n[STA Configuration]\n";
      output += "Status:          " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected") + "\n";
      if (WiFi.status() == WL_CONNECTED) {
        output += "SSID:            " + WiFi.SSID() + "\n";
        output += "BSSID:           " + WiFi.BSSIDstr() + "\n";
        output += "IP Address:      " + WiFi.localIP().toString() + "\n";
        output += "RSSI:            " + String(WiFi.RSSI()) + " dBm\n";
        output += "Channel:         " + String(WiFi.channel()) + "\n";
      }
    }
    return;
  }
  if (args == "scan") {
    output += "wifi: Scanning nearby networks...\n";
    int n = WiFi.scanNetworks();
    if (n == WIFI_SCAN_FAILED) output += "wifi: Scan failed\n";
    else if (n == 0) output += "wifi: No networks found\n";
    else {
      output += "wifi: Found " + String(n) + " network(s)\n";
      output += "wifi: RSSI | CH | AUTH     | SSID\n";
      output += "wifi: -----+----+----------+----------------\n";
      for (int i = 0; i < n && i < 25; i++) {
        String auth = getAuthModeStr(WiFi.encryptionType(i));
        char buf[80];
        snprintf(buf, sizeof(buf), "wifi: %4d | %2d | %-10s | %s\n", WiFi.RSSI(i), WiFi.channel(i), auth.c_str(), WiFi.SSID(i).c_str());
        output += buf;
        yield();
      }
      if (n > 25) output += "wifi: ... +" + String(n - 25) + " more\n";
    }
    WiFi.scanDelete();
    return;
  }
  if (args == "stats") {
    output += "=== WiFi Statistics ===\n";
    output += "RSSI:            " + String(WiFi.RSSI()) + " dBm\n";
    int rssi = WiFi.RSSI();
    String quality = (rssi >= -50) ? "Excellent" : (rssi >= -67) ? "Good"
                                                 : (rssi >= -78) ? "Fair"
                                                 : (rssi >= -90) ? "Poor"
                                                                 : "No Signal";
    output += "Signal Quality:  " + quality + "\n";
#if ESP_ARDUINO_VERSION_MAJOR < 3
    output += "Current Rate:    " + String(WiFi.getPhyMode()) + "\n";
    output += "Min Rate:        " + String(WiFi.getMinRate() / 2) + " Mbps\n";
    output += "Max Rate:        " + String(WiFi.getMaxRate() / 2) + " Mbps\n";
#else
    output += "Current Rate:    N/A (Core 3.x)\n";
    output += "Min Rate:        N/A\n";
    output += "Max Rate:        N/A\n";
#endif
    return;
  }
  if (args == "set power" || args.startsWith("set power ")) {
    String valStr = (args == "set power") ? "" : args.substring(10);
    valStr.trim();

    if (valStr.isEmpty()) {
      output = "wifi: Usage: wifi set power <dBm>\n";
      output += "wifi: Range: 2 (min) ~ 20 (max) dBm [Core 3.x]\n";
      output += "wifi: Step: 0.5 dBm granularity\n";

      // ✨ 修复：正确显示当前功率
#if ESP_ARDUINO_VERSION_MAJOR >= 3
      output += "wifi: Current: " + String(getWiFiTxPowerDbm()) + " dBm\n";
#else
      output += "wifi: Current: " + String(WiFi.getTxPower()) + " dBm\n";
#endif
      output += "wifi: Note: Actual power may be limited by WiFi mode (b/g/n)\n";
      return;
    }

    int power = valStr.toInt();

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    if (power < 2 || power > 20) {
      output = "wifi: Error: Power must be 2 ~ 20 dBm (Core 3.x)\n";
      return;
    }
#else
    if (power < -20 || power > 20) {
      output = "wifi: Error: Power must be -20 ~ 20 dBm (Core 2.x)\n";
      return;
    }
#endif

    if (setWiFiTxPower(power)) {
      output = "wifi: Tx power set to " + String(power) + " dBm\n";

      wifi_mode_t mode = WiFi.getMode();
      if (mode & WIFI_STA) {
        output += "wifi: Note: In 802.11n mode, max ~15dBm may apply\n";
      }
    } else {
      output = "wifi: Failed to set power (check WiFi mode/state)\n";
    }
    return;
  }
  output += "WiFi Commands (Safe Mode):\n";
  output += "  wifi [status]        - Show WiFi status summary\n";
  output += "  wifi info            - Display detailed configuration\n";
  output += "  wifi scan            - Scan nearby wireless networks\n";
  output += "  wifi stats           - Show signal quality & rate info\n";
  output += "  wifi set power <dBm> - Adjust TX power (2~20, safe)\n\n";
  output += "Note: Commands that change SSID/channel/mode are disabled\n";
  output += "      to prevent WebSocket disconnection.\n";
}

// ========== 目录递归删除 ==========
bool deleteDirectoryRecursive(String path) {
  if (path == "/" || path == "") return false;
  if (isProtectedPath(path)) return false;
  File dir = LittleFS.open(path, "r");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }
  std::vector<String> items;
  File item = dir.openNextFile();
  while (item) {
    String itemPath = item.path();
    if (itemPath.isEmpty()) {
      String itemName = item.name();
      int lastSlash = itemName.lastIndexOf('/');
      if (lastSlash != -1) itemName = itemName.substring(lastSlash + 1);
      itemPath = joinPath(path, itemName);
    }
    if (!isProtectedPath(itemPath)) { items.push_back(itemPath); }
    item.close();
    item = dir.openNextFile();
  }
  dir.close();
  bool success = true;
  for (const String& itemPath : items) {
    File check = LittleFS.open(itemPath, "r");
    bool isDir = (check && check.isDirectory());
    if (check) check.close();
    if (isDir) {
      if (!deleteDirectoryRecursive(itemPath)) success = false;
    } else {
      if (!LittleFS.remove(itemPath)) success = false;
    }
    yield();
  }
  if (success && !LittleFS.rmdir(path)) success = false;
  return success;
}

// ========== 脚本解释器辅助函数 ==========
String extractSubstring(String str, int start, int end) {
  String res = "";
  for (int i = start; i < end && i < str.length(); i++) res += str[i];
  return res;
}

int calculateExpression(String expr) {
  expr.trim();
  if (expr.isEmpty()) return 0;
  int parenDepth = 0, lastOpenParen = -1;
  for (int i = 0; i < expr.length(); i++) {
    if (expr[i] == '(') {
      lastOpenParen = i;
      parenDepth++;
    } else if (expr[i] == ')') {
      parenDepth--;
      if (parenDepth == 0 && lastOpenParen != -1) {
        String left = extractSubstring(expr, 0, lastOpenParen);
        String mid = extractSubstring(expr, lastOpenParen + 1, i);
        String right = extractSubstring(expr, i + 1, expr.length());
        int midVal = calculateExpression(mid);
        return calculateExpression(left + String(midVal) + right);
      }
    }
  }
  String clean = "";
  for (int i = 0; i < expr.length(); i++)
    if (expr[i] != ' ') clean += expr[i];
  if (clean.isEmpty()) return 0;
  std::vector<int> nums;
  std::vector<char> ops;
  String numStr;
  for (int i = 0; i <= clean.length(); i++) {
    char c = (i == clean.length()) ? '+' : clean[i];
    if (isdigit(c) || (c == '-' && (i == 0 || !isdigit(clean[i - 1])))) numStr += c;
    else {
      if (!numStr.isEmpty()) {
        nums.push_back(numStr.toInt());
        numStr = "";
      }
      if (i < clean.length()) ops.push_back(c);
    }
  }
  if (nums.empty()) return 0;
  for (int i = 0; i < ops.size();) {
    if (ops[i] == '*' || ops[i] == '/' || ops[i] == '%') {
      int result = (ops[i] == '*') ? nums[i] * nums[i + 1] : (ops[i] == '/') ? (nums[i + 1] ? nums[i] / nums[i + 1] : 0)
                                                                             : (nums[i + 1] ? nums[i] % nums[i + 1] : 0);
      nums[i] = result;
      nums.erase(nums.begin() + i + 1);
      ops.erase(ops.begin() + i);
    } else i++;
  }
  int result = nums[0];
  for (int i = 0; i < ops.size() && i + 1 < nums.size(); i++)
    result = (ops[i] == '+') ? result + nums[i + 1] : result - nums[i + 1];
  return result;
}

String substituteVariables(String line) {
  int pos = 0;
  while ((pos = line.indexOf('$', pos)) != -1) {
    int end = pos + 1;
    if (end >= line.length()) break;
    if (line[end] == '?') end++;
    else
      while (end < line.length() && (isalnum(line[end]) || line[end] == '_')) end++;
    if (end == pos + 1) {
      pos++;
      continue;
    }
    String varName = line.substring(pos + 1, end);
    varName.trim();
    String replacement = (varName == "?") ? String(g_lastExitCode) : (scriptVars.count(varName) ? scriptVars[varName] : "");
    line = line.substring(0, pos) + replacement + line.substring(end);
    pos = pos + replacement.length();
    if (pos > line.length()) pos = line.length();
  }
  return line;
}

bool containsAnyChar(String str, const char* chars) {
  for (int i = 0; chars[i] != 0; i++)
    if (str.indexOf(chars[i]) != -1) return true;
  return false;
}

bool evaluateCondition(String condition) {
  condition.trim();
  if (condition.startsWith("(") && condition.endsWith(")")) {
    condition = condition.substring(1, condition.length() - 1);
    condition.trim();
  }
  const char* ops[] = { "==", "!=", "<=", ">=", "<", ">" };
  int foundOp = -1, opPos = -1;
  for (int i = 0; i < 6; i++) {
    int p = condition.indexOf(ops[i]);
    if (p != -1 && (opPos == -1 || p < opPos)) {
      opPos = p;
      foundOp = i;
    }
  }
  if (foundOp == -1) return !condition.isEmpty();
  String left = condition.substring(0, opPos), right = condition.substring(opPos + strlen(ops[foundOp]));
  left.trim();
  right.trim();
  int leftNum = calculateExpression(left), rightNum = calculateExpression(right);
  bool leftIsNum = (!left.isEmpty() && (String(leftNum) == left || containsAnyChar(left, "+-*/%")));
  bool rightIsNum = (!right.isEmpty() && (String(rightNum) == right || containsAnyChar(right, "+-*/%")));
  if (leftIsNum && rightIsNum) {
    switch (foundOp) {
      case 0: return leftNum == rightNum;
      case 1: return leftNum != rightNum;
      case 2: return leftNum <= rightNum;
      case 3: return leftNum >= rightNum;
      case 4: return leftNum < rightNum;
      case 5: return leftNum > rightNum;
    }
  }
  if (foundOp == 0) return left == right;
  if (foundOp == 1) return left != right;
  return false;
}

int findBlockEndEx(const std::vector<String>& lines, int startLine, const char* endKeyword) {
  int depth = 1;
  for (int i = startLine; i < (int)lines.size(); i++) {
    String line = lines[i];
    line.trim();
    if (line.startsWith("if ") || line.startsWith("for ") || line.startsWith("while ")) depth++;
    else if (line == "fi" || line == "done") {
      depth--;
      if (depth == 0 && line == endKeyword) return i;
    }
  }
  return -1;
}

bool loadScriptInput(const String& inputPath) {
  scriptInputLines.clear();
  scriptInputIndex = 0;
  if (!LittleFS.exists(inputPath)) return false;
  File f = LittleFS.open(inputPath, "r");
  if (!f || f.isDirectory()) {
    f.close();
    return false;
  }
  String line;
  while (f.available()) {
    char c = f.read();
    if (c == '\n') {
      scriptInputLines.push_back(line);
      line = "";
    } else if (c != '\r') line += c;
  }
  if (!line.isEmpty()) scriptInputLines.push_back(line);
  f.close();
  return true;
}

bool scriptReadInput(String& varName, String& output) {
  if (scriptInputIndex < (int)scriptInputLines.size()) {
    scriptVars[varName] = scriptInputLines[scriptInputIndex++];
    output += "  > script: read " + varName + " = '" + scriptVars[varName] + "'\n";
    return true;
  }
  output += "  > script: Warning: No more input data\n";
  scriptVars[varName] = "";
  return false;
}

// ========== 前置声明 ==========
void executeCommand(String cmd, String& output, String& newPrompt, bool& clearTerminal, String& dlFileName, String& dlContent, bool& triggerUpload);
void executeScriptBlockEx(const std::vector<String>& lines, int startLine, int endLine, String& output, bool* outBreak = nullptr, bool* outContinue = nullptr);

// ========== 分块传输函数（修复版）==========
bool startChunkedDownload(const String& filePath, uint8_t clientNum) {
  if (!LittleFS.exists(filePath)) return false;
  File f = LittleFS.open(filePath, "r");
  if (!f || f.isDirectory()) {
    if (f) f.close();
    return false;
  }
  size_t fSize = f.size();
  if (fSize > MAX_FILE_SIZE) {
    f.close();
    String err = "{\"output\":\"dl: File too large (max " + String(MAX_FILE_SIZE / 1024) + "KB)\\n\"}";
    webSocket.sendTXT(clientNum, err);
    return false;
  }
  chunkState.active = true;
  chunkState.isUpload = false;
  chunkState.sending = false;
  chunkState.lastPrintedProgress = -1;
  chunkState.filename = filePath.substring(filePath.lastIndexOf('/') + 1);
  chunkState.totalSize = fSize;
  chunkState.totalChunks = (fSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
  chunkState.currentChunk = 0;
  chunkState.file = f;
  chunkState.clientNum = clientNum;
  chunkState.lastChunkTime = millis();
  String header = "{\"dl_start\":{\"filename\":\"" + escapeJson(chunkState.filename) + "\",\"size\":" + String(fSize) + ",\"chunks\":" + String(chunkState.totalChunks) + ",\"chunk_size\":" + String(CHUNK_SIZE) + "}}";
  webSocket.sendTXT(clientNum, header);
  return true;
}

void sendNextChunk() {
  if (!chunkState.active || chunkState.isUpload || chunkState.sending) return;

  if (chunkState.currentChunk >= chunkState.totalChunks) {
    String endMsg = "{\"dl_end\":{\"status\":\"complete\",\"checksum\":\"ok\"}}";
    webSocket.sendTXT(chunkState.clientNum, endMsg);
    chunkState.file.close();
    chunkState.active = false;
    chunkState.sending = false;
    Serial.println("dl: Transfer complete: " + chunkState.filename);
    return;
  }

  chunkState.sending = true;

  size_t offset = chunkState.currentChunk * CHUNK_SIZE;
  size_t readSize = min(CHUNK_SIZE, chunkState.totalSize - offset);

  chunkState.file.seek(offset);
  std::vector<uint8_t> buffer(readSize);
  size_t actuallyRead = chunkState.file.read(buffer.data(), readSize);

  if (actuallyRead != readSize) {
    Serial.printf("dl: Warning: read %d/%d bytes at offset %d\n", actuallyRead, readSize, offset);
  }

  String b64 = base64Encode(buffer.data(), actuallyRead);
  String chunkMsg = "{\"dl_chunk\":{\"index\":" + String(chunkState.currentChunk) + ",\"data\":\"" + b64 + "\",\"size\":" + String(actuallyRead) + ",\"chunk_size\":" + String(CHUNK_SIZE) + ",\"progress\":" + String((chunkState.currentChunk + 1) * 100 / chunkState.totalChunks) + "}}";

  bool sent = webSocket.sendTXT(chunkState.clientNum, chunkMsg);

  if (sent) {
    chunkState.currentChunk++;
    chunkState.lastChunkTime = millis();
    int progress = chunkState.currentChunk * 100 / chunkState.totalChunks;
    if (progress % 10 == 0 && progress != chunkState.lastPrintedProgress) {
      Serial.printf("dl: %s - %d%%\n", chunkState.filename.c_str(), progress);
      chunkState.lastPrintedProgress = progress;
    }
  }

  chunkState.sending = false;
  yield();
}

bool handleUploadStart(const String& filename, size_t totalSize, size_t clientChunkSize, uint8_t clientNum) {
  String safeName = filename;
  safeName.replace("/", "");
  safeName.replace("\\", "");
  safeName.replace("..", "");
  if (safeName.isEmpty()) safeName = "upload_" + String(millis()) + ".bin";
  String fullPath = resolvePath(safeName);
  if (!canWritePath(fullPath)) {
    String err = "{\"ul_ack\":{\"status\":\"error\",\"msg\":\"Permission denied\"}}";
    webSocket.sendTXT(clientNum, err);
    return false;
  }
  if (totalSize > MAX_FILE_SIZE) {
    String err = "{\"ul_ack\":{\"status\":\"error\",\"msg\":\"File too large\"}}";
    webSocket.sendTXT(clientNum, err);
    return false;
  }
  int lastSlash = fullPath.lastIndexOf('/');
  if (lastSlash > 0 && !LittleFS.exists(fullPath.substring(0, lastSlash))) {
    LittleFS.mkdir(fullPath.substring(0, lastSlash));
  }
  chunkState.active = true;
  chunkState.isUpload = true;
  chunkState.sending = false;
  chunkState.lastPrintedProgress = -1;
  chunkState.filename = fullPath;
  chunkState.totalSize = totalSize;
  chunkState.clientChunkSize = (clientChunkSize > 0) ? clientChunkSize : CHUNK_SIZE;
  chunkState.totalChunks = (totalSize + chunkState.clientChunkSize - 1) / chunkState.clientChunkSize;
  chunkState.currentChunk = 0;

  chunkState.file = LittleFS.open(fullPath, "w");
  chunkState.clientNum = clientNum;
  chunkState.lastChunkTime = millis();
  if (!chunkState.file) {
    chunkState.active = false;
    String err = "{\"ul_ack\":{\"status\":\"error\",\"msg\":\"Cannot create file\"}}";
    webSocket.sendTXT(clientNum, err);
    return false;
  }
  String ack = "{\"ul_ack\":{\"status\":\"ready\",\"chunks\":" + String(chunkState.totalChunks) + "}}";
  webSocket.sendTXT(clientNum, ack);
  return true;
}

bool handleUploadChunk(size_t index, const String& b64Data, uint8_t clientNum) {
  if (!chunkState.active || !chunkState.isUpload || index != chunkState.currentChunk) {
    Serial.printf("ul: Reject chunk %d (expected %d)\n", index, chunkState.currentChunk);
    return false;
  }

  std::vector<uint8_t> binData = base64Decode(b64Data);
  if (binData.empty() && !b64Data.isEmpty()) {
    Serial.println("ul: Base64 decode failed!");
    return false;
  }

  size_t offset = index * chunkState.clientChunkSize;

  if (!chunkState.file.seek(offset)) {
    Serial.printf("ul: seek(%d) failed!\n", offset);
    return false;
  }

  size_t written = chunkState.file.write(binData.data(), binData.size());
  if (written != binData.size()) {
    Serial.printf("ul: Write incomplete: %d/%d bytes at offset %d\n", written, binData.size(), offset);
    if (written < binData.size()) {
      size_t remaining = binData.size() - written;
      size_t written2 = chunkState.file.write(binData.data() + written, remaining);
      if (written2 != remaining) {
        Serial.println("ul: Critical write error after retry!");
        return false;
      }
      written += written2;
    }
  }

  if (chunkState.currentChunk % 8 == 0) {
    chunkState.file.flush();
  }

  chunkState.currentChunk++;
  chunkState.lastChunkTime = millis();

  int progress = chunkState.currentChunk * 100 / chunkState.totalChunks;
  String ack = "{\"ul_ack\":{\"index\":" + String(index) + ",\"received\":" + String(chunkState.currentChunk * chunkState.clientChunkSize) + ",\"progress\":" + String(progress) + "}}";
  webSocket.sendTXT(clientNum, ack);

  if (chunkState.currentChunk >= chunkState.totalChunks) {
    chunkState.file.flush();
    chunkState.file.close();

    File verify = LittleFS.open(chunkState.filename, "r");
    if (verify) {
      size_t actualSize = verify.size();
      verify.close();
      if (actualSize != chunkState.totalSize) {
        Serial.printf("ul: Warning: file size mismatch! expected %d, got %d\n", chunkState.totalSize, actualSize);
      }
    }

    String done = "{\"ul_ack\":{\"status\":\"complete\",\"written\":" + String(chunkState.totalSize) + "}}";
    webSocket.sendTXT(clientNum, done);
    Serial.printf("ul: Saved '%s' (%d bytes, %d chunks)\n", chunkState.filename.c_str(), chunkState.totalSize, chunkState.totalChunks);
    chunkState.active = false;
    return true;
  }
  return true;
}

void checkChunkTimeout() {
  if (!chunkState.active) return;
  uint32_t elapsed = millis() - chunkState.lastChunkTime;
  if (elapsed > CHUNK_TIMEOUT) {
    String err = chunkState.isUpload ? "{\"ul_ack\":{\"status\":\"timeout\"}}" : "{\"output\":\"dl: Transfer timeout\\n\"}";
    if (chunkState.file) {
      chunkState.file.flush();
      chunkState.file.close();
    }
    chunkState.active = false;
    chunkState.sending = false;
    if (chunkState.clientNum < 255) webSocket.sendTXT(chunkState.clientNum, err);
    Serial.printf("chunk: Timeout after %dms\n", elapsed);
  }
}

// ========== LLM 回调函数 ==========
void on_llm_token(const char* token, void* user_data) {
  uint8_t* client_id = (uint8_t*)user_data;
  if (*client_id < 255) {
    String json = "{\"output\":\"";
    const char* p = token;
    while (*p) {
      switch (*p) {
        case '\\': json += "\\\\"; break;
        case '"': json += "\\\""; break;
        case '\n': json += "\\n"; break;
        case '\r': json += "\\r"; break;
        default: json += *p; break;
      }
      p++;
    }
    json += "\"}";
    webSocket.sendTXT(*client_id, json);
  }
  Serial.print(token);
}

void on_llm_done(float tps, void* user_data) {
  uint8_t* client_id = (uint8_t*)user_data;
  llmGenerationActive = false;
  if (*client_id < 255) {
    String json = "{\"output\":\"\\nllama: Done: " + String(tps, 1) + " tok/s\\n\"}";
    webSocket.sendTXT(*client_id, json);
  }
  Serial.printf("\nllama: Done: %.1f tok/s\n", tps);
}

bool executeScriptLine(const String& line, const std::vector<String>& lines, int& lineIdx, String& output, bool& shouldBreak, bool& shouldContinue) {
  shouldBreak = shouldContinue = false;
  String trimmed = line;
  trimmed.trim();
  if (trimmed.isEmpty() || trimmed.startsWith("#")) return true;
  String processed = substituteVariables(trimmed);
  if (processed == "break") {
    shouldBreak = true;
    return true;
  }
  if (processed == "continue") {
    shouldContinue = true;
    return true;
  }
  if (processed.startsWith("read ")) {
    String varName = processed.substring(5);
    varName.trim();
    if (varName.isEmpty()) {
      output += "  > script: Error: read requires a variable name\n";
      return true;
    }
    scriptReadInput(varName, output);
    return true;
  }
  if (processed == "sleep" || processed.startsWith("sleep ")) {
    String arg = (processed == "sleep") ? "" : processed.substring(6);
    arg.trim();
    if (arg.isEmpty()) {
      output += "  > sleep: Usage: sleep <seconds> or <milliseconds>ms\n";
      return true;
    }
    unsigned long delayMs = arg.endsWith("ms") ? arg.substring(0, arg.length() - 2).toInt() : arg.toInt() * 1000;
    output += "  > sleep: Waiting " + arg + "...\n";
    for (unsigned long elapsed = 0; elapsed < delayMs; elapsed += 50) {
      delay(min((unsigned long)50, delayMs - elapsed));
      yield();
    }
    return true;
  }
  if (processed.startsWith("set ")) {
    String assign = processed.substring(4);
    int eqPos = assign.indexOf('=');
    if (eqPos != -1) {
      String var = assign.substring(0, eqPos), val = assign.substring(eqPos + 1);
      var.trim();
      val.trim();
      int calcResult = calculateExpression(val);
      bool isExpr = containsAnyChar(val, "+-*/%()");
      scriptVars[var] = (isExpr || val == String(calcResult)) ? String(calcResult) : (val.startsWith("\"") && val.endsWith("\"") ? val.substring(1, val.length() - 1) : val);
    }
    return true;
  }
  if (processed.startsWith("if ")) {
    String condition = processed.substring(3);
    int thenPos = condition.indexOf(" then");
    if (thenPos != -1) condition = condition.substring(0, thenPos);
    condition.trim();
    condition = substituteVariables(condition);
    bool condResult = evaluateCondition(condition);
    int fiLine = findBlockEndEx(lines, lineIdx + 1, "fi");
    if (fiLine == -1) {
      output += "  > script: Error: missing 'fi'\n";
      return false;
    }
    int elseLine = -1;
    for (int j = lineIdx + 1; j < fiLine; j++) {
      String temp = lines[j];
      temp.trim();
      if (temp == "else") {
        elseLine = j;
        break;
      }
    }
    if (condResult) executeScriptBlockEx(lines, lineIdx + 1, (elseLine != -1 ? elseLine : fiLine), output);
    else if (elseLine != -1) executeScriptBlockEx(lines, elseLine + 1, fiLine, output);
    lineIdx = fiLine;
    return true;
  }
  if (trimmed.startsWith("while ")) {
    String condition = trimmed.substring(6);
    int doPos = condition.indexOf(" do");
    if (doPos != -1) condition = condition.substring(0, doPos);
    condition.trim();
    int doneLine = findBlockEndEx(lines, lineIdx + 1, "done");
    if (doneLine == -1) {
      output += "  > script: Error: missing 'done' for while\n";
      return false;
    }
    for (int iter = 0; iter < 500; iter++) {
      if (!evaluateCondition(substituteVariables(condition))) break;
      bool loopBreak = false, loopContinue = false;
      executeScriptBlockEx(lines, lineIdx + 1, doneLine, output, &loopBreak, &loopContinue);
      if (loopBreak) break;
      if (loopContinue) {
        yield();
        continue;
      }
      yield();
    }
    lineIdx = doneLine;
    return true;
  }
  if (processed.startsWith("for ")) {
    String rest = processed.substring(4);
    int inPos = rest.indexOf(" in "), doPos = rest.indexOf(" do");
    if (inPos == -1 || doPos == -1) {
      output += "  > script: Syntax error: invalid for loop\n";
      return false;
    }
    String varName = rest.substring(0, inPos), range = rest.substring(inPos + 4, doPos);
    varName.trim();
    range.trim();
    int dotPos = range.indexOf("..");
    if (dotPos == -1) {
      output += "  > script: Syntax error: use 'for i in 1..5 do'\n";
      return false;
    }
    int start = calculateExpression(substituteVariables(range.substring(0, dotPos)));
    int end = calculateExpression(substituteVariables(range.substring(dotPos + 2)));
    int doneLine = findBlockEndEx(lines, lineIdx + 1, "done");
    if (doneLine == -1) {
      output += "  > script: Error: missing 'done'\n";
      return false;
    }
    for (int j = start; j <= end; j++) {
      scriptVars[varName] = String(j);
      bool loopBreak = false, loopContinue = false;
      executeScriptBlockEx(lines, lineIdx + 1, doneLine, output, &loopBreak, &loopContinue);
      if (loopBreak) break;
      if (loopContinue) continue;
      yield();
    }
    lineIdx = doneLine;
    return true;
  }
  String cmdOutput, dummyPrompt, dummyDlF, dummyDlC;
  bool dummyClear, dummyUl;
  executeCommand(processed, cmdOutput, dummyPrompt, dummyClear, dummyDlF, dummyDlC, dummyUl);
  if (!cmdOutput.isEmpty()) {
    output += "  > ";
    int start = 0;
    for (size_t i = 0; i < cmdOutput.length(); i++) {
      if (cmdOutput[i] == '\n') {
        output += cmdOutput.substring(start, i + 1);
        if (i + 1 < cmdOutput.length()) output += "  > ";
        start = i + 1;
      }
    }
    if (start < cmdOutput.length()) output += cmdOutput.substring(start);
  }
  return true;
}

void executeScriptBlockEx(const std::vector<String>& lines, int startLine, int endLine, String& output, bool* outBreak, bool* outContinue) {
  if (startLine < 0 || endLine > (int)lines.size() || startLine >= endLine) {
    output += "  > script: Error: invalid block range\n";
    return;
  }
  for (int i = startLine; i < endLine;) {
    if (i < 0 || i >= (int)lines.size()) break;
    bool shouldBreak = false, shouldContinue = false;
    if (!executeScriptLine(lines[i], lines, i, output, shouldBreak, shouldContinue)) break;
    if (shouldContinue) {
      if (outContinue) *outContinue = true;
      return;
    }
    if (shouldBreak) {
      if (outBreak) *outBreak = true;
      return;
    }
    i++;
  }
}

// ========== HTML前端内容 ==========
const char HTML_CONTENT[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-QINUX Terminal</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { background: #000; display: flex; justify-content: center; align-items: center; min-height: 100vh; font-family: 'Courier New', monospace; overflow: hidden; transition: background 0.3s; }
        body.matrix-active { background: #000 !important; }
        .crt { width: 95vw; max-width: 1200px; aspect-ratio: 4 / 3; max-height: 92vh; background: #1a1a1a; border-radius: 20px; padding: clamp(10px, 2vw, 30px); box-shadow: 0 0 50px rgba(0, 255, 0, 0.15), inset 0 0 20px rgba(0, 0, 0, 0.8); position: relative; display: flex; flex-direction: column; }
        .screen { flex: 1; width: 100%; background: #000000; border: 3px solid #003300; border-radius: 10px; padding: clamp(5px, 1.5vw, 15px); color: #00ff00; font-size: clamp(9px, 1.4vw, 18px); line-height: 1.2; position: relative; overflow: hidden; text-shadow: 0 0 clamp(1px, 0.3vw, 3px) #00ff00; display: flex; flex-direction: column; }
        .scanline { position: absolute; top: 0; left: 0; width: 100%; height: 100%; background: linear-gradient(to bottom, transparent 50%, rgba(0, 30, 0, 0.25) 50%); background-size: 100% clamp(4px, 1vw, 8px); pointer-events: none; animation: scan 4s linear infinite; z-index: 1; }
        .screen::after { content: ''; position: absolute; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0, 255, 0, 0.02); pointer-events: none; animation: flicker 0.15s infinite alternate; z-index: 1; }
        @keyframes scan { 0% { transform: translateY(0); } 100% { transform: translateY(-4px); } }
        @keyframes flicker { 0% { opacity: 0.97; } 100% { opacity: 1; } }
        #output { flex: 1; width: 100%; overflow-y: auto; white-space: pre-wrap; word-wrap: break-word; margin-bottom: clamp(5px, 1vw, 10px); z-index: 2; position: relative; }
        #output::-webkit-scrollbar { width: clamp(4px, 0.8vw, 8px); }
        #output::-webkit-scrollbar-thumb { background: #005500; border-radius: 3px; }
        .input-line { display: flex; align-items: center; width: 100%; flex-shrink: 0; z-index: 2; position: relative; }
        .prompt { color: #00ff00; margin-right: clamp(4px, 0.5vw, 8px); flex-shrink: 0; }
        #input { flex: 1; background: transparent; border: none; outline: none; color: #00ff00; font-family: 'Courier New', monospace; font-size: inherit; text-shadow: 0 0 clamp(1px, 0.3vw, 3px) #00ff00; }
        #hiddenFileInput { display: none; }
        #matrix-canvas { position: absolute; top: 0; left: 0; width: 100%; height: 100%; background: #000; z-index: 10; display: none; }
        .matrix-active #output, .matrix-active .input-line { display: none !important; }
        .cat-container { position: fixed; top: 0; left: 0; width: 100vw; height: 100vh; background: #000; overflow: hidden; z-index: 150; display: none; pointer-events: none; }
        .falling-cat { position: absolute; top: -10%; animation: fall linear forwards; will-change: transform, opacity; user-select: none; pointer-events: none; filter: drop-shadow(0 0 2px rgba(0,255,0,0.3)); }
        @keyframes fall { 0% { transform: translateY(0) rotate(-10deg); opacity: 0; } 5% { opacity: 0.9; } 95% { opacity: 0.9; } 100% { transform: translateY(115vh) rotate(20deg); opacity: 0; } }
        .cat-active .crt { display: none !important; }
        .cat-active body { background: #000 !important; }
        .fortune-text { color: #00ff88; font-style: italic; text-shadow: 0 0 5px #00ff88; animation: glow 2s ease-in-out infinite alternate; display: block; margin: 4px 0; }
        @keyframes glow { from { text-shadow: 0 0 3px #00ff88; } to { text-shadow: 0 0 15px #00ff88, 0 0 30px #00aa55; } }
        .exit-hint { position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%); color: #0f0; background: rgba(0, 20, 0, 0.85); padding: 8px 16px; border-radius: 4px; z-index: 200; font-size: clamp(10px, 2vw, 14px); border: 1px solid #005500; animation: hintFade 3s forwards; }
        @keyframes hintFade { 0%, 80% { opacity: 1; } 100% { opacity: 0; } }
    </style>
</head>
<body>
    <input type="file" id="hiddenFileInput">
    <canvas id="matrix-canvas"></canvas>
    <div class="cat-container" id="catContainer"></div>
    <div class="crt">
        <div class="screen">
            <div class="scanline"></div>
            <div id="output"></div>
            <div class="input-line">
                <span class="prompt" id="prompt">root@esp32:/# </span>
                <input type="text" id="input" autocomplete="off" autofocus>
            </div>
        </div>
    </div>
    <script>
        const output = document.getElementById('output');
        const input = document.getElementById('input');
        const prompt = document.getElementById('prompt');
        const fileInput = document.getElementById('hiddenFileInput');
        const catContainer = document.getElementById('catContainer');
        const matrixCanvas = document.getElementById('matrix-canvas');
        const matrixCtx = matrixCanvas.getContext('2d');
        const CHUNK_SIZE = 4096;
        let ws;
        let matrixInterval = null;
        let catSpawnInterval = null;
        const CAT_EMOJIS = ['🐱', '😺', '😸', '😻', '😽', '🙀', '😿', '😾', '🐈', '🐈‍⬛', '🐾'];
        const cmdHistory = [];
        let histPos = -1;
        let isEditingMode = false;
        let lastDlProg = -1;
        let lastUlProg = -1;

        function showExitHint(text) {
            const existing = document.querySelector('.exit-hint');
            if (existing) existing.remove();
            const hint = document.createElement('div');
            hint.className = 'exit-hint';
            hint.textContent = text;
            document.body.appendChild(hint);
            setTimeout(() => { if (hint.parentNode) hint.remove(); }, 3000);
        }

        function startMatrix() {
            matrixCanvas.width = window.innerWidth;
            matrixCanvas.height = window.innerHeight;
            const chars = '01ﾠﾡ￡￢￥￤￦￧￨￩￪￫￬￭｡｢｣､･ｦｧｨｩｪｫｬｭｮｯｰｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝ1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ';
            const fontSize = 14;
            const columns = Math.floor(matrixCanvas.width / fontSize);
            const drops = [];
            for (let i = 0; i < columns; i++) drops[i] = Math.random() * -100;
            document.body.classList.add('matrix-active');
            matrixCanvas.style.display = 'block';
            showExitHint("Press any key / Click / ESC to exit Matrix");
            function draw() {
                matrixCtx.fillStyle = 'rgba(0, 0, 0, 0.05)';
                matrixCtx.fillRect(0, 0, matrixCanvas.width, matrixCanvas.height);
                matrixCtx.fillStyle = '#0f0';
                matrixCtx.font = fontSize + 'px monospace';
                for (let i = 0; i < drops.length; i++) {
                    matrixCtx.fillText(chars.charAt(Math.floor(Math.random() * chars.length)), i * fontSize, drops[i] * fontSize);
                    if (drops[i] * fontSize > matrixCanvas.height && Math.random() > 0.975) drops[i] = 0;
                    drops[i]++;
                }
            }
            matrixInterval = setInterval(draw, 33);
            const exitHandler = (e) => {
                if (['Escape', 'Enter', ' '].includes(e.key) || e.type === 'click') {
                    e.preventDefault();
                    stopMatrix();
                    document.removeEventListener('keydown', exitHandler);
                    document.removeEventListener('click', exitHandler);
                }
            };
            document.addEventListener('keydown', exitHandler);
            document.addEventListener('click', exitHandler);
        }
        function stopMatrix() {
            if (matrixInterval) { clearInterval(matrixInterval); matrixInterval = null; }
            matrixCanvas.style.display = 'none';
            document.body.classList.remove('matrix-active');
            input.focus();
        }
        function startCatEasterEgg() {
            catContainer.innerHTML = '';
            document.body.classList.add('cat-active');
            catContainer.style.display = 'block';
            showExitHint("Click anywhere or press any key to stop the cats 🐱");
            for(let i=0; i<10; i++) setTimeout(spawnCat, i * 50);
            catSpawnInterval = setInterval(spawnCat, 100);
            const exitHandler = (e) => {
                e.preventDefault?.();
                stopCatEasterEgg();
                document.removeEventListener('click', exitHandler);
                document.removeEventListener('keydown', exitHandler);
            };
            document.addEventListener('click', exitHandler);
            document.addEventListener('keydown', exitHandler);
        }
        function spawnCat() {
            const cat = document.createElement('div');
            cat.className = 'falling-cat';
            cat.textContent = CAT_EMOJIS[Math.floor(Math.random() * CAT_EMOJIS.length)];
            const left = Math.random() * 100;
            const size = Math.random() * 40 + 24;
            const duration = Math.random() * 2.5 + 2.5;
            const delay = Math.random() * 0.2;
            cat.style.left = `${left}%`;
            cat.style.fontSize = `${size}px`;
            cat.style.animationDuration = `${duration}s`;
            cat.style.animationDelay = `${delay}s`;
            cat.style.opacity = Math.random() * 0.3 + 0.7;
            cat.addEventListener('animationend', () => cat.remove());
            catContainer.appendChild(cat);
        }
        function stopCatEasterEgg() {
            if (catSpawnInterval) { clearInterval(catSpawnInterval); catSpawnInterval = null; }
            catContainer.innerHTML = '';
            document.body.classList.remove('cat-active');
            catContainer.style.display = 'none';
            input.focus();
        }
        function renderFortune(text) {
            const clean = text.trim().replace(/^"|"$/g, '');
            return `<span class="fortune-text"> ${clean}</span>\n`;
        }
        function formatBytes(bytes) {
            if (bytes < 1024) return bytes + ' B';
            if (bytes < 1024*1024) return (bytes/1024).toFixed(1) + ' KB';
            return (bytes/1024/1024).toFixed(1) + ' MB';
        }
        function handleDownloadStart(info) {
            window.dlBuffer = new Uint8Array(info.size);
            window.dlReceived = 0;
            window.dlFilename = info.filename;
            window.dlTotal = info.size;
            window.dlChunkSize = info.chunk_size || CHUNK_SIZE;
            lastDlProg = -1;
            output.textContent += `dl: Starting: ${info.filename} (${formatBytes(info.size)})\n`;
            output.scrollTop = output.scrollHeight;
        }
        function handleDownloadChunk(chunk) {
            if (!window.dlBuffer) return;
            try {
                const offset = chunk.index * (chunk.chunk_size || CHUNK_SIZE);
                const binary = atob(chunk.data);
                for (let i = 0; i < binary.length; i++) { window.dlBuffer[offset + i] = binary.charCodeAt(i); }
                window.dlReceived += chunk.size;
                let prog = chunk.progress || Math.floor(window.dlReceived * 100 / window.dlTotal);
                let step = Math.floor(prog / 5);
                if (step > lastDlProg) {
                    lastDlProg = step;
                    output.textContent += `dl: Progress: ${prog}%\n`;
                    output.scrollTop = output.scrollHeight;
                }
                ws.send(`__DL_ACK__:${chunk.index}`);
            } catch(e) {
                output.textContent += `dl: Error: ${e.message}\n`;
                output.textContent += `dl: Reconnecting...\n`;
                output.scrollTop = output.scrollHeight;
                setTimeout(() => connect(), 1000);
            }
        }
        function handleDownloadEnd(info) {
            if (!window.dlBuffer) return;
            const blob = new Blob([window.dlBuffer]);
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = window.dlFilename;
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            URL.revokeObjectURL(url);
            output.textContent += `dl: Complete: ${window.dlFilename}\n`;
            output.scrollTop = output.scrollHeight;
            delete window.dlBuffer;
        }
        function startChunkedUpload(file) {
            const reader = new FileReader();
            const chunkSize = CHUNK_SIZE;
            let offset = 0;
            lastUlProg = -1;
            output.textContent += `ul: Starting: ${file.name} (${formatBytes(file.size)})\n`;
            output.scrollTop = output.scrollHeight;
            ws.send(`__UPLOAD_START__:filename:${file.name},size:${file.size},chunk_size:${chunkSize}`);
            function sendNextChunk() {
                if (offset >= file.size) {
                    ws.send(`__UPLOAD_END__`);
                    output.textContent += `ul: Saving file...\n`;
                    output.scrollTop = output.scrollHeight;
                    return;
                }
                const end = Math.min(offset + chunkSize, file.size);
                const slice = file.slice(offset, end);
                reader.onload = (e) => {
                    const arr = new Uint8Array(e.target.result);
                    let binary = '';
                    for (let i = 0; i < arr.length; i++) binary += String.fromCharCode(arr[i]);
                    const b64 = btoa(binary);
                    ws.send(`__UPLOAD_CHUNK__:index:${Math.floor(offset/chunkSize)},${b64}`);
                    offset = end;
                    let prog = Math.floor(offset * 100 / file.size);
                    let step = Math.floor(prog / 5);
                    if (step > lastUlProg) {
                        lastUlProg = step;
                        output.textContent += `ul: Progress: ${prog}%\n`;
                        output.scrollTop = output.scrollHeight;
                    }
                    setTimeout(sendNextChunk, 50);
                };
                reader.readAsArrayBuffer(slice);
            }
            sendNextChunk();
        }
        function connect() {
            const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
            ws = new WebSocket(`${protocol}//${window.location.hostname}:81`);
            ws.onopen = () => {
                output.textContent += "=== ESP32-CINUX SYSTEM CONNECTED ===\n";
                output.textContent += "Type 'help' for command list\n\n";
                output.scrollTop = output.scrollHeight;
                syncTime();
            };
            ws.onmessage = (evt) => {
                try {
                    const data = JSON.parse(evt.data);
                    if (data.dl_start) { handleDownloadStart(data.dl_start); return; }
                    if (data.dl_chunk) { handleDownloadChunk(data.dl_chunk); return; }
                    if (data.dl_end) { handleDownloadEnd(data.dl_end); return; }
                    if (data.ul_ack) {
                        if (data.ul_ack.status === 'error') {
                            output.textContent += `ul: Error: ${data.ul_ack.msg}\n`;
                            output.scrollTop = output.scrollHeight;
                        } else if (data.ul_ack.status === 'complete') {
                            output.textContent += `ul: Saved: ${data.ul_ack.written} bytes\n`;
                            output.scrollTop = output.scrollHeight;
                        }
                        return;
                    }
                    if (data.output) {
                        if(data.output.includes("edit: Type 'EOF' to save & exit")) isEditingMode = true;
                        if(data.output.includes("edit: File saved")) isEditingMode = false;
                        if (data.output.includes("__MATRIX_MODE__")) { startMatrix(); return; }
                        if (data.output.includes("__CAT_EASTER_EGG__")) { startCatEasterEgg(); return; }
                        if (data.prompt && data.prompt.includes("fortune")) output.textContent += renderFortune(data.output);
                        else output.textContent += data.output;
                        output.scrollTop = output.scrollHeight;
                    }
                    if (data.prompt) prompt.textContent = data.prompt;
                    if (data.clear) output.textContent = '';
                    if (data.download) {
                        const { filename, content } = data.download;
                        try {
                            const byteCharacters = atob(content);
                            const byteNumbers = new Array(byteCharacters.length);
                            for (let i = 0; i < byteCharacters.length; i++) byteNumbers[i] = byteCharacters.charCodeAt(i);
                            const blob = new Blob([new Uint8Array(byteNumbers)]);
                            const url = URL.createObjectURL(blob);
                            const a = document.createElement('a'); a.href = url; a.download = filename;
                            document.body.appendChild(a); a.click(); document.body.removeChild(a);
                            URL.revokeObjectURL(url);
                        } catch(e) { output.textContent += "[ERR] Download failed: " + e.message + "\n"; output.scrollTop = output.scrollHeight; }
                    }
                    if (data.upload) fileInput.click();
                } catch (e) { output.textContent += "[ERR] Message parse failed\n"; output.scrollTop = output.scrollHeight; }
            };
            ws.onclose = () => { output.textContent += "\n=== CONNECTION LOST - RECONNECTING ===\n"; output.scrollTop = output.scrollHeight; setTimeout(connect, 2000); };
            ws.onerror = (err) => console.error("WebSocket error:", err);
        }
        fileInput.addEventListener('change', (e) => {
            const file = e.target.files[0];
            if (!file) return;
            startChunkedUpload(file);
            e.target.value = '';
        });
        function syncTime() { ws.send(`__SYNC_TIME__:${Math.floor(Date.now() / 1000)}`); }
        input.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
                const cmd = input.value;
                if(isEditingMode) output.textContent += cmd + '\n';
                else output.textContent += prompt.textContent + cmd + '\n';
                output.scrollTop = output.scrollHeight;
                if (cmd.trim()) ws.send(cmd);
                if (cmd.trim() && !document.body.classList.contains('matrix-active') && !document.body.classList.contains('cat-active')) {
                    cmdHistory.push(cmd);
                    histPos = -1;
                }
                input.value = '';
            }
            if (e.key === 'ArrowUp') {
                e.preventDefault();
                if (cmdHistory.length > 0) {
                    histPos = Math.min(histPos + 1, cmdHistory.length - 1);
                    input.value = cmdHistory[cmdHistory.length - 1 - histPos];
                }
            }
            if (e.key === 'ArrowDown') {
                e.preventDefault();
                if (histPos > 0) {
                    histPos--;
                    input.value = cmdHistory[cmdHistory.length - 1 - histPos];
                } else if (histPos === 0) {
                    histPos = -1;
                    input.value = '';
                }
            }
            if (document.body.classList.contains('matrix-active') || document.body.classList.contains('cat-active')) e.stopPropagation();
        });
        document.querySelector('.screen').addEventListener('click', (e) => {
            if (!document.body.classList.contains('matrix-active') && !document.body.classList.contains('cat-active')) input.focus();
        });
        window.addEventListener('resize', () => {
            if (matrixCanvas.style.display !== 'none') { matrixCanvas.width = window.innerWidth; matrixCanvas.height = window.innerHeight; }
        });
        document.addEventListener('touchmove', (e) => {
            if (document.body.classList.contains('matrix-active') || document.body.classList.contains('cat-active')) e.preventDefault();
        }, { passive: false });
        connect();
    </script>
</body>
</html>
)rawliteral";

// ========== 核心命令执行函数 ==========
void executeCommand(String cmd, String& output, String& newPrompt, bool& clearTerminal, String& dlFileName, String& dlContent, bool& triggerUpload) {
  output = "";
  newPrompt = "root@esp32:" + currentPath + "# ";
  clearTerminal = false;
  dlFileName = "";
  dlContent = "";
  triggerUpload = false;
  g_lastExitCode = 0;

  if (isEditing) {
    newPrompt = "";
    if (cmd == "EOF") {
      isEditing = false;
      newPrompt = "root@esp32:" + currentPath + "# ";
      output = "\nedit: File saved: " + editingFilePath + "\n";
      editingFilePath = "";
      return;
    }
    if (!canWritePath(editingFilePath)) {
      output = "edit: Permission denied: '" + editingFilePath + "' is protected\n";
      isEditing = false;
      newPrompt = "root@esp32:" + currentPath + "# ";
      editingFilePath = "";
      return;
    }
    File f = LittleFS.open(editingFilePath, "a");
    if (f) {
      f.println(cmd);
      f.close();
      output = "";
    } else {
      output = "edit: Error: Failed to write file!\n";
      isEditing = false;
      newPrompt = "root@esp32:" + currentPath + "# ";
      editingFilePath = "";
    }
    return;
  }

  if (cmd.startsWith("__SYNC_TIME__:")) {
    setSystemTime(cmd.substring(14).toInt());
    output = "time: Browser time applied to ESP32 RTC.\n";
    return;
  }

  // ========== llama 命令处理 ==========
  if (cmd == "llama init" || cmd.startsWith("llama init ")) {
    String modelPath = LLM_MODEL_PATH;
    String tokenPath = LLM_TOKEN_PATH;
    if (cmd.startsWith("llama init ")) {
      String args = cmd.substring(10);
      args.trim();
      int spacePos = args.indexOf(' ');
      if (spacePos == -1) {
        output = "llama: Error: init requires 2 arguments\n";
        output += "llama: Usage: llama init <model.bin> <tokens.bin>\n";
        output += "llama: Example: llama init models/my.bin tokens/tok.bin\n";
        output += "llama: Or use 'llama init' for default /bin/ models\n";
        return;
      }
      modelPath = args.substring(0, spacePos);
      tokenPath = args.substring(spacePos + 1);
      modelPath.trim();
      tokenPath.trim();
      if (!modelPath.startsWith("/")) modelPath = resolvePath(modelPath);
      if (!tokenPath.startsWith("/")) tokenPath = resolvePath(tokenPath);
    }
    if (!LittleFS.exists(modelPath)) {
      output = "llama: Model not found: " + modelPath + "\n";
      return;
    }
    if (!LittleFS.exists(tokenPath)) {
      output = "llama: Token file not found: " + tokenPath + "\n";
      return;
    }
    if (llm_bridge_init(modelPath.c_str(), tokenPath.c_str())) {
      output = "llama: Model loaded: " + modelPath + "\n";
      output += "llama: Tokens: " + tokenPath + "\n";
    } else {
      output = "llama: Init failed. Check serial logs.\n";
    }
    return;
  }
  if (cmd == "llama status") {
    if (!llm_bridge_is_ready()) output = "llama: Status: Not initialized\n";
    else if (llm_bridge_is_busy()) output = "llama: Status: Generating...\n";
    else output = "llama: Status: Ready\n";
    return;
  }
  if (cmd == "llama free") {
    if (!llm_bridge_is_ready()) {
      output = "llama: Not initialized\n";
      return;
    }
    llm_bridge_free();
    output = "llama: Model unloaded. PSRAM freed.\n";
    return;
  }
  if (cmd.startsWith("llama ")) {
    String args = cmd.substring(6);
    args.trim();
    if (!llm_bridge_is_ready()) {
      output = "llama: Run 'llama init' first\n";
      return;
    }
    if (llm_bridge_is_busy()) {
      output = "llama: Busy generating.\n";
      return;
    }
    String prompt = "";
    bool hasQuotes = false;
    int promptEnd = -1;
    if (args.length() >= 2) {
      if (args.startsWith("\"")) {
        for (int i = 1; i < args.length(); i++) {
          if (args[i] == '"' && (i == 1 || args[i - 1] != '\\')) {
            prompt = args.substring(1, i);
            promptEnd = i + 1;
            hasQuotes = true;
            break;
          }
        }
      } else if (args.startsWith("'")) {
        for (int i = 1; i < args.length(); i++) {
          if (args[i] == '\'' && (i == 1 || args[i - 1] != '\\')) {
            prompt = args.substring(1, i);
            promptEnd = i + 1;
            hasQuotes = true;
            break;
          }
        }
      }
    }
    if (!hasQuotes) {
      output = "llama: Error: Prompt must be enclosed in quotes\n";
      output += "llama: Usage: llama \"<your prompt>\" [-l N]\n";
      output += "llama: Examples:\n";
      output += "* llama \"Hello world\"\n";
      output += "* llama 'Ask me anything' -l 100\n";
      output += "* llama -l 50 \"Short reply\"\n";
      output += "llama: Note: Prompt MUST be enclosed in quotes\n";
      return;
    }
    int max_len = 256;
    if (promptEnd >= 0 && promptEnd < args.length()) {
      String rest = args.substring(promptEnd);
      rest.trim();
      int lPos = rest.indexOf("-l ");
      if (lPos == -1) lPos = rest.indexOf("-l");
      if (lPos != -1) {
        String numPart = rest.substring(lPos + 2);
        numPart.trim();
        String numStr = "";
        for (int i = 0; i < numPart.length(); i++) {
          if (isdigit(numPart[i]) || (numPart[i] == '-' && i == 0)) numStr += numPart[i];
          else break;
        }
        int parsed = numStr.toInt();
        if (parsed > 0 && parsed <= 2048) max_len = parsed;
      }
    }
    if (!prompt.isEmpty() && prompt.charAt(prompt.length() - 1) != ' ') prompt += " ";
    if (prompt.isEmpty()) {
      output = "llama: Error: Empty prompt\n";
      return;
    }
    llm_bridge_generate(prompt.c_str(), max_len, on_llm_token, on_llm_done, &llmActiveClient);
    return;
  }

  if (cmd == "help") {
    output += "SHELL BUILTINS & NAVIGATION\n----------------------------\n";
    output += "  help                      - Show this help message\n";
    output += "  pwd                       - Print current working directory path\n";
    output += "  cd <path>                 - Change current working directory to target path\n";
    output += "  ls [-l]                   - List directory contents (-l for detailed view)\n\n";
    output += "FILE SYSTEM COMMANDS\n--------------------\n";
    output += "  touch <file>              - Create a new empty file\n";
    output += "  mkdir <dir>               - Create a new empty directory\n";
    output += "  cat <file>                - Display the full content of a file\n";
    output += "  edit <file>               - Edit a file (input EOF to save and exit)\n";
    output += "  cp <src> <dst>            - Copy source file/directory to destination\n";
    output += "  mv <src> <dst>            - Move or rename a file/directory\n";
    output += "  rm [-r] <path>            - Delete file (add -r to delete directory recursively)\n";
    output += "  grep <pattern> <file>     - Search for matching text pattern in a file\n";
    output += "  echo <txt> [> file]       - Print text (redirect to file with > symbol)\n\n";
    output += "SCRIPTING SYNTAX\n----------------\n";
    output += "  run <script> [input]      - Execute script file (optional input data file)\n";
    output += "  * read <var>              - Read input line and store into variable $var\n";
    output += "  * set var=value           - Define a custom shell variable\n";
    output += "  * set var=expr            - Calculate arithmetic expression (+-*/%) and assign\n";
    output += "  * if cond then ... fi     - Basic conditional execution block\n";
    output += "  * if cond then ... else ... fi - Conditional execution with else branch\n";
    output += "  * for i in 1..5 do ... done - Numeric loop (iterate from 1 to 5)\n";
    output += "  * while cond do ... done  - Loop while the condition is true\n";
    output += "  * break                   - Exit the current loop immediately\n";
    output += "  * continue                - Skip to next loop iteration\n";
    output += "  * sleep <sec>|<ms>ms      - Delay execution (seconds or milliseconds)\n";
    output += "  * $?                      - Get exit code of last command (0 = success)\n";
    output += "  * # comment               - Single line comment (ignored by shell)\n\n";
    output += "SYSTEM INFO\n-------------\n";
    output += "  uname                     - Show system name and version information\n";
    output += "  cpuinfo                   - Display CPU hardware details\n";
    output += "  free                      - Show system memory usage statistics\n";
    output += "  df                        - Show disk partition space usage\n";
    output += "  sysinfo                   - Show comprehensive system information\n";
    output += "  date [-s]                 - Show/set current system date and time\n";
    output += "  reset                     - Reset terminal to default state\n\n";
    output += "WIFI MANAGEMENT (Safe)\n------------------------\n";
    output += "  wifi [status]             - Show WiFi status summary\n";
    output += "  wifi info                 - Display detailed config\n";
    output += "  wifi scan                 - Scan surrounding networks\n";
    output += "  wifi stats                - Signal quality & rates\n";
    output += "  wifi set power <dBm>      - Adjust TX power (2~20)\n\n";
    output += "LLM / AI COMMANDS\n-------------------\n";
    output += "  llama init                - Load the LLM model from /bin/\n";
    output += "  llama init <m.bin> <t.bin> - Load custom model (relative paths)\n";
    output += "  llama \"<prompt>\" [-l N] - Generate (prompt MUST be quoted)\n";
    output += "  llama 'Ask me'            - Single quotes also work\n";
    output += "  llama status              - Show LLM model status\n";
    output += "  llama free                - Unload model and free PSRAM\n";
    output += "  * Protected: /bin/ is read-only\n\n";
    output += "HARDWARE CONTROL\n------------------\n";
    output += "  gpio [-s <pin> <level>]   - View GPIO status (set pin level with -s)\n";
    output += "  adc [pin]                 - Read ADC1 value (default: GPIO4)\n";
    output += "  * Safe GPIO for this chip: " + getSafeGpioList() + "\n";
    output += "  * ADC1 pins: " + getAdc1ValidPins() + "\n\n";
    output += "FILE TRANSFER & FUN\n---------------------\n";
    output += "  dl <file>                 - Download file (supports chunked transfer up to 5MB)\n";
    output += "  ul                        - Upload file (supports chunked transfer up to 5MB)\n";
    output += "  clear                     - Clear all content in the terminal screen\n";
    output += "  matrix                    - Enter the digital rain 🌧️\n";
    output += "  fortune                   - Ask the silicon oracle for wisdom 🎱\n";
    output += "  cat                       - Try it without parameters! 🐱\n";
    return;
  }

  if (cmd == "dl" || cmd.startsWith("dl ")) {
    String pathArg = (cmd == "dl") ? "" : cmd.substring(3);
    pathArg.trim();
    if (pathArg.isEmpty()) {
      output = "dl: Usage: dl <file>\n";
      return;
    }
    String fullPath = resolvePath(pathArg);
    if (!LittleFS.exists(fullPath)) {
      output = "dl: " + pathArg + ": No such file\n";
      return;
    }
    File f = LittleFS.open(fullPath, "r");
    if (f.isDirectory()) {
      output = "dl: " + pathArg + ": Is a directory\n";
      f.close();
      return;
    }
    f.close();
    if (!startChunkedDownload(fullPath, llmActiveClient)) { output = "dl: Failed to start transfer\n"; }
    return;
  }

  if (cmd == "ul") {
    triggerUpload = true;
    output = "ul: Please select a file via browser popup...\n";
    return;
  }

  if (cmd == "run" || cmd.startsWith("run ")) {
    String args = (cmd == "run") ? "" : cmd.substring(4);
    args.trim();
    int spacePos = args.indexOf(" ");
    String scriptPath = args, inputPath = "";
    if (spacePos != -1) {
      scriptPath = args.substring(0, spacePos);
      inputPath = args.substring(spacePos + 1);
      scriptPath.trim();
      inputPath.trim();
    }
    if (scriptPath.isEmpty()) {
      output = "run: Usage: run <script> [input_file]\n";
      return;
    }
    String fullPath = resolvePath(scriptPath);
    if (!LittleFS.exists(fullPath)) {
      output = "run: File not found: " + scriptPath + "\n";
      return;
    }
    File f = LittleFS.open(fullPath, "r");
    if (!f || f.isDirectory()) {
      output = "run: Not a valid file\n";
      f.close();
      return;
    }
    if (!inputPath.isEmpty() && !loadScriptInput(resolvePath(inputPath))) output += "run: Warning: Failed to load input file\n";
    else {
      scriptInputLines.clear();
      scriptInputIndex = 0;
    }
    scriptVars.clear();
    output += "---------------------------------\nrun: Executing: " + fullPath + "\n";
    if (!inputPath.isEmpty()) output += "run: With input: " + inputPath + "\n";
    output += "---------------------------------\n";
    std::vector<String> lines;
    String line;
    while (f.available()) {
      char c = f.read();
      if (c == '\n') {
        lines.push_back(line);
        line = "";
      } else if (c != '\r') line += c;
    }
    if (!line.isEmpty()) lines.push_back(line);
    f.close();
    executeScriptBlockEx(lines, 0, lines.size(), output);
    output += "---------------------------------\nrun: Completed!\n---------------------------------\n";
    return;
  }

  if (cmd == "uname") {
    output = "ESP32-QINUX v1.0\nKernel: LittleFS on ESP32\nArchitecture: Xtensa LX7\n";
    return;
  }
  if (cmd == "clear") {
    clearTerminal = true;
    return;
  }
  if (cmd == "pwd") {
    output = currentPath + "\n";
    return;
  }

  if (cmd == "ls" || cmd.startsWith("ls ")) {
    bool longFormat = false;
    String pathArg = "";
    if (cmd == "ls -l") {
      longFormat = true;
      pathArg = currentPath;
    } else if (cmd.startsWith("ls -l ")) {
      longFormat = true;
      pathArg = cmd.substring(5);
      pathArg.trim();
    } else if (cmd.startsWith("ls ")) {
      pathArg = cmd.substring(3);
      pathArg.trim();
    }
    if (pathArg.isEmpty()) pathArg = currentPath;
    String targetPath = resolvePath(pathArg);
    File root = LittleFS.open(targetPath, "r");
    if (!root) {
      output = "ls: cannot access '" + pathArg + "': No such file or directory\n";
      return;
    }
    if (!root.isDirectory()) {
      String fileName = targetPath.substring(targetPath.lastIndexOf('/') + 1);
      if (fileName.isEmpty()) fileName = targetPath;
      if (longFormat) output += "- " + String(root.size(), 6) + "B " + fileName + "\n";
      else output += fileName + "\n";
      root.close();
      return;
    }
    File file = root.openNextFile();
    while (file) {
      String fileName = file.name();
      int lastSlash = fileName.lastIndexOf('/');
      if (lastSlash != -1) fileName = fileName.substring(lastSlash + 1);
      if (longFormat) {
        if (file.isDirectory()) output += "d  <DIR> " + fileName + "/\n";
        else output += "- " + String(file.size(), 6) + "B " + fileName + "\n";
      } else {
        output += fileName;
        if (file.isDirectory()) output += "/";
        output += "\n";
      }
      file.close();
      file = root.openNextFile();
      yield();
    }
    root.close();
    if (isProtectedPath(targetPath)) output += "[!] " + targetPath + " is read-only (protected)\n";
    return;
  }

  if (cmd == "cd" || cmd.startsWith("cd ")) {
    String pathArg = (cmd == "cd") ? "" : cmd.substring(3);
    pathArg.trim();
    if (pathArg.isEmpty()) {
      output = "cd: Usage: cd <path>\n";
      return;
    }
    String newPath = resolvePath(pathArg);
    if (LittleFS.exists(newPath)) {
      File f = LittleFS.open(newPath);
      if (f.isDirectory()) {
        currentPath = newPath;
        newPrompt = "root@esp32:" + currentPath + "# ";
      } else output = "cd: " + pathArg + ": Not a directory\n";
      f.close();
    } else output = "cd: " + pathArg + ": No such file or directory\n";
    return;
  }

  if (cmd == "cat" || cmd.startsWith("cat ")) {
    if (cmd == "cat") {
      output = "__CAT_EASTER_EGG__";
      return;
    }
    String pathArg = cmd.substring(4);
    pathArg.trim();
    if (pathArg.isEmpty()) {
      output = "cat: Usage: cat <file>\n";
      return;
    }
    String fullPath = resolvePath(pathArg);
    if (LittleFS.exists(fullPath)) {
      File f = LittleFS.open(fullPath, "r");
      if (f && !f.isDirectory()) {
        while (f.available()) output += (char)f.read();
        output += "\n";
      } else output = "cat: " + pathArg + ": Is a directory\n";
      f.close();
    } else output = "cat: " + pathArg + ": No such file\n";
    return;
  }

  if (cmd == "edit" || cmd.startsWith("edit ")) {
    String pathArg = (cmd == "edit") ? "" : cmd.substring(5);
    pathArg.trim();
    if (pathArg.isEmpty()) {
      output = "edit: Usage: edit <file>\n";
      return;
    }
    if (!canWritePath(resolvePath(pathArg))) {
      output = "edit: Permission denied: '" + pathArg + "' is read-only\n";
      return;
    }
    editingFilePath = resolvePath(pathArg);
    File f = LittleFS.open(editingFilePath, "w");
    if (!f) {
      output = "edit: Cannot create file\n";
      editingFilePath = "";
      return;
    }
    f.close();
    isEditing = true;
    newPrompt = "";
    output = "---------------------------------\nedit: " + editingFilePath + "\nedit: Type 'EOF' to save & exit\n---------------------------------\n";
    return;
  }

  if (cmd == "mkdir" || cmd.startsWith("mkdir ")) {
    String pathArg = (cmd == "mkdir") ? "" : cmd.substring(6);
    pathArg.trim();
    if (pathArg.isEmpty()) {
      output = "mkdir: Usage: mkdir <dir>\n";
      return;
    }
    if (!canWritePath(resolvePath(pathArg))) {
      output = "mkdir: Permission denied: protected directory\n";
      return;
    }
    if (!LittleFS.mkdir(resolvePath(pathArg))) output = "mkdir: cannot create '" + pathArg + "'\n";
    return;
  }

  if (cmd == "touch" || cmd.startsWith("touch ")) {
    String pathArg = (cmd == "touch") ? "" : cmd.substring(6);
    pathArg.trim();
    if (pathArg.isEmpty()) {
      output = "touch: Usage: touch <file>\n";
      return;
    }
    String fullPath = resolvePath(pathArg);
    if (!canWritePath(fullPath)) {
      output = "touch: Permission denied: protected directory\n";
      return;
    }
    if (LittleFS.exists(fullPath)) {
      File f = LittleFS.open(fullPath);
      if (f.isDirectory()) output = "touch: " + pathArg + ": Is a directory\n";
      else output = "touch: " + pathArg + ": File exists\n";
      f.close();
    } else {
      File f = LittleFS.open(fullPath, "w");
      if (f) f.close();
    }
    return;
  }

  if (cmd == "rm" || cmd.startsWith("rm ")) {
    String args = (cmd == "rm") ? "" : cmd.substring(3);
    args.trim();
    if (args.isEmpty()) {
      output = "rm: Usage: rm <path>\n       rm -r <directory>\n       rm [-r] * (remove all in current dir)\n";
      return;
    }
    bool recursive = false;
    String pathArg = args;
    if (args.startsWith("-")) {
      int firstSpace = args.indexOf(' ');
      String flags = (firstSpace == -1) ? args : args.substring(0, firstSpace);
      pathArg = (firstSpace == -1) ? "" : args.substring(firstSpace + 1);
      pathArg.trim();
      if (flags.indexOf('r') != -1) recursive = true;
    }
    if (pathArg == "*" || pathArg == "./*") {
      File dir = LittleFS.open(currentPath, "r");
      if (!dir || !dir.isDirectory()) {
        output = "rm: cannot access '" + currentPath + "'\n";
        if (dir) dir.close();
        g_lastExitCode = 1;
        return;
      }
      File item = dir.openNextFile();
      while (item) {
        String itemName = item.name();
        int lastSlash = itemName.lastIndexOf('/');
        if (lastSlash != -1) itemName = itemName.substring(lastSlash + 1);
        String itemPath = joinPath(currentPath, itemName);
        if (isProtectedPath(itemPath)) {
          item.close();
          item = dir.openNextFile();
          continue;
        }
        bool isDir = item.isDirectory();
        String displayName = itemName + (isDir ? "/" : "");
        item.close();
        if (isDir) {
          if (recursive) {
            if (deleteDirectoryRecursive(itemPath)) output += "rm: removed '" + displayName + "'\n";
            else {
              output += "rm: cannot remove '" + displayName + "': Partial failure\n";
              g_lastExitCode = 1;
            }
          } else {
            output += "rm: " + displayName + "Is a directory (use -r)\n";
            g_lastExitCode = 1;
          }
        } else {
          if (LittleFS.remove(itemPath)) output += "rm: removed '" + itemName + "'\n";
          else {
            output += "rm: cannot remove '" + itemName + "'\n";
            g_lastExitCode = 1;
          }
        }
        item = dir.openNextFile();
        yield();
      }
      dir.close();
      return;
    }
    if (pathArg.isEmpty()) {
      output = "rm: Usage: rm [-r] <path>\n";
      return;
    }
    String fullPath = resolvePath(pathArg);
    if (isProtectedPath(fullPath)) {
      output = "rm: Permission denied: '" + pathArg + "' is protected\n";
      g_lastExitCode = 1;
      return;
    }
    if (!LittleFS.exists(fullPath)) {
      output = "rm: " + pathArg + ": No such file\n";
      g_lastExitCode = 1;
      return;
    }
    File target = LittleFS.open(fullPath, "r");
    if (!target) {
      output = "rm: cannot access '" + pathArg + "'\n";
      g_lastExitCode = 1;
      return;
    }
    bool isDir = target.isDirectory();
    target.close();
    if (isDir) {
      if (!recursive) {
        File testDir = LittleFS.open(fullPath, "r");
        File test = testDir.openNextFile();
        bool isEmpty = (test ? false : true);
        if (test) test.close();
        testDir.close();
        if (!isEmpty) {
          output = "rm: " + pathArg + ": Is a directory (use -r)\n";
          g_lastExitCode = 1;
          return;
        }
        if (LittleFS.rmdir(fullPath)) output = "rm: removed directory '" + pathArg + "'\n";
        else {
          output = "rm: cannot remove '" + pathArg + "'\n";
          g_lastExitCode = 1;
        }
      } else {
        if (deleteDirectoryRecursive(fullPath)) output = "rm: recursively removed '" + pathArg + "'\n";
        else {
          output = "rm: cannot remove '" + pathArg + "': Partial failure\n";
          g_lastExitCode = 1;
        }
      }
    } else {
      if (LittleFS.remove(fullPath)) output = "rm: removed '" + pathArg + "'\n";
      else {
        output = "rm: cannot remove '" + pathArg + "'\n";
        g_lastExitCode = 1;
      }
    }
    return;
  }

  if (cmd == "cp" || cmd.startsWith("cp ")) {
    String args = (cmd == "cp") ? "" : cmd.substring(3);
    args.trim();
    int spacePos = args.indexOf(" ");
    if (spacePos == -1) {
      output = "cp: Usage: cp <src> <dst>\n";
      return;
    }
    String srcPath = args.substring(0, spacePos), dstPath = args.substring(spacePos + 1);
    srcPath.trim();
    dstPath.trim();
    String fullSrc = resolvePath(srcPath), fullDst = resolvePath(dstPath);
    if (isProtectedPath(fullSrc) || isProtectedPath(fullDst)) {
      output = "cp: Permission denied: protected path\n";
      g_lastExitCode = 1;
      return;
    }
    if (!LittleFS.exists(fullSrc)) {
      output = "cp: source not found\n";
      return;
    }
    File srcFile = LittleFS.open(fullSrc, "r");
    if (!srcFile || srcFile.isDirectory()) {
      output = "cp: not a file\n";
      srcFile.close();
      return;
    }
    if (LittleFS.exists(fullDst)) {
      File dstTest = LittleFS.open(fullDst);
      if (dstTest.isDirectory()) { fullDst = joinPath(fullDst, fullSrc.substring(fullSrc.lastIndexOf('/') + 1)); }
      dstTest.close();
    }
    File dstFile = LittleFS.open(fullDst, "w");
    if (!dstFile) {
      output = "cp: create failed\n";
      srcFile.close();
      return;
    }
    while (srcFile.available()) dstFile.write(srcFile.read());
    srcFile.close();
    dstFile.close();
    output = "cp: " + fullSrc + " -> " + fullDst + "\n";
    return;
  }

  if (cmd == "mv" || cmd.startsWith("mv ")) {
    String args = (cmd == "mv") ? "" : cmd.substring(3);
    args.trim();
    int spacePos = args.indexOf(" ");
    if (spacePos == -1) {
      output = "mv: Usage: mv <src> <dst>\n";
      return;
    }
    String srcPath = args.substring(0, spacePos), dstPath = args.substring(spacePos + 1);
    srcPath.trim();
    dstPath.trim();
    String fullSrc = resolvePath(srcPath), fullDst = resolvePath(dstPath);
    if (isProtectedPath(fullSrc) || isProtectedPath(fullDst)) {
      output = "mv: Permission denied: protected path\n";
      g_lastExitCode = 1;
      return;
    }
    if (!LittleFS.exists(fullSrc)) {
      output = "mv: source not found\n";
      return;
    }
    if (LittleFS.exists(fullDst)) {
      File dstTest = LittleFS.open(fullDst);
      if (dstTest.isDirectory()) { fullDst = joinPath(fullDst, fullSrc.substring(fullSrc.lastIndexOf('/') + 1)); }
      dstTest.close();
    }
    if (LittleFS.rename(fullSrc, fullDst)) output = "mv: " + fullSrc + " -> " + fullDst + "\n";
    else {
      output = "mv: failed\n";
      g_lastExitCode = 1;
    }
    return;
  }

  if (cmd == "echo" || cmd.startsWith("echo ")) {
    if (cmd == "echo") {
      output = "echo: Usage: echo <text> [> file] [>> file]\n";
      return;
    }
    String rest = cmd.substring(5);
    rest.trim();
    int gtPos = -1, gtgtPos = -1;
    bool inQuote = false;
    char quoteChar = 0;
    for (int i = 0; i < rest.length(); i++) {
      char c = rest[i];
      if ((c == '"' || c == '\'') && (i == 0 || rest[i - 1] != '\\')) {
        if (!inQuote) {
          inQuote = true;
          quoteChar = c;
        } else if (c == quoteChar) inQuote = false;
      } else if (!inQuote) {
        if (c == '>' && i + 1 < rest.length() && rest[i + 1] == '>') {
          gtgtPos = i;
          break;
        } else if (c == '>' && gtPos == -1) gtPos = i;
      }
    }
    String text = "", filePath = "";
    bool append = false;
    if (gtgtPos != -1) {
      text = rest.substring(0, gtgtPos);
      filePath = rest.substring(gtgtPos + 2);
      append = true;
    } else if (gtPos != -1) {
      text = rest.substring(0, gtPos);
      filePath = rest.substring(gtPos + 1);
      append = false;
    } else text = rest;
    text.trim();
    filePath.trim();
    if (text.length() >= 2 && ((text.startsWith("\"") && text.endsWith("\"")) || (text.startsWith("'") && text.endsWith("'")))) text = text.substring(1, text.length() - 1);
    if (!filePath.isEmpty()) {
      if (!canWritePath(resolvePath(filePath))) {
        output = "echo: Permission denied: '" + filePath + "' is read-only\n";
        g_lastExitCode = 1;
        return;
      }
      File f = LittleFS.open(resolvePath(filePath), append ? "a" : "w");
      if (f) {
        f.print(text);
        f.close();
      } else {
        output = "echo: write error\n";
        g_lastExitCode = 1;
      }
    } else output = text + "\n";
    return;
  }

  if (cmd == "grep" || cmd.startsWith("grep ")) {
    String rest = (cmd == "grep") ? "" : cmd.substring(5);
    rest.trim();
    int spaceIdx = rest.indexOf(" ");
    if (spaceIdx == -1) {
      output = "grep: Usage: grep <pattern> <file>\n";
      return;
    }
    String pattern = rest.substring(0, spaceIdx), fileArg = rest.substring(spaceIdx + 1);
    fileArg.trim();
    String fullPath = resolvePath(fileArg);
    if (LittleFS.exists(fullPath)) {
      File f = LittleFS.open(fullPath, "r");
      if (f && !f.isDirectory()) {
        int lineNum = 0;
        String line;
        while (f.available()) {
          char c = f.read();
          if (c == '\n') {
            if (line.indexOf(pattern) != -1) output += String(lineNum) + ":" + line + "\n";
            line = "";
            lineNum++;
          } else line += c;
        }
        if (!line.isEmpty() && line.indexOf(pattern) != -1) output += String(lineNum + 1) + ":" + line + "\n";
        f.close();
      }
    } else {
      output = "grep: " + fileArg + ": No such file\n";
      g_lastExitCode = 1;
    }
    return;
  }

  if (cmd == "cpuinfo") {
    output += "cpuinfo: Chip Model:    " + String(ESP.getChipModel()) + "\n";
    output += "cpuinfo: Chip Revision: " + String(ESP.getChipRevision()) + "\n";
    output += "cpuinfo: Cores:         " + String(ESP.getChipCores()) + "\n";
    output += "cpuinfo: CPU Freq:      " + String(ESP.getCpuFreqMHz()) + " MHz\n";
    output += "cpuinfo: SDK Version:   " + String(ESP.getSdkVersion()) + "\n";
    output += "cpuinfo: Chip ID:       " + String((uint32_t)ESP.getEfuseMac(), HEX) + "\n";
    return;
  }

  if (cmd == "free") {
    uint32_t heapTotal = ESP.getHeapSize(), heapFree = ESP.getFreeHeap(), heapUsed = heapTotal - heapFree;
    output += "free: [Heap Memory]\n";
    output += "free: Total:     " + String(heapTotal / 1024) + " KB\n";
    output += "free: Used:      " + String(heapUsed / 1024) + " KB\n";
    output += "free: Free:      " + String(heapFree / 1024) + " KB\n";
    if (ESP.getPsramSize() > 0) {
      uint32_t psramTotal = ESP.getPsramSize(), psramFree = ESP.getFreePsram();
      output += "free: [PSRAM]\n";
      output += "free: Total:     " + String(psramTotal / 1024) + " KB\n";
      output += "free: Free:      " + String(psramFree / 1024) + " KB\n";
    }
    return;
  }

  if (cmd == "df") {
    uint64_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    output += "df: File System: LittleFS\n";
    output += "df: Total Size:  " + String(total / 1024) + " KB\n";
    output += "df: Used:        " + String(used / 1024) + " KB\n";
    output += "df: Free:        " + String((total - used) / 1024) + " KB\n";
    return;
  }

  if (cmd == "sysinfo") {
    unsigned long uptimeSec = millis() / 1000;
    output += "sysinfo: Uptime:      " + String(uptimeSec / 86400) + "d " + String((uptimeSec % 86400) / 3600) + "h\n";
    output += "sysinfo: Chip:        " + String(ESP.getChipModel()) + "\n";
    output += "sysinfo: Heap Free:   " + String(ESP.getFreeHeap() / 1024) + " KB\n";
    output += "sysinfo: WiFi IP:     " + WiFi.softAPIP().toString() + "\n";
    return;
  }

  if (cmd == "date" || cmd.startsWith("date -s")) {
    if (cmd == "date") {
      time_t now;
      time(&now);
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);
      char timeStr[64];
      strftime(timeStr, sizeof(timeStr), "%c", &timeinfo);
      output += "date: " + String(timeStr) + "\n";
    } else {
      String args = cmd.substring(7);
      args.trim();
      if (args.isEmpty()) {
        output = "date: Usage: date -s \"YYYY-MM-DD HH:MM:SS\"\n";
      } else {
        String dateStr = args;
        if ((dateStr.startsWith("\"") && dateStr.endsWith("\"")) || (dateStr.startsWith("'") && dateStr.endsWith("'"))) dateStr = dateStr.substring(1, dateStr.length() - 1);
        time_t newTime;
        if (parseDateTimeString(dateStr, newTime)) {
          setSystemTime(newTime);
          output = "date: Date/Time set to: " + dateStr + "\n";
        } else output = "date: Usage: date -s \"YYYY-MM-DD HH:MM:SS\"\n";
      }
    }
    return;
  }

  if (cmd == "reset") {
    output += "reset: Last Reset: " + getResetReason() + "\n";
    return;
  }

  if (cmd == "gpio") {
    output += "gpio: Pin | Level\ngpio: ----+------\n";
    for (int pin = 0; pin <= 48; pin++) {
      if (isSafeGpio(pin)) { output += "gpio: " + String(pin < 10 ? "  " : " ") + String(pin) + " | " + String(digitalRead(pin)) + "\n"; }
    }
    return;
  }

  if (cmd == "gpio -s" || cmd.startsWith("gpio -s ")) {
    String args = (cmd == "gpio -s") ? "" : cmd.substring(7);
    args.trim();
    int space = args.indexOf(' ');
    if (space == -1) {
      output = "gpio: Usage: gpio -s <pin> <level>\n";
      return;
    }
    int pin = args.substring(0, space).toInt(), level = args.substring(space + 1).toInt();
    if (!isSafeGpio(pin)) {
      output = "gpio: Error: GPIO not safe!\n";
      return;
    }
    if (level != 0 && level != 1) {
      output = "gpio: Error: Level must be 0 or 1!\n";
      return;
    }
    pinMode(pin, OUTPUT);
    digitalWrite(pin, level);
    output = "gpio: GPIO" + String(pin) + " set to " + String(level) + "\n";
    return;
  }

  if (cmd == "adc" || cmd.startsWith("adc ")) {
    int adcPin = 4;
    if (cmd.startsWith("adc ")) {
      String arg = cmd.substring(4);
      arg.trim();
      if (!arg.isEmpty()) adcPin = arg.toInt();
    }
    if (!isAdc1Pin(adcPin)) {
      output += "adc: GPIO" + String(adcPin) + " is not a valid ADC1 pin\n";
      output += "adc: Valid ADC1 pins for this chip: " + getAdc1ValidPins() + "\n";
      output += "adc: Note: ADC2 pins not supported (WiFi conflict)\n";
      g_lastExitCode = 1;
    } else {
      analogReadResolution(12);
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3)
      analogSetAttenuation(ADC_11db);
#endif
      int raw = analogRead(adcPin);
      output += "adc: GPIO" + String(adcPin) + ": " + String(raw) + " / " + String((raw / 4095.0) * 3.3, 2) + "V\n";
    }
    return;
  }

  if (cmd == "wifi" || cmd.startsWith("wifi ")) {
    String args = (cmd == "wifi") ? "" : cmd.substring(5);
    args.trim();
    handleWifiCommand(args, output);
    return;
  }

  if (cmd == "fortune") {
    randomSeed(ESP.getEfuseMac() ^ millis());
    output = FORTUNES[random(FORTUNE_COUNT)] + String("\n");
    return;
  }
  if (cmd == "matrix") {
    output = "__MATRIX_MODE__";
    return;
  }

  if (!cmd.isEmpty()) {
    output = "Command not found: " + cmd + "\nType 'help' for available commands.\n";
    g_lastExitCode = 1;
  }
}

// ========== WebSocket事件处理 ==========
void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type != WStype_TEXT) return;
  llmActiveClient = num;
  String msg = (char*)payload;

  if (msg.startsWith("__DL_ACK__:")) {
    if (chunkState.active && !chunkState.isUpload && !chunkState.sending) { sendNextChunk(); }
    return;
  }

  if (msg.startsWith("__UPLOAD_START__:")) {
    String args = msg.substring(17);
    String fname = "";
    size_t fsize = 0;
    size_t cChunkSize = CHUNK_SIZE;
    int p1 = args.indexOf("filename:");
    int p2 = args.indexOf(",size:");
    int p3 = args.indexOf(",chunk_size:");
    if (p1 != -1 && p2 != -1) {
      fname = args.substring(p1 + 9, p2);
      fsize = args.substring(p2 + 6, (p3 != -1) ? p3 : args.length()).toInt();
      if (p3 != -1) cChunkSize = args.substring(p3 + 12).toInt();
    }
    if (cChunkSize == 0) cChunkSize = CHUNK_SIZE;
    handleUploadStart(fname, fsize, cChunkSize, num);
    return;
  }

  if (msg.startsWith("__UPLOAD_CHUNK__:")) {
    String args = msg.substring(17);
    int idxPos = args.indexOf("index:");
    int dataPos = args.indexOf(",");
    if (idxPos != -1 && dataPos != -1) {
      size_t idx = args.substring(idxPos + 6, dataPos).toInt();
      String b64 = args.substring(dataPos + 1);
      handleUploadChunk(idx, b64, num);
    }
    return;
  }

  if (msg == "__UPLOAD_END__") {
    if (chunkState.active && chunkState.isUpload) {
      chunkState.file.flush();
      chunkState.file.close();
      chunkState.active = false;
    }
    return;
  }

  if (llmGenerationActive && !msg.startsWith("llama status")) {
    String resp = "{\"output\":\"llama: Busy generating.\\n\",";
    resp += "\"prompt\":\"root@esp32:" + currentPath + "# \"}";
    webSocket.sendTXT(num, resp);
    return;
  }

  llmActiveClient = num;
  String output, newPrompt, dlFileName, dlContent;
  bool clearTerminal, triggerUpload;

  if (msg.startsWith("__UPLOAD__:")) {
    const int PREFIX_LEN = 11;
    int firstSep = msg.indexOf(':', PREFIX_LEN);
    if (firstSep != -1) {
      String filename = msg.substring(PREFIX_LEN, firstSep);
      int lastSlash = filename.lastIndexOf('/'), lastBackslash = filename.lastIndexOf('\\');
      int lastPos = max(lastSlash, lastBackslash);
      if (lastPos != -1) filename = filename.substring(lastPos + 1);
      filename.replace("/", "");
      filename.replace("\\", "");
      filename.replace("..", "");
      if (filename.isEmpty()) filename = "upload_" + String(millis()) + ".bin";
      String fullPath = resolvePath(filename);
      if (!canWritePath(fullPath)) {
        String resp = "{\"prompt\":\"root@esp32:" + currentPath + "# \",\"output\":\"ul: Permission denied: protected directory\\n\"}";
        webSocket.sendTXT(num, resp);
        return;
      }
      String b64data = msg.substring(firstSep + 1);
      std::vector<uint8_t> binData = base64Decode(b64data);
      int lastSlashPos = fullPath.lastIndexOf('/');
      if (lastSlashPos > 0 && !LittleFS.exists(fullPath.substring(0, lastSlashPos))) LittleFS.mkdir(fullPath.substring(0, lastSlashPos));
      File f = LittleFS.open(fullPath, "w");
      if (f && !f.isDirectory()) {
        size_t written = f.write(binData.data(), binData.size());
        f.close();
        output = (written == binData.size()) ? "ul: Saved '" + filename + "' (" + String(written) + " bytes)\n" : "ul: Write incomplete: " + String(written) + "/" + String(binData.size()) + "\n";
      } else {
        if (f) f.close();
        output = "ul: Failed to open: " + fullPath + "\n";
      }
      String resp = "{\"prompt\":\"root@esp32:" + currentPath + "# \",\"output\":\"" + escapeJson(output) + "\"}";
      webSocket.sendTXT(num, resp);
      return;
    }
    webSocket.sendTXT(num, "{\"prompt\":\"root@esp32:" + currentPath + "# \",\"output\":\"ul: Protocol error\\n\"}");
    return;
  }

  executeCommand(msg, output, newPrompt, clearTerminal, dlFileName, dlContent, triggerUpload);
  String resp = "{\"prompt\":\"" + escapeJson(newPrompt) + "\",";
  if (!output.isEmpty()) resp += "\"output\":\"" + escapeJson(output) + "\",";
  resp += "\"clear\":" + String(clearTerminal ? "true" : "false");
  if (!dlFileName.isEmpty()) resp += ",\"download\":{\"filename\":\"" + escapeJson(dlFileName) + "\",\"content\":\"" + escapeJson(dlContent) + "\"}";
  if (triggerUpload) resp += ",\"upload\":true";
  resp += "}";
  webSocket.sendTXT(num, resp);
}

// ========== 串口输入处理 ==========
void processSerialInput() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (c == '\r' && Serial.peek() == '\n') Serial.read();
      if (!serialInputBuffer.isEmpty()) {
        String cmd = serialInputBuffer;
        String output, newPrompt, dummyDlF, dummyDlC;
        bool clearTerminal, dummyUl;
        Serial.println();
        executeCommand(cmd, output, newPrompt, clearTerminal, dummyDlF, dummyDlC, dummyUl);
        if (clearTerminal)
          for (int i = 0; i < 20; i++) Serial.println();
        Serial.print(output);
        if (!isEditing) Serial.print(newPrompt);
        serialInputBuffer = "";
      } else {
        Serial.println();
        if (!isEditing) Serial.print("root@esp32:/# ");
      }
    } else if (c == 8) {
      if (!serialInputBuffer.isEmpty()) {
        serialInputBuffer.remove(serialInputBuffer.length() - 1);
        Serial.write(8);
        Serial.write(' ');
        Serial.write(8);
      }
    } else {
      Serial.write(c);
      serialInputBuffer += c;
    }
  }
}

// ========== Web服务器处理 ==========
void handleRoot() {
  webServer.send_P(200, "text/html", HTML_CONTENT);
}
void handleNotFound() {
  webServer.sendHeader("Location", "http://" + AP_IP.toString() + "/");
  webServer.send(302, "text/plain", "Redirecting...");
}

// ========== 初始化与主循环 ==========
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1000);
  Serial.println("\n\n-------------------------------");
  Serial.println("   ESP32-QINUX SYSTEM BOOT    ");
  Serial.println("-------------------------------");
  initAllGpioLow();
  if (!LittleFS.begin(true, "/littlefs", 5, "storage")) {
    Serial.println("LittleFS Failed!");
    return;
  }
  Serial.println("LittleFS OK.");
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP: ");
  Serial.println(AP_SSID);
  dnsServer.start(DNS_PORT, "*", AP_IP);
  webServer.on("/", handleRoot);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
  if (!LittleFS.exists("/etc")) LittleFS.mkdir("/etc");
  if (!LittleFS.exists("/home")) LittleFS.mkdir("/home");
  if (!LittleFS.exists("/sys")) LittleFS.mkdir("/sys");
  if (!LittleFS.exists("/bin")) LittleFS.mkdir("/bin");
  Serial.println("Qinux System Ready.");
  Serial.print("root@esp32:/# ");
  setWiFiTxPower(2);
}

void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();
  webSocket.loop();
  processSerialInput();
  checkChunkTimeout();

  if (chunkState.active && !chunkState.isUpload && !chunkState.sending) {
    sendNextChunk();
  }
}