#ifndef ESP_HARDWARE_H
#define ESP_HARDWARE_H

#include <Arduino.h>

// ========== 系统信息 ==========
String getResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    case ESP_RST_POWERON:  return "Power On";
    case ESP_RST_EXT:      return "External Pin";
    case ESP_RST_SW:       return "Software Reset";
    case ESP_RST_PANIC:    return "Exception/Panic";
    case ESP_RST_INT_WDT:  return "Interrupt WDT";
    case ESP_RST_TASK_WDT: return "Task WDT";
    case ESP_RST_WDT:      return "Other WDT";
    case ESP_RST_DEEPSLEEP:return "Deep Sleep Wakeup";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "Unknown";
  }
}

// ========== GPIO 安全校验 ==========
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
  // 启动时将所有安全 GPIO 设为高阻输入（最安全状态）
  for (int pin = 0; pin <= 48; pin++) {
    if (isSafeGpio(pin)) { pinMode(pin, INPUT); }
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

// ========== ADC 引脚校验 ==========
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

#endif
