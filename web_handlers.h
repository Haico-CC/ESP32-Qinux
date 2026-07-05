#ifndef ESP_WEB_HANDLERS_H
#define ESP_WEB_HANDLERS_H

#include <WebSocketsServer.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>

// ========== 全局对象 extern ==========
extern WebServer webServer;
extern WebSocketsServer webSocket;
extern DNSServer dnsServer;
extern String currentPath;
extern String serialInputBuffer;
extern bool isEditing;
extern String editingFilePath;
extern int g_lastExitCode;
extern uint8_t llmActiveClient;
extern bool llmGenerationActive;
extern std::map<uint8_t, String> clientEditFiles;

// ========== 前置声明 ==========
void executeCommand(String cmd, String& output, String& newPrompt, bool& clearTerminal, String& dlFileName, String& dlContent, bool& triggerUpload);

// ========== Web 编辑器启动 ==========
static void handleWebEdit(uint8_t num, const String& msg) {
  String pathArg = (msg == "edit") ? "" : msg.substring(5);
  pathArg.trim();
  if (pathArg.isEmpty()) {
    webSocket.sendTXT(num, "{\"output\":\"edit: Usage: edit <file>\\n\"}");
    return;
  }
  String fullPath = resolvePath(pathArg);
  if (isProtectedPath(fullPath)) {
    webSocket.sendTXT(num, "{\"output\":\"edit: Permission denied: protected file\\n\"}");
    return;
  }
  String fileContent = "";
  if (LittleFS.exists(fullPath)) {
    File f = LittleFS.open(fullPath, "r");
    if (f && !f.isDirectory()) {
      size_t fSize = f.size();
      if (fSize > 128 * 1024) {
        f.close();
        webSocket.sendTXT(num, "{\"output\":\"edit: File too large for editor (max 128KB, use dl to download)\\n\"}");
        return;
      }
      fileContent = f.readString();
      f.close();
    } else if (f) f.close();
  }
  clientEditFiles[num] = fullPath;
  String json = "{\"edit_start\":{\"filename\":\"" + escapeJson(pathArg) + "\",\"content\":\"" + escapeJson(fileContent) + "\"}}";
  webSocket.sendTXT(num, json);
}

// ========== WebSocket 事件处理 ==========
void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type != WStype_TEXT) return;
  llmActiveClient = num;
  String msg = (char*)payload;

  if (msg.startsWith("__DL_ACK__:")) {
    if (chunkState.active && !chunkState.isUpload && !chunkState.sending) sendNextChunk();
    return;
  }
  if (msg.startsWith("__UPLOAD_START__:")) {
    String args = msg.substring(17);
    String fname = ""; size_t fsize = 0; size_t cChunkSize = CHUNK_SIZE;
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
      handleUploadChunk(idx, args.substring(dataPos + 1), num);
    }
    return;
  }
  if (msg == "__UPLOAD_END__") {
    if (chunkState.active && chunkState.isUpload) {
      chunkState.file.flush(); chunkState.file.close(); chunkState.active = false;
    }
    return;
  }
  if (msg.startsWith("__EDIT_SAVE__:")) {
    String payload = msg.substring(14);
    int sep = payload.indexOf(':');
    String filename, b64Content;
    if (sep != -1) { filename = payload.substring(0, sep); b64Content = payload.substring(sep + 1); }
    else { if (clientEditFiles.count(num)) { filename = clientEditFiles[num]; b64Content = payload; } else { webSocket.sendTXT(num, "{\"output\":\"edit: Save failed: no active file\\n\"}"); return; } }
    String fullPath = resolvePath(filename);
    if (!canWritePath(fullPath)) { webSocket.sendTXT(num, "{\"output\":\"edit: Permission denied\\n\"}"); return; }
    std::vector<uint8_t> data = base64Decode(b64Content);
    File f = LittleFS.open(fullPath, "w");
    if (!f || f.isDirectory()) { if (f) f.close(); webSocket.sendTXT(num, "{\"output\":\"edit: Cannot write file\\n\"}"); return; }
    size_t written = f.write(data.data(), data.size()); f.close();
    clientEditFiles.erase(num);
    webSocket.sendTXT(num, "{\"output\":\"edit: File saved: " + escapeJson(filename) + " (" + String(written) + " bytes)\\n\"}");
    return;
  }
  if (msg == "edit" || msg.startsWith("edit ")) { handleWebEdit(num, msg); return; }

  if (llmGenerationActive && !msg.startsWith("llama status")) {
    webSocket.sendTXT(num, "{\"output\":\"llama: Busy generating.\\n\",\"prompt\":\"root@esp32:" + currentPath + "# \"}");
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
      if (max(lastSlash, lastBackslash) != -1) filename = filename.substring(max(lastSlash, lastBackslash) + 1);
      filename.replace("/", ""); filename.replace("\\", ""); filename.replace("..", "");
      if (filename.isEmpty()) filename = "upload_" + String(millis()) + ".bin";
      String fullPath = resolvePath(filename);
      if (!canWritePath(fullPath)) { webSocket.sendTXT(num, "{\"prompt\":\"root@esp32:" + currentPath + "# \",\"output\":\"ul: Permission denied\\n\"}"); return; }
      std::vector<uint8_t> binData = base64Decode(msg.substring(firstSep + 1));
      int lastSlashPos = fullPath.lastIndexOf('/');
      if (lastSlashPos > 0 && !LittleFS.exists(fullPath.substring(0, lastSlashPos))) LittleFS.mkdir(fullPath.substring(0, lastSlashPos));
      File f = LittleFS.open(fullPath, "w");
      if (f && !f.isDirectory()) {
        size_t written = f.write(binData.data(), binData.size()); f.close();
        output = (written == binData.size()) ? "ul: Saved '" + filename + "' (" + String(written) + " bytes)\n" : "ul: Write incomplete: " + String(written) + "/" + String(binData.size()) + "\n";
      } else { if (f) f.close(); output = "ul: Failed to open: " + fullPath + "\n"; }
      webSocket.sendTXT(num, "{\"prompt\":\"root@esp32:" + currentPath + "# \",\"output\":\"" + escapeJson(output) + "\"}");
      return;
    }
    webSocket.sendTXT(num, "{\"prompt\":\"root@esp32:" + currentPath + "# \",\"output\":\"ul: Protocol error\\n\"}");
    return;
  }

  wifiCmdClientNum = num;
  executeCommand(msg, output, newPrompt, clearTerminal, dlFileName, dlContent, triggerUpload);
  wifiCmdClientNum = 255;
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
        if (clearTerminal) for (int i = 0; i < 20; i++) Serial.println();
        Serial.print(output);
        if (!isEditing) Serial.print(newPrompt);
        serialInputBuffer = "";
      } else {
        Serial.println();
        if (!isEditing) Serial.print("root@esp32:/# ");
      }
    } else if (c == 8) {
      if (!serialInputBuffer.isEmpty()) { serialInputBuffer.remove(serialInputBuffer.length() - 1); Serial.write(8); Serial.write(' '); Serial.write(8); }
    } else { Serial.write(c); serialInputBuffer += c; }
  }
}

// ========== Web 服务器处理 ==========
void handleRoot() { webServer.send_P(200, "text/html", HTML_CONTENT); }
void handleNotFound() { webServer.sendHeader("Location", "http://" + AP_IP.toString() + "/"); webServer.send(302, "text/plain", "Redirecting..."); }

#endif
