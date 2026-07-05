#ifndef ESP_LLM_COMMANDS_H
#define ESP_LLM_COMMANDS_H

#include <Arduino.h>
#include <WebSocketsServer.h>
#include "llm.h"

extern WebSocketsServer webSocket;
extern bool llmGenerationActive;

void on_llm_token(const char* token, void* user_data) {
  uint8_t* client_id = (uint8_t*)user_data;
  if (*client_id < 255) {
    String json = "{\"output\":\"";
    const char* p = token;
    while (*p) {
      switch (*p) {
        case '\\': json += "\\\\"; break;
        case '"':  json += "\\\""; break;
        case '\n': json += "\\n"; break;
        case '\r': json += "\\r"; break;
        default:   json += *p; break;
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

#endif
