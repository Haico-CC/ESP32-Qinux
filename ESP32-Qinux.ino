// ============================================================
//  ESP32-Qinux — Retro CRT Terminal System
//  Architecture: header-only modules for Arduino IDE compatibility
// ============================================================

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
#include "llm.h"

// ========== 系统常量 ==========
const char*     AP_SSID     = "Welcome Back to the 80's !";
const char*     AP_PASS     = "";
const byte      DNS_PORT    = 53;
IPAddress       AP_IP(192, 168, 4, 1);
const unsigned long SERIAL_BAUD = 115200;
const size_t    CHUNK_SIZE     = 4096;
const size_t    MAX_FILE_SIZE  = 5 * 1024 * 1024;
const uint32_t  CHUNK_TIMEOUT  = 8000;
const char*     PROTECTED_DIRS[] = { "/bin", "/etc", "/sys" };
const int       PROTECTED_COUNT = sizeof(PROTECTED_DIRS) / sizeof(PROTECTED_DIRS[0]);
const unsigned long STA_CONNECT_TIMEOUT = 5000;
const char*     WIFI_CFG_PATH  = "/sys/wifi.cfg";
const char*     LLM_MODEL_PATH = "/bin/stories260K.bin";
const char*     LLM_TOKEN_PATH = "/bin/tok512.bin";

#include "utils.h"
#include "hardware.h"
#include "wifi_all.h"
#include "script_engine.h"
#include "chunk_transfer.h"
// ========== LLM Bridge 接口声明（必须在 commands.h 之前）==========
bool llm_bridge_init(const char*, const char*);
bool llm_bridge_generate(const char*, int, llm_token_cb, generated_complete_cb, void*);
bool llm_bridge_is_ready();
bool llm_bridge_is_busy();
void llm_bridge_free();

#include "llm_commands.h"
#include "commands.h"
#include "web_ui.h"
#include "web_handlers.h"

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

// ========== WiFi STA 配网状态 ==========
String staSsid = "";
String staPass = "";
bool staConnecting = false;
bool staConnected = false;
unsigned long staConnectStart = 0;
uint8_t wifiCmdClientNum = 255;

// ========== LLM 全局状态 ==========
uint8_t llmActiveClient = 255;
bool llmGenerationActive = false;

// ========== 分块传输状态 ==========
ChunkTransferState chunkState;

// ========== Web 编辑器映射 ==========
std::map<uint8_t, String> clientEditFiles;

// ========== 脚本执行上下文 ==========
std::map<String, String> scriptVars;
std::vector<String> scriptInputLines;
int scriptInputIndex = 0;
bool scriptWaitingInput = false;
String scriptInputVarName = "";

// ========== 初始化 ==========
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
  WiFi.mode(WIFI_AP_STA);
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
  if (LittleFS.exists(WIFI_CFG_PATH)) {
    Serial.println("WiFi STA: Saved config found, auto-connecting...");
    tryAutoConnect(STA_CONNECT_TIMEOUT);
  } else {
    Serial.println("WiFi STA: No saved config, use 'wifi connect' to set up");
  }
  Serial.println("Qinux System Ready.");
  Serial.print("root@esp32:/# ");
  setWiFiTxPower(2);
}

// ========== 主循环 ==========
void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();
  webSocket.loop();
  processSerialInput();
  checkChunkTimeout();
}
