#ifndef ESP_CHUNK_TRANSFER_H
#define ESP_CHUNK_TRANSFER_H

#include <LittleFS.h>
#include <vector>

// ========== 分块传输状态 ==========
extern WebSocketsServer webSocket;

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
extern ChunkTransferState chunkState;

// ========== 分块下载 ==========
bool startChunkedDownload(const String& filePath, uint8_t clientNum) {
  if (!LittleFS.exists(filePath)) return false;
  File f = LittleFS.open(filePath, "r");
  if (!f || f.isDirectory()) { if (f) f.close(); return false; }
  size_t fSize = f.size();
  if (fSize > MAX_FILE_SIZE) {
    f.close();
    webSocket.sendTXT(clientNum, "{\"output\":\"dl: File too large (max " + String(MAX_FILE_SIZE/1024) + "KB)\\n\"}");
    return false;
  }
  chunkState.active = true; chunkState.isUpload = false; chunkState.sending = false;
  chunkState.lastPrintedProgress = -1;
  chunkState.filename = filePath.substring(filePath.lastIndexOf('/') + 1);
  chunkState.totalSize = fSize;
  chunkState.totalChunks = (fSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
  chunkState.currentChunk = 0; chunkState.file = f;
  chunkState.clientNum = clientNum; chunkState.lastChunkTime = millis();
  String header = "{\"dl_start\":{\"filename\":\"" + escapeJson(chunkState.filename) + "\",\"size\":" + String(fSize) + ",\"chunks\":" + String(chunkState.totalChunks) + ",\"chunk_size\":" + String(CHUNK_SIZE) + "}}";
  webSocket.sendTXT(clientNum, header);
  return true;
}

void sendNextChunk() {
  if (!chunkState.active || chunkState.isUpload || chunkState.sending) return;
  if (chunkState.currentChunk >= chunkState.totalChunks) {
    webSocket.sendTXT(chunkState.clientNum, "{\"dl_end\":{\"status\":\"complete\",\"checksum\":\"ok\"}}");
    chunkState.file.close(); chunkState.active = false; chunkState.sending = false;
    Serial.println("dl: Transfer complete: " + chunkState.filename);
    return;
  }
  chunkState.sending = true;
  size_t offset = chunkState.currentChunk * CHUNK_SIZE;
  size_t readSize = min(CHUNK_SIZE, chunkState.totalSize - offset);
  chunkState.file.seek(offset);
  std::vector<uint8_t> buffer(readSize);
  size_t actuallyRead = chunkState.file.read(buffer.data(), readSize);
  if (actuallyRead != readSize)
    Serial.printf("dl: Warning: read %d/%d bytes at offset %d\n", actuallyRead, readSize, offset);
  String b64 = base64Encode(buffer.data(), actuallyRead);
  String chunkMsg = "{\"dl_chunk\":{\"index\":" + String(chunkState.currentChunk) + ",\"data\":\"" + b64 + "\",\"size\":" + String(actuallyRead) + ",\"chunk_size\":" + String(CHUNK_SIZE) + ",\"progress\":" + String((chunkState.currentChunk + 1) * 100 / chunkState.totalChunks) + "}}";
  bool sent = webSocket.sendTXT(chunkState.clientNum, chunkMsg);
  if (sent) {
    chunkState.currentChunk++; chunkState.lastChunkTime = millis();
    int progress = chunkState.currentChunk * 100 / chunkState.totalChunks;
    if (progress % 10 == 0 && progress != chunkState.lastPrintedProgress) {
      Serial.printf("dl: %s - %d%%\n", chunkState.filename.c_str(), progress);
      chunkState.lastPrintedProgress = progress;
    }
  }
  chunkState.sending = false;
  yield();
}

// ========== 分块上传 ==========
bool handleUploadStart(const String& filename, size_t totalSize, size_t clientChunkSize, uint8_t clientNum) {
  String safeName = filename;
  safeName.replace("/", ""); safeName.replace("\\", ""); safeName.replace("..", "");
  if (safeName.isEmpty()) safeName = "upload_" + String(millis()) + ".bin";
  String fullPath = resolvePath(safeName);
  if (!canWritePath(fullPath)) {
    webSocket.sendTXT(clientNum, "{\"ul_ack\":{\"status\":\"error\",\"msg\":\"Permission denied\"}}");
    return false;
  }
  if (totalSize > MAX_FILE_SIZE) {
    webSocket.sendTXT(clientNum, "{\"ul_ack\":{\"status\":\"error\",\"msg\":\"File too large\"}}");
    return false;
  }
  int lastSlash = fullPath.lastIndexOf('/');
  if (lastSlash > 0 && !LittleFS.exists(fullPath.substring(0, lastSlash)))
    LittleFS.mkdir(fullPath.substring(0, lastSlash));
  chunkState.active = true; chunkState.isUpload = true; chunkState.sending = false;
  chunkState.lastPrintedProgress = -1; chunkState.filename = fullPath;
  chunkState.totalSize = totalSize;
  chunkState.clientChunkSize = (clientChunkSize > 0) ? clientChunkSize : CHUNK_SIZE;
  chunkState.totalChunks = (totalSize + chunkState.clientChunkSize - 1) / chunkState.clientChunkSize;
  chunkState.currentChunk = 0;
  chunkState.file = LittleFS.open(fullPath, "w");
  chunkState.clientNum = clientNum; chunkState.lastChunkTime = millis();
  if (!chunkState.file) { chunkState.active = false; webSocket.sendTXT(clientNum, "{\"ul_ack\":{\"status\":\"error\",\"msg\":\"Cannot create file\"}}"); return false; }
  webSocket.sendTXT(clientNum, "{\"ul_ack\":{\"status\":\"ready\",\"chunks\":" + String(chunkState.totalChunks) + "}}");
  return true;
}

bool handleUploadChunk(size_t index, const String& b64Data, uint8_t clientNum) {
  if (!chunkState.active || !chunkState.isUpload || index != chunkState.currentChunk) return false;
  std::vector<uint8_t> binData = base64Decode(b64Data);
  if (binData.empty() && !b64Data.isEmpty()) return false;
  size_t offset = index * chunkState.clientChunkSize;
  if (!chunkState.file.seek(offset)) return false;
  size_t written = chunkState.file.write(binData.data(), binData.size());
  if (written != binData.size()) {
    size_t remaining = binData.size() - written;
    size_t written2 = chunkState.file.write(binData.data() + written, remaining);
    if (written2 != remaining) return false;
  }
  if (chunkState.currentChunk % 8 == 0) chunkState.file.flush();
  chunkState.currentChunk++; chunkState.lastChunkTime = millis();
  int progress = chunkState.currentChunk * 100 / chunkState.totalChunks;
  webSocket.sendTXT(clientNum, "{\"ul_ack\":{\"index\":" + String(index) + ",\"received\":" + String(chunkState.currentChunk * chunkState.clientChunkSize) + ",\"progress\":" + String(progress) + "}}");
  if (chunkState.currentChunk >= chunkState.totalChunks) {
    chunkState.file.flush(); chunkState.file.close();
    File verify = LittleFS.open(chunkState.filename, "r");
    if (verify) { size_t actualSize = verify.size(); verify.close(); }
    webSocket.sendTXT(clientNum, "{\"ul_ack\":{\"status\":\"complete\",\"written\":" + String(chunkState.totalSize) + "}}");
    Serial.printf("ul: Saved '%s' (%d bytes, %d chunks)\n", chunkState.filename.c_str(), chunkState.totalSize, chunkState.totalChunks);
    chunkState.active = false; return true;
  }
  return true;
}

void checkChunkTimeout() {
  if (!chunkState.active) return;
  if (millis() - chunkState.lastChunkTime > CHUNK_TIMEOUT) {
    if (!chunkState.isUpload && chunkState.clientNum != 255)
      webSocket.sendTXT(chunkState.clientNum, "{\"output\":\"dl: Transfer timeout\\n\"}");
    if (chunkState.file) { chunkState.file.flush(); chunkState.file.close(); }
    chunkState.active = false;
  }
}

#endif
