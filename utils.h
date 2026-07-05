#ifndef ESP_UTILS_H
#define ESP_UTILS_H

#include <Arduino.h>
#include <vector>
#include <LittleFS.h>

// ========== 全局变量 extern 声明 ==========
extern String currentPath;

// ========== Base64 编解码 ==========
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

static inline bool is_base64(uint8_t c) { return (isalnum(c) || (c == '+') || (c == '/')); }

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

// ========== 路径处理 ==========
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
      if (part == "" || part == ".") { start = i + 1; continue; }
      if (part == "..") { if (!parts.empty()) parts.pop_back(); }
      else parts.push_back(part);
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

// ========== JSON 转义 ==========
String escapeJson(String str) {
  String result = "";
  for (size_t i = 0; i < str.length(); i++) {
    char c = str[i];
    if (c == '\\') result += "\\\\";
    else if (c == '"') result += "\\\"";
    else if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else if ((unsigned char)c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
      result += buf;
    }
    else result += c;
  }
  return result;
}

// ========== 受保护目录 ==========
bool isProtectedPath(const String& path) {
  String resolved = resolvePath(path);
  for (int i = 0; i < PROTECTED_COUNT; i++)
    if (resolved == PROTECTED_DIRS[i] || resolved.startsWith(String(PROTECTED_DIRS[i]) + "/")) return true;
  return false;
}

bool canWritePath(const String& path) { return !isProtectedPath(path); }

// ========== 时间处理 ==========
void setSystemTime(time_t epochSecs) {
  struct timeval tv = { epochSecs, 0 };
  settimeofday(&tv, NULL);
}

bool parseDateTimeString(String dateStr, time_t& outTime) {
  int year, month, day, hour, minute, second;
  if (sscanf(dateStr.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
    struct tm timeinfo = {};
    timeinfo.tm_year = year - 1900; timeinfo.tm_mon = month - 1; timeinfo.tm_mday = day;
    timeinfo.tm_hour = hour; timeinfo.tm_min = minute; timeinfo.tm_sec = second;
    outTime = mktime(&timeinfo);
    return true;
  }
  return false;
}

// ========== 目录递归删除 ==========
bool deleteDirectoryRecursive(String path) {
  if (path == "/" || path == "") return false;
  if (isProtectedPath(path)) return false;
  File dir = LittleFS.open(path, "r");
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return false; }
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
    if (!isProtectedPath(itemPath)) items.push_back(itemPath);
    item.close();
    item = dir.openNextFile();
  }
  dir.close();
  bool success = true;
  for (const String& itemPath : items) {
    File check = LittleFS.open(itemPath, "r");
    bool isDir = (check && check.isDirectory());
    if (check) check.close();
    if (isDir) { if (!deleteDirectoryRecursive(itemPath)) success = false; }
    else { if (!LittleFS.remove(itemPath)) success = false; }
    yield();
  }
  if (success && !LittleFS.rmdir(path)) success = false;
  return success;
}

#endif
