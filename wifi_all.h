#ifndef ESP_WIFI_ALL_H
#define ESP_WIFI_ALL_H

#include <WiFi.h>
#include <LittleFS.h>

// ========== 全局变量 extern ==========
extern String staSsid, staPass;
extern bool   staConnecting, staConnected;
extern unsigned long staConnectStart;
extern uint8_t wifiCmdClientNum;
extern WebSocketsServer webSocket;

// ========== 状态显示辅助 ==========
String getWiFiModeStr(wifi_mode_t mode) {
  switch (mode) {
    case WIFI_OFF: return "OFF";
    case WIFI_STA: return "STA";
    case WIFI_AP:  return "AP";
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

// ========== WiFi 功率控制 ==========
#if ESP_ARDUINO_VERSION_MAJOR >= 3
wifi_power_t dbmToWifiPower(int dbm) {
  return static_cast<wifi_power_t>(constrain((int)((float)dbm * 4.0f + 0.5f), -4, 84));
}
float wifiPowerToDbm(wifi_power_t power) { return (float)power / 4.0f; }
int getWiFiTxPowerDbm() { return (int)(wifiPowerToDbm(WiFi.getTxPower()) + 0.5f); }
#endif

bool setWiFiTxPower(int dbm) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  dbm = constrain(dbm, -1, 21);
  return WiFi.setTxPower(dbmToWifiPower(dbm));
#else
  dbm = constrain(dbm, -20, 20);
  return WiFi.setTxPower(dbm);
#endif
}

// ========== 配网配置持久化 ==========
bool loadWifiConfig(String& ssid, String& pass) {
  if (!LittleFS.exists(WIFI_CFG_PATH)) return false;
  File f = LittleFS.open(WIFI_CFG_PATH, "r");
  if (!f) return false;
  String content = f.readString();
  f.close();
  // 不能用 content.trim()——会把换行分隔符吃掉！
  int sep = content.indexOf('\n');
  if (sep == -1) return false;
  ssid = content.substring(0, sep);
  pass = content.substring(sep + 1);
  ssid.trim(); pass.trim();
  return !ssid.isEmpty();
}

void saveWifiConfig(const String& ssid, const String& pass) {
  // 确保 /sys 目录存在
  if (!LittleFS.exists("/sys")) LittleFS.mkdir("/sys");
  File f = LittleFS.open(WIFI_CFG_PATH, "w");
  if (!f) return;
  f.print(ssid + "\n" + pass + "\n");
  f.flush();
  f.close();
}

void clearWifiConfig() {
  if (LittleFS.exists(WIFI_CFG_PATH)) LittleFS.remove(WIFI_CFG_PATH);
}

bool tryAutoConnect(unsigned long timeoutMs = 5000) {
  String savedSsid, savedPass;
  if (!loadWifiConfig(savedSsid, savedPass)) return false;
  Serial.printf("WiFi STA: Connecting to \"%s\"...\n", savedSsid.c_str());
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  staSsid = savedSsid; staPass = savedPass;
  staConnecting = true; staConnectStart = millis();
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
      staConnected = true; staConnecting = false;
      Serial.println("WiFi STA: Connected! IP: " + WiFi.localIP().toString());
      return true;
    }
    delay(200);
  }
  WiFi.disconnect(true);
  staConnecting = false; staConnected = false;
  Serial.printf("WiFi STA: Timeout (%ds), AP-only mode\n", (int)(timeoutMs/1000));
  return false;
}

// ========== WiFi 命令处理 ==========
void handleWifiCommand(String args, String& output);
// 实现见下方（因依赖 handleWifiCommand 内联细节，保留在此头文件）

void handleWifiCommand(String args, String& output) {
  args.trim();
  if (args.isEmpty() || args == "status") {
    output += "=== WiFi Status ===\n";
    output += "wifi: Mode:       " + getWiFiModeStr(WiFi.getMode()) + "\n";
    output += "wifi: Channel:    " + String(WiFi.channel()) + "\n";
    output += "wifi: MAC:        " + WiFi.macAddress() + "\n";
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    output += "wifi: Tx Power:   " + String(getWiFiTxPowerDbm()) + " dBm\n";
#else
    output += "wifi: Tx Power:   " + String(WiFi.getTxPower()) + " dBm\n";
#endif
    output += "\n";
    if (WiFi.getMode() & WIFI_AP) {
      output += "--- AP (Hotspot) ---\n";
      output += "  SSID:           " + WiFi.softAPSSID() + "\n";
      output += "  IP:             " + WiFi.softAPIP().toString() + "\n";
      output += "  Clients:        " + String(WiFi.softAPgetStationNum()) + "\n";
    }
    if (WiFi.getMode() & WIFI_STA) {
      output += "--- STA (Client) ---\n";
      if (staConnected || WiFi.status() == WL_CONNECTED) {
        output += "  Status:         Connected\n";
        output += "  SSID:           " + WiFi.SSID() + "\n";
        output += "  IP:             " + WiFi.localIP().toString() + "\n";
        output += "  RSSI:           " + String(WiFi.RSSI()) + " dBm\n";
      } else if (staConnecting) {
        output += "  Status:         Connecting... (" + String((millis()-staConnectStart)/1000) + "s)\n";
      } else if (LittleFS.exists(WIFI_CFG_PATH)) {
        String savedSsid, savedPass;
        loadWifiConfig(savedSsid, savedPass);
        output += "  Status:         Disconnected (saved: " + savedSsid + ")\n";
      } else {
        output += "  Status:         Disconnected (no saved network)\n";
      }
    }
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
    output += "Min Rate:        N/A\nMax Rate:        N/A\n";
#endif
    output += "MAC Address:     " + WiFi.macAddress() + "\n";
    output += "Channel:         " + String(WiFi.channel()) + "\n";
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
        char buf[80];
        snprintf(buf, sizeof(buf), "wifi: %4d | %2d | %-10s | %s\n", WiFi.RSSI(i), WiFi.channel(i), getAuthModeStr(WiFi.encryptionType(i)).c_str(), WiFi.SSID(i).c_str());
        output += buf; yield();
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
    String quality = (rssi >= -50) ? "Excellent" : (rssi >= -67) ? "Good" : (rssi >= -78) ? "Fair" : (rssi >= -90) ? "Poor" : "No Signal";
    output += "Signal Quality:  " + quality + "\n";
#if ESP_ARDUINO_VERSION_MAJOR < 3
    output += "Current Rate:    " + String(WiFi.getPhyMode()) + "\n";
    output += "Min Rate:        " + String(WiFi.getMinRate() / 2) + " Mbps\n";
    output += "Max Rate:        " + String(WiFi.getMaxRate() / 2) + " Mbps\n";
#else
    output += "Current Rate:    N/A (Core 3.x)\nMin Rate:        N/A\nMax Rate:        N/A\n";
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
    if (power < 2 || power > 20) { output = "wifi: Error: Power must be 2 ~ 20 dBm (Core 3.x)\n"; return; }
#else
    if (power < -20 || power > 20) { output = "wifi: Error: Power must be -20 ~ 20 dBm (Core 2.x)\n"; return; }
#endif
    if (setWiFiTxPower(power)) {
      output = "wifi: Tx power set to " + String(power) + " dBm\n";
      if (WiFi.getMode() & WIFI_STA) output += "wifi: Note: In 802.11n mode, max ~15dBm may apply\n";
    } else output = "wifi: Failed to set power (check WiFi mode/state)\n";
    return;
  }
  // ---- STA 连接 ----
  if (args == "disconnect") {
    if (!(WiFi.getMode() & WIFI_STA)) { output = "wifi: STA mode not active\n"; return; }
    WiFi.disconnect(true);
    staConnected = false; staConnecting = false;
    output = "wifi: Disconnected. Saved config preserved.\n";
    return;
  }
  if (args.startsWith("connect ")) {
    String params = args.substring(8); params.trim();
    String ssidArg, passArg;
    if (params.startsWith("\"")) {
      int endQuote = params.indexOf('"', 1);
      if (endQuote == -1) { output = "wifi: Error: Unclosed quote in SSID\n"; return; }
      ssidArg = params.substring(1, endQuote);
      passArg = params.substring(endQuote + 1);
    } else {
      int spacePos = params.indexOf(' ');
      if (spacePos == -1) { ssidArg = params; passArg = ""; }
      else { ssidArg = params.substring(0, spacePos); passArg = params.substring(spacePos + 1); }
    }
    ssidArg.trim(); passArg.trim();
    if (ssidArg.isEmpty()) { output = "wifi: Error: SSID cannot be empty\n"; return; }
    output += passArg.isEmpty() ? "wifi: Connecting to \"" + ssidArg + "\" (open network)...\n" : "wifi: Connecting to \"" + ssidArg + "\"...\n";
    WiFi.disconnect(false); delay(100);
    WiFi.begin(ssidArg.c_str(), passArg.c_str());
    staSsid = ssidArg; staPass = passArg;
    staConnecting = true; staConnected = false;
    staConnectStart = millis();
    unsigned long connStart = millis();
    int lastReportedSec = -1;
    while (millis() - connStart < STA_CONNECT_TIMEOUT) {
      if (WiFi.status() == WL_CONNECTED) {
        staConnected = true; staConnecting = false;
        if (wifiCmdClientNum != 255) webSocket.sendTXT(wifiCmdClientNum, "{\"output\":\"wifi: Connected!\\n\"}");
        output += "wifi: Connected!\nwifi: IP: " + WiFi.localIP().toString() + "\n";
        output += "wifi: RSSI: " + String(WiFi.RSSI()) + " dBm\n";
        saveWifiConfig(ssidArg, passArg);
        output += "wifi: Config saved to " + String(WIFI_CFG_PATH) + "\n";
        return;
      }
      unsigned long elapsed = millis() - connStart;
      int curSec = elapsed / 1000;
      if (curSec != lastReportedSec && curSec > 0 && wifiCmdClientNum != 255) {
        lastReportedSec = curSec;
        String prog = "wifi: Connecting... (" + String(curSec) + "s)\n";
        webSocket.sendTXT(wifiCmdClientNum, "{\"output\":\"" + escapeJson(prog) + "\"}");
      }
      delay(200);
    }
    WiFi.disconnect(true); staConnecting = false; staConnected = false;
    if (wifiCmdClientNum != 255) webSocket.sendTXT(wifiCmdClientNum, "{\"output\":\"wifi: Connection timeout (5s). Check SSID/password.\\n\"}");
    output = "wifi: Connection timeout (5s). Check SSID/password.\n";
    return;
  }
  if (args == "forget") {
    if (LittleFS.exists(WIFI_CFG_PATH)) {
      clearWifiConfig();
      if (WiFi.getMode() & WIFI_STA) WiFi.disconnect(true);
      staConnected = false; staConnecting = false;
      output = "wifi: Saved config cleared.\n";
    } else output = "wifi: No saved config to forget.\n";
    return;
  }
  output += "WiFi Commands:\n";
  output += "  wifi [status]            - Show WiFi status (AP + STA)\n";
  output += "  wifi info                - Display detailed configuration\n";
  output += "  wifi scan                - Scan nearby wireless networks\n";
  output += "  wifi connect <SSID> [PW] - Connect to WiFi (omit PW for open)\n";
  output += "  wifi disconnect           - Disconnect from WiFi (keep config)\n";
  output += "  wifi forget               - Forget saved WiFi config\n";
  output += "  wifi stats                - Signal quality & rate info\n";
  output += "  wifi set power <dBm>      - Adjust TX power (2~20)\n";
}

#endif
